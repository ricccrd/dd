// dd/runtime/frontend/x86_64 -- the dispatcher loop (translate-on-miss; service syscalls).

// ---- W6A item 3: SMC (self-modifying code) for in-process JIT guests ----  gate NOSMC=1
// Once a guest takes a PROT_EXEC (RWX) mmap (g_rwx_guest, set in os/linux/service.c), it may overwrite
// code it already executed (and we translated+cached). We can't see the write otherwise, so after
// translating a block we mprotect its source 16KB page READ-ONLY; a guest write then traps in
// jit86_lazyguard (frontend/x86_64/elf.c), which calls smc_on_write() to unprotect the page + drop the
// stale translations (g_map/g_ibtc), so the modified bytes re-translate. Entirely inert unless
// g_rwx_guest is set (smc_protect returns immediately) -> zero effect on the normal (non-JIT) matrix.
// g_rwx_guest is defined in service.c, included before this TU in the x86 unity build.
extern int g_rwx_guest;
#define SMC_MAX 8192
static uint64_t g_smc_pg[SMC_MAX];
static int g_smc_n;
static uint64_t g_smc_flushes; // PROF: number of SMC re-translate events
static int g_nosmc = -1;
static void smc_protect(uint64_t pc) {
    if (!g_rwx_guest) return; // no JIT guest -> inert (matrix bit-exact)
    if (g_nosmc < 0) g_nosmc = (getenv("NOSMC") != NULL);
    if (g_nosmc) return;
    uint64_t pg = pc & ~0x3FFFull; // 16KB macOS hardware page
    for (int i = 0; i < g_smc_n; i++)
        if (g_smc_pg[i] == pg) return; // already protected
    if (mprotect((void *)pg, 0x4000, PROT_READ) != 0) return; // code page -> read-only; writes trap
    if (g_smc_n < SMC_MAX) g_smc_pg[g_smc_n++] = pg;
}
// If `a` falls in a protected SMC page, unprotect+forget it and return 1 (caller drops translations).
// Re-protected the next time the page is translated. Called from jit86_lazyguard (elf.c, after this TU).
static int smc_on_write(uint64_t a) {
    if (!g_rwx_guest) return 0;
    uint64_t pg = a & ~0x3FFFull;
    for (int i = 0; i < g_smc_n; i++)
        if (g_smc_pg[i] == pg) {
            mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE); // let the guest's write through
            g_smc_pg[i] = g_smc_pg[--g_smc_n];                    // re-protected on next translate
            g_smc_flushes++;
            return 1;
        }
    return 0;
}

// ---------------- dispatcher ----------------
static uint64_t g_prevpc, g_curpc; // debug: track block transitions for fault diagnosis
static void run_guest(struct cpu *c) {
    pthread_setspecific(g_cpu_key, c);
    while (!c->exited) {
        if (c->rip == SIGRETURN_PC) do_sigreturn(c); // handler returned -> restore interrupted context
        if (g_pending) maybe_deliver_signal(c);      // async signal pending -> redirect to guest handler
        // W6A item 1: a non-PIE ET_EXEC's un-relocated absolute CODE jump lands on its original low link
        // vaddr (unmapped/__PAGEZERO); redirect into the biased image so we translate the real code.
        // g_nonpie_lo is 0 for PIE/static-PIE (set only for ET_EXEC in load_elf) -> inert otherwise.
        // Mirrors the aarch64 engine's jit/dispatch.c redirect; coexists with the W5-B R_TIER2 hook below.
        if (g_nonpie_lo && c->rip >= g_nonpie_lo && c->rip < g_nonpie_hi) c->rip += g_nonpie_bias;
        g_prevpc = g_curpc;
        g_curpc = c->rip;
        g_disp_n++;
        if (g_trace && g_tracecap && g_disp_n > g_tracecap) { // bound trace output for runaway guests
            fprintf(stderr, "[jit86] trace cap %llu blocks reached -> stop\n", (unsigned long long)g_tracecap);
            c->exited = 1;
            c->exit_code = 99;
            break;
        }
        if (g_nochain && g_loadbase && c->rip == g_loadbase + 0x2ee0) g_malloc_n++; // count __libc_malloc_impl entries
        if (g_nochain && g_loadbase) {
            uint64_t po =
                g_prevpc - g_loadbase; // __libc_malloc_impl first-handout: dump the new group's avail_mask (rbp=meta)
            if (po >= 0x32a0 && po <= 0x3340) {
                uint64_t rbp = c->r[5], rax = c->r[0];
                uint32_t avail = (rbp > 0x10000) ? *(uint32_t *)(rbp + 0x1c) : 0;
                fprintf(stderr, "[av] blk+%llx handout=%llx meta(rbp)=%llx avail_mask[rbp+1c]=%x freed[rbp+18]=%x\n",
                        (unsigned long long)po, (unsigned long long)rax, (unsigned long long)rbp, avail,
                        (rbp > 0x10000) ? *(uint32_t *)(rbp + 0x18) : 0);
            }
        }
        if (g_w8 && *g_w8 != g_w8v) { // byte-watchpoint: report the block that just changed it
            fprintf(stderr, "[w8] @%p %02x -> %02x  by block +%llx  malloc#=%llu  rsi=%llx\n", (void *)g_w8, g_w8v,
                    *g_w8, (unsigned long long)(g_prevpc - g_loadbase), (unsigned long long)g_malloc_n,
                    (unsigned long long)c->r[6]);
            g_w8v = *g_w8;
        }
        // Cache mutation (translate / flush / chain / IBTC fill) is serialized under g_jit_lock once a
        // guest thread exists. Single-threaded skips the lock entirely. W^X is per-thread on Apple
        // Silicon, so a peer executing cached code is unaffected by this thread's write window.
        if (g_threaded) pthread_mutex_lock(&g_jit_lock);
        void *code = map_host(c->rip);
        if (!code) {
            if (g_cp + (1u << 16) > g_cache + CACHE_SZ) {
                if (g_threaded) { // can't flush while peers may be executing the cache we'd drop
                    fprintf(stderr, "[jit86] code cache full with threads (unsupported)\n");
                    _exit(70);
                }
                pthread_jit_write_protect_np(0);
                g_cp = g_cache;
                memset(g_map, 0, sizeof g_map);
                g_npend = 0;
                pthread_jit_write_protect_np(1);
                memset(g_ibtc, 0, sizeof g_ibtc); // body pointers now stale -> drop the cache
            }
            pthread_jit_write_protect_np(0);
            g_emit_start = g_cp;
            code = translate_block(c->rip);
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(g_emit_start, (size_t)(g_cp - g_emit_start));
            smc_protect(c->rip); // W6A item 3: guard this source page so a JIT guest's overwrite is seen
        }
        if (c->ic_miss) { // IBTC: an indirect branch missed -> cache {target -> body}
            // The IBTC probe in emitted code reads g_ibtc unlocked; a concurrent torn fill -> wrong body.
            // Skip the fill when threaded (indirect branches fall to the locked dispatcher: correct, slower).
            if (!g_threaded) {
                void *body = map_body(c->rip);
                if (body) {
                    uint32_t h = (uint32_t)((c->rip >> 2) & (IBTC_N - 1));
                    g_ibtc[h].target = c->rip;
                    g_ibtc[h].body = body;
                    g_ibtc_fill++;
                }
            }
            c->ic_miss = 0;
        }
        if (g_threaded) pthread_mutex_unlock(&g_jit_lock);
        if (g_trace) { // x86 flags derived from cpu->nzcv (convention: stored C = NOT x86 CF)
            unsigned nz = (unsigned)c->nzcv;
            int CF = !((nz >> 29) & 1), ZF = (nz >> 30) & 1, SF = (nz >> 31) & 1, OF = (nz >> 28) & 1;
            fprintf(stderr,
                    "[blk] rip=%llx rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx r8=%llx r9=%llx "
                    "r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx fl=C%dZ%dS%dO%d\n",
                    (unsigned long long)c->rip, (unsigned long long)c->r[RAX], (unsigned long long)c->r[3],
                    (unsigned long long)c->r[RCX], (unsigned long long)c->r[RDX], (unsigned long long)c->r[RSI],
                    (unsigned long long)c->r[RDI], (unsigned long long)c->r[RBP], (unsigned long long)c->r[8],
                    (unsigned long long)c->r[9], (unsigned long long)c->r[10], (unsigned long long)c->r[11],
                    (unsigned long long)c->r[12], (unsigned long long)c->r[13], (unsigned long long)c->r[14],
                    (unsigned long long)c->r[15], CF, ZF, SF, OF);
        }
        c->reason = 0;
        run_block(c, code);
        if (c->reason == 99) {
            fprintf(stderr, "[jit86] aborting at rip marker %llx (unimplemented opcode)\n", (unsigned long long)c->rip);
            if (g_trace) {
                for (int rr = 0; rr < 16; rr++) { // dump heap-pointer regs (meta etc.)
                    uint64_t v = c->r[rr];
                    if (v > 0x100000000ull && v < 0x200000000ull && (v & 7) == 0) {
                        fprintf(stderr, "  r%d=%llx:", rr, (unsigned long long)v);
                        for (int i = 0; i < 5; i++)
                            fprintf(stderr, " %016llx", (unsigned long long)((uint64_t *)v)[i]);
                        fprintf(stderr, "\n");
                    }
                }
            }
            c->exited = 1;
            c->exit_code = 70;
            break;
        }
        if (c->reason == R_TIER2) {
            // W5B: a hot self-loop's back-edge counter fired; recompile+swap it in. rip already = loop start.
            tier2_promote(c->rip);
            continue;
        }
        if (c->reason == R_CPUID) {
            do_cpuid(c);
            continue;
        } // rip already = next
        if (c->reason == R_X87FLD) {
            x87_fld_m80(c);
            continue;
        } // fld m80  (rip already = next)
        if (c->reason == R_X87FSTP) {
            x87_fstp_m80(c);
            continue;
        }                         // fstp m80
        if (c->reason == R_DIV) { // 128/64 unsigned div (rip already = next)
            uint64_t d = c->divop;
            if (d == 0) {
                fprintf(stderr, "[jit86] #DE divide-by-zero\n");
                c->exited = 1;
                c->exit_code = 136;
                break;
            }
            unsigned __int128 num = ((unsigned __int128)c->r[RDX] << 64) | c->r[RAX];
            c->r[RAX] = (uint64_t)(num / d);
            c->r[RDX] = (uint64_t)(num % d);
            continue;
        }
        if (c->reason == R_IDIV) { // 128/64 signed idiv
            int64_t d = (int64_t)c->divop;
            if (d == 0) {
                fprintf(stderr, "[jit86] #DE divide-by-zero\n");
                c->exited = 1;
                c->exit_code = 136;
                break;
            }
            __int128 num = ((__int128)(int64_t)c->r[RDX] << 64) | c->r[RAX];
            c->r[RAX] = (uint64_t)(num / d);
            c->r[RDX] = (uint64_t)(num % d);
            continue;
        }
        if (c->reason == R_SYSCALL) {
            service(c);
            if (c->exited) break;
            if (c->redirect) c->redirect = 0; /* else rip already = next (set at exit) */
        }
        // R_BRANCH: c->rip already holds the target
    }
}
