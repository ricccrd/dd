// frontend/x86_64/dispatch_hooks.h -- the x86-64 frontend's definitions of the shared run_guest()
// dispatch seam (engine-dedup PR3/PR4). Mirror of frontend/aarch64/dispatch_hooks.h: the shared
// jit/dispatch.c calls these hooks at every spot where the dispatcher historically diverged per guest
// arch (see docs/design/engine-dedup.md §A.3/§B.3). Each macro reproduces EXACTLY what the standalone
// frontend/x86_64/dispatch.c did, so swapping the x86 target onto jit/dispatch.c is behavior-preserving.
//
// The macros are EXPANDED at their call sites inside jit/dispatch.c's run_guest() loop (not here), so
// `continue`/`break` reach that loop and the engine globals (g_ibtc/g_xibtc, map_body, g_ibtc_fill,
// do_cpuid/do_repstr/x87_*/tier2_promote, the R_* codes, ...) are in scope there even though this header
// is pulled in early (targets/linux_x86_64.c #includes it right after abi.h). Every name used in a macro
// body is defined earlier in the x86 unity TU (engine_glue.c, jit/cache.c, x86_ops.c, translate.c) — all
// included before jit/dispatch.c, where the macros expand.
//
// Hooks the shared loop expects (the four PR2 seams + the PR3/PR4 additions for opts committed after the
// design was written -- W6A SMC, opt2 2-way IBTC, the per-block trace dump, the ibtc_base entry setup):
//   G_OWN_TRAMPOLINES   x86 supplies its own run_block/block_return (translate.c) -> suppress the shared
//                       (aarch64) naked trampolines in jit/dispatch.c (different reg model: cpu pinned x28)
//   G_DISPATCH_ENTER    one-time per-thread setup before the loop (x86: publish the 2-way IBTC base)
//   G_DISPATCH_DEBUG    top-of-loop instrumentation (+ the x86 top-of-loop async-signal check)
//   G_SHADOW_CLEAR      wholesale-flush engine reset (x86: drop the 2-way IBTC; aarch64: shadow stack)
//   G_DISPATCH_CHAIN    post-translate chaining (x86: NO-OP -- translate_block already chained)
//   G_AFTER_TRANSLATE   post-translate per-arch step (x86: W6A SMC source-page write-protect)
//   G_TRACE_DUMP        per-block JT trace dump (x86 register/flag layout; the 5th divergence)
//   G_IBTC_FILL         IBTC miss fill (x86: 1-way/2-way, keyed on ic_miss, plain body)
//   G_DISPATCH_REASON   post-run_block reason handling (x86: cpuid/repstr/x87/div/idiv/tier2/syscall)

// ---- x86 dispatch support relocated out of the lifted dispatch.c -------------------------------------
// These DEFINITIONS used to sit at the top of frontend/x86_64/dispatch.c (above run_guest). The swap
// stops #include-ing that file, but elf.c (jit86_lazyguard / jit86_faulth) and the G_AFTER_TRANSLATE /
// G_DISPATCH_DEBUG hooks still need them, so they move here (the x86 dispatch seam). This header is
// #included exactly once in the x86 unity TU -> each is defined once. They reference only libc + the
// extern g_rwx_guest (defined later in os/linux/service.c) -> position-independent here.

// debug: track block transitions for fault diagnosis (extern'd by frontend/x86_64/elf.c).
static uint64_t g_prevpc, g_curpc;

// ---- W6A item 3: SMC (self-modifying code) for in-process JIT guests ----  gate NOSMC=1
// Once a guest takes a PROT_EXEC (RWX) mmap (g_rwx_guest, set in os/linux/service.c), it may overwrite
// code it already executed (and we translated+cached). After translating a block we mprotect its source
// 16KB page READ-ONLY; a guest write then traps in jit86_lazyguard (elf.c) -> smc_on_write() unprotects
// the page + drops the stale translations so the modified bytes re-translate. Entirely inert unless
// g_rwx_guest is set -> zero effect on the normal (non-JIT) matrix.
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
        if (g_smc_pg[i] == pg) return;                        // already protected
    if (mprotect((void *)pg, 0x4000, PROT_READ) != 0) return; // code page -> read-only; writes trap
    if (g_smc_n < SMC_MAX) g_smc_pg[g_smc_n++] = pg;
}
// If `a` falls in a protected SMC page, unprotect+forget it and return 1 (caller drops translations).
// Re-protected the next time the page is translated. Called from jit86_lazyguard (elf.c).
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
// ------------------------------------------------------------------------------------------------------

// x86 keeps its own naked trampolines (frontend/x86_64/translate.c: cpu pinned in x28, 16-GPR model,
// host save offsets #168..#264). Defining this tells jit/dispatch.c NOT to emit the aarch64 ones.
#define G_OWN_TRAMPOLINES 1

// One-time entry setup: opt2's emitted indirect hot path loads the 2-way IBTC base from cpu->ibtc_base
// in a single insn, so publish it once before the dispatcher loop. (Was the line right after
// pthread_setspecific in frontend/x86_64/dispatch.c.)
#define G_DISPATCH_ENTER(c) ((c)->ibtc_base = (uint64_t)g_xibtc)

// (4) Top-of-loop instrumentation. x86 checks the async-signal flag at the top of every iteration (the
// shared loop also checks it at the bottom -- the two are the same block boundary, so the top check here
// just preserves x86's historical position; maybe_deliver_signal is guarded + idempotent under g_pending,
// so the extra bottom check is a no-op once delivered). Then the fault-diagnosis block: prev/cur pc, the
// trace cap runaway guard, the malloc/avail-mask dumps, and the byte watchpoint. Verbatim from the old
// frontend/x86_64/dispatch.c. A PLAIN brace block (NOT do/while(0)) so the trace-cap `break` reaches the
// shared dispatcher while-loop -- the original broke the loop immediately, not just the macro.
#define G_DISPATCH_DEBUG(c)                                                                                            \
    {                                                                                                                  \
        if (g_pending) maybe_deliver_signal(c); /* async signal pending -> redirect to guest handler */                \
        g_prevpc = g_curpc;                                                                                            \
        g_curpc = (c)->rip;                                                                                            \
        g_disp_n++;                                                                                                    \
        if (g_trace && g_tracecap && g_disp_n > g_tracecap) { /* bound trace output for runaway guests */              \
            fprintf(stderr, "[jit86] trace cap %llu blocks reached -> stop\n", (unsigned long long)g_tracecap);        \
            (c)->exited = 1;                                                                                           \
            (c)->exit_code = 99;                                                                                       \
            break;                                                                                                     \
        }                                                                                                              \
        if (g_nochain && g_loadbase && (c)->rip == g_loadbase + 0x2ee0) g_malloc_n++; /* __libc_malloc_impl entries */ \
        if (g_nochain && g_loadbase) {                                                                                 \
            uint64_t po =                                                                                              \
                g_prevpc - g_loadbase; /* malloc first-handout: dump the new group's avail_mask (rbp=meta) */          \
            if (po >= 0x32a0 && po <= 0x3340) {                                                                        \
                uint64_t rbp = (c)->r[5], rax = (c)->r[0];                                                             \
                uint32_t avail = (rbp > 0x10000) ? *(uint32_t *)(rbp + 0x1c) : 0;                                      \
                fprintf(stderr, "[av] blk+%llx handout=%llx meta(rbp)=%llx avail_mask[rbp+1c]=%x freed[rbp+18]=%x\n",  \
                        (unsigned long long)po, (unsigned long long)rax, (unsigned long long)rbp, avail,               \
                        (rbp > 0x10000) ? *(uint32_t *)(rbp + 0x18) : 0);                                              \
            }                                                                                                          \
        }                                                                                                              \
        if (g_w8 && *g_w8 != g_w8v) { /* byte-watchpoint: report the block that just changed it */                     \
            fprintf(stderr, "[w8] @%p %02x -> %02x  by block +%llx  malloc#=%llu  rsi=%llx\n", (void *)g_w8, g_w8v,    \
                    *g_w8, (unsigned long long)(g_prevpc - g_loadbase), (unsigned long long)g_malloc_n,                \
                    (unsigned long long)(c)->r[6]);                                                                    \
            g_w8v = *g_w8;                                                                                             \
        }                                                                                                              \
    }

// §B-equivalent on-flush engine reset. x86 has no shadow stack; instead, on a wholesale cache flush the
// opt2 2-way IBTC bodies point into the cache we just dropped, so zero it. (The shared loop already
// memset()s the 1-way g_ibtc inline; this drops the x86-only g_xibtc.) Was the `memset(g_xibtc, ...)`
// after the flush in frontend/x86_64/dispatch.c.
#define G_SHADOW_CLEAR(c) memset(g_xibtc, 0, sizeof g_xibtc)

// A3 (aarch64-only lever): no §B-off block-entry alignment on x86. Defined so the shared jit/dispatch.c
// compiles; expands to a compile-time 0 -> the alignment `while` is dead-stripped on x86.
#define G_BLOCK_ALIGN 0

// ARM-B1 (aarch64-only lever): no VDBE meta-trace on x86 (the x86 IBTC keys differently -- no ic_site/pc
// seam). Defined empty so the shared jit/dispatch.c compiles for the x86_64 engine; both levers are
// aarch64-only and default-OFF, so x86 behaviour is unchanged.
#define G_VDBE_SDC_FILL(c) ((void)0)
#define G_IBPROF_LOG(c) ((void)0)

// Post-translate chaining. x86's translate_block() already calls patch_links_to() internally (frontend/
// x86_64/translate.c, gated !g_threaded), so the dispatcher must NOT chain again. (aarch64 moved chaining
// to the dispatcher; x86 keeps it in translate_block -- the per-arch placement the shared loop hides here.)
#define G_DISPATCH_CHAIN(c) ((void)0)

// W6A item 3: after translating a block, write-protect its 16KB source page so a JIT (RWX-mmap) guest's
// later overwrite traps in jit86_lazyguard -> smc_on_write() drops the stale translation. Inert unless
// g_rwx_guest is set (smc_protect returns immediately). Was the smc_protect(c->rip) after the translate.
#define G_AFTER_TRANSLATE(c) smc_protect((c)->rip)

// (5) Per-block JT trace dump. x86 register/flag layout (flags derived from cpu->nzcv; stored C = NOT
// x86 CF). Verbatim from frontend/x86_64/dispatch.c.
#define G_TRACE_DUMP(c)                                                                                                \
    if (g_trace) {                                                                                                     \
        unsigned nz = (unsigned)(c)->nzcv;                                                                             \
        int CF = !((nz >> 29) & 1), ZF = (nz >> 30) & 1, SF = (nz >> 31) & 1, OF = (nz >> 28) & 1;                     \
        fprintf(stderr,                                                                                                \
                "[blk] rip=%llx rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx r8=%llx r9=%llx "       \
                "r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx fl=C%dZ%dS%dO%d\n",                             \
                (unsigned long long)(c)->rip, (unsigned long long)(c)->r[RAX], (unsigned long long)(c)->r[3],          \
                (unsigned long long)(c)->r[RCX], (unsigned long long)(c)->r[RDX], (unsigned long long)(c)->r[RSI],     \
                (unsigned long long)(c)->r[RDI], (unsigned long long)(c)->r[RBP], (unsigned long long)(c)->r[8],       \
                (unsigned long long)(c)->r[9], (unsigned long long)(c)->r[10], (unsigned long long)(c)->r[11],         \
                (unsigned long long)(c)->r[12], (unsigned long long)(c)->r[13], (unsigned long long)(c)->r[14],        \
                (unsigned long long)(c)->r[15], CF, ZF, SF, OF);                                                       \
    }

// (1) IBTC miss fill. x86 keys off c->ic_miss (0/1), stores the PLAIN body (no body-8 stub; x16-x21 are
// free scratch, no stash/restore), and is skipped under threads (the indirect probe reads g_ibtc/g_xibtc
// unlocked -> a torn fill would dispatch the wrong body). IBTC1WAY=1 restores the old 1-way shared-g_ibtc
// fill; otherwise opt2's 2-way set-associative g_xibtc insert. Verbatim from frontend/x86_64/dispatch.c.
#define G_IBTC_FILL(c)                                                                                                 \
    if ((c)->ic_miss) {                                                                                                \
        if (!g_threaded) {                                                                                             \
            void *body = map_body((c)->rip);                                                                           \
            if (body) {                                                                                                \
                if (ibtc1way()) { /* IBTC1WAY=1: exact prior 1-way shared-g_ibtc fill */                               \
                    uint32_t h = (uint32_t)(((c)->rip >> 2) & (IBTC_N - 1));                                           \
                    g_ibtc[h].target = (c)->rip;                                                                       \
                    g_ibtc[h].body = body;                                                                             \
                } else { /* opt2: 2-way insert -> reuse the way already holding this target, else a free */            \
                    uint32_t s = (uint32_t)(((c)->rip >> 2) & (XIBTC_SETS - 1));                                       \
                    int w0 = s * 2, w1 = s * 2 + 1;                                                                    \
                    int w = (!g_xibtc[w0].target || g_xibtc[w0].target == (c)->rip)   ? w0                             \
                            : (!g_xibtc[w1].target || g_xibtc[w1].target == (c)->rip) ? w1                             \
                                                                                      : w0; /* way, else evict way0 */ \
                    g_xibtc[w].target = (c)->rip;                                                                      \
                    g_xibtc[w].body = body;                                                                            \
                }                                                                                                      \
                g_ibtc_fill++;                                                                                         \
            }                                                                                                          \
        }                                                                                                              \
        (c)->ic_miss = 0;                                                                                              \
    }

// (2) Post-run_block reason handling. The full x86 reason switch: the unimplemented-opcode abort (99),
// the W5-B R_TIER2 promote, R_CPUID, the W4-C R_REPSTR rep cmps/scas idiom, the x87 m80 fld/fstp, the
// 128/64 div/idiv done in C, and finally R_SYSCALL (x86 pre-advances rip in the emitter, so NO post-
// service pc-advance -- the per-arch syscall tail convention lives here; aarch64 does pc += 4 instead).
// Each non-syscall case `continue`s the shared while-loop (so the shared `if (reason==R_TIER2) ...`
// tail line never re-fires for x86). Verbatim from frontend/x86_64/dispatch.c. `break` exits the loop.
#define G_DISPATCH_REASON(c)                                                                                           \
    if ((c)->reason == 99) {                                                                                           \
        fprintf(stderr, "[jit86] aborting at rip marker %llx (unimplemented opcode)\n", (unsigned long long)(c)->rip); \
        if (g_trace) {                                                                                                 \
            for (int rr = 0; rr < 16; rr++) { /* dump heap-pointer regs (meta etc.) */                                 \
                uint64_t v = (c)->r[rr];                                                                               \
                if (v > 0x100000000ull && v < 0x200000000ull && (v & 7) == 0) {                                        \
                    fprintf(stderr, "  r%d=%llx:", rr, (unsigned long long)v);                                         \
                    for (int i = 0; i < 5; i++)                                                                        \
                        fprintf(stderr, " %016llx", (unsigned long long)((uint64_t *)v)[i]);                           \
                    fprintf(stderr, "\n");                                                                             \
                }                                                                                                      \
            }                                                                                                          \
        }                                                                                                              \
        (c)->exited = 1;                                                                                               \
        (c)->exit_code = 70;                                                                                           \
        break;                                                                                                         \
    }                                                                                                                  \
    if ((c)->reason == R_TIER2) { /* W5B: hot self-loop back-edge fired; recompile+swap. rip = loop start */           \
        tier2_promote((c)->rip);                                                                                       \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_CPUID) {                                                                                      \
        do_cpuid(c);                                                                                                   \
        continue;                                                                                                      \
    } /* rip already = next */                                                                                         \
    if ((c)->reason == R_AVX) { /* VEX/EVEX AVX insn: emulate in C; rip = the insn, do_avx advances it */              \
        do_avx(c);                                                                                                     \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_SSE3B) { /* legacy 0F38/0F3A insn: emulate in C; rip = the insn, do_sse3b advances it */      \
        do_sse3b(c);                                                                                                   \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_REPSTR) {                                                                                     \
        do_repstr(c);                                                                                                  \
        continue;                                                                                                      \
    } /* W4-C rep cmps/scas idiom (rip already = next) */                                                              \
    if ((c)->reason == R_X87FLD) {                                                                                     \
        x87_fld_m80(c);                                                                                                \
        continue;                                                                                                      \
    } /* fld m80 (rip already = next) */                                                                               \
    if ((c)->reason == R_X87FSTP) {                                                                                    \
        x87_fstp_m80(c);                                                                                               \
        continue;                                                                                                      \
    } /* fstp m80 */                                                                                                   \
    if ((c)->reason == R_DIV) { /* 128/64 unsigned div (rip already = next) */                                         \
        uint64_t d = (c)->divop;                                                                                       \
        if (d == 0) {                                                                                                  \
            if (raise_guest_de(c)) continue; /* #DE -> guest SIGFPE handler (queued; delivered at loop top) */         \
            fprintf(stderr, "[jit86] #DE divide-by-zero\n");                                                           \
            (c)->exited = 1;                                                                                           \
            (c)->exit_code = 136;                                                                                      \
            break;                                                                                                     \
        }                                                                                                              \
        unsigned __int128 num = ((unsigned __int128)(c)->r[RDX] << 64) | (c)->r[RAX];                                  \
        (c)->r[RAX] = (uint64_t)(num / d);                                                                             \
        (c)->r[RDX] = (uint64_t)(num % d);                                                                             \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_IDIV) { /* 128/64 signed idiv */                                                              \
        int64_t d = (int64_t)(c)->divop;                                                                               \
        if (d == 0) {                                                                                                  \
            if (raise_guest_de(c)) continue; /* #DE -> guest SIGFPE handler (queued; delivered at loop top) */         \
            fprintf(stderr, "[jit86] #DE divide-by-zero\n");                                                           \
            (c)->exited = 1;                                                                                           \
            (c)->exit_code = 136;                                                                                      \
            break;                                                                                                     \
        }                                                                                                              \
        __int128 num = ((__int128)(int64_t)(c)->r[RDX] << 64) | (c)->r[RAX];                                           \
        (c)->r[RAX] = (uint64_t)(num / d);                                                                             \
        (c)->r[RDX] = (uint64_t)(num % d);                                                                             \
        continue;                                                                                                      \
    }                                                                                                                  \
    if ((c)->reason == R_SYSCALL) {                                                                                    \
        service(c);                                                                                                    \
        if ((c)->exited) break;                                                                                        \
        if ((c)->redirect) (c)->redirect = 0; /* else rip already = next (set at exit) */                              \
    }                                                                                                                  \
    /* R_BRANCH: c->rip already holds the target */
