// dd/runtime/frontend/x86_64 -- the dispatcher loop (translate-on-miss; service syscalls).

// ---------------- dispatcher ----------------
static uint64_t g_prevpc, g_curpc; // debug: track block transitions for fault diagnosis
static void run_guest(struct cpu *c) {
    pthread_setspecific(g_cpu_key, c);
    while (!c->exited) {
        if (c->rip == SIGRETURN_PC) do_sigreturn(c); // handler returned -> restore interrupted context
        if (g_pending) maybe_deliver_signal(c);      // async signal pending -> redirect to guest handler
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
