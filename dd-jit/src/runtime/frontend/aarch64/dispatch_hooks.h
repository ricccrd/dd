// frontend/aarch64/dispatch_hooks.h -- the aarch64 frontend's definitions of the shared run_guest()
// dispatch seam (engine-dedup PR2). The shared jit/dispatch.c calls these hooks at the four spots where
// the dispatcher historically diverged per guest arch (see docs/design/engine-dedup.md §A.3/§B.3):
//
//   G_DISPATCH_DEBUG(c)   top-of-loop debug instrumentation       (x86-only; EMPTY on aarch64)
//   G_SHADOW_CLEAR(c)     §B shadow-stack reset on wholesale flush (aarch64-only)
//   G_IBTC_FILL(c)        IBTC miss fill keyed on ic_site/body-8   (per-arch IBTC contract)
//   G_DISPATCH_REASON(c)  post-run_block reason handling + the per-arch syscall pc-advance
//
// Each macro below expands to EXACTLY the code that used to be inline in jit/dispatch.c's run_guest(), so
// the aarch64 engine is bit-identical after the refactor. The macros are expanded at their call sites
// inside the dispatcher loop (not here), so `break` reaches the loop and engine globals (g_ibtc, map_body,
// g_prof_*, IBTC_N, R_SYSCALL, ...) are in scope there even though this header is included early via abi.h.
// The x86 frontend supplies its own definitions of these names in a later PR (PR3/PR4).

// (4) x86-only top-of-loop instrumentation (g_prevpc/g_disp_n/trace cap/g_w8 watchpoint/malloc track).
// aarch64 has no such block -> empty.
#define G_DISPATCH_DEBUG(c) ((void)0)

// §B shadow stack: on a wholesale cache flush the shadow host_rets point into the cache we just dropped,
// so reset the shadow stack pointer. (Was the inline `c->ssp = 0;`.)
#define G_SHADOW_CLEAR(c) ((c)->ssp = 0)

// (1) IBTC miss fill. aarch64 keys off c->ic_site (0=none, 1=shared-only, else=per-site IC literal addr),
// stores body-8 (the indirect-entry stub that restores guest x16/x17), and patches the per-site
// monomorphic IC literals in the W^X cache. Byte-for-byte the prior inline block.
/* W5C: race-free threaded IBTC fill. The fill runs under g_jit_lock (single writer);    \
 * readers are lock-free emitted code. Two hazards historically forced the threaded skip: \
 *   (1) the per-site monomorphic IC writes a NON-atomic 16-byte literal pair into the    \
 *       W^X code cache, requiring a process-wide W^X flip + tearable by a peer reader;    \
 *   (2) the shared hash {target,body} pair was written as two plain stores -> a peer     \
 *       reader could observe a torn entry (new target / stale body, or vice-versa).      \
 * Fix: under threads, fill ONLY the shared hash, and publish it with a single 128-bit    \
 * RELEASE store (ibtc_publish, atomic under LSE2). The reader consumes it with a single  \
 * atomic-acquire ldp, so the pair is always observed whole -> no torn dispatch. The      \
 * per-site IC (which would need the tearable literal-pair write) is skipped under threads;\
 * its inline compare just always misses (literals stay 0) and falls into the shared hash.\
 * Single-threaded behavior is byte-identical: shared hash via plain stores in dependency \
 * order + the per-site IC exactly as before. NOMTIBTC=1 restores the locked-dispatcher    \
 * path (threaded indirect branches miss to C every time). */
#define G_IBTC_FILL(c)                                                                                          \
    if ((c)->ic_site) {                                                                                         \
        g_prof_miss++;                                                                                          \
        int _mt = g_threaded;                                                                                   \
        void *bd = (_mt && !g_mtibtc) ? NULL : map_body((c)->pc);                                               \
        if (bd) {                                                                                               \
            uint32_t h = (uint32_t)(((c)->pc >> 2) & (IBTC_N - 1));                                             \
            void *bind = (void *)((uint64_t)J_RX(bd) - 8); /* RX body_ind (entry that restores x16/x17) */     \
            if (_mt) {                                                                                          \
                /* threaded: single 128-bit atomic release publish; consumed by an atomic ldp reader */        \
                ibtc_publish(&g_ibtc[h], (c)->pc, bind);                                                        \
                g_mtfill++;                                                                                     \
            } else {                                                                                            \
                g_ibtc[h].body = bind; /* body_ind; written first */                                           \
                __atomic_store_n(&g_ibtc[h].target, (c)->pc, __ATOMIC_RELEASE);                                \
                if ((c)->ic_site != 1) { /* per-site monomorphic cache (literals in the code cache) */         \
                    /* ic_site is the RX address of the literal pair (from a runtime adr); write via RW. */     \
                    uint64_t *site = (uint64_t *)J_RW((c)->ic_site);                                           \
                    jit_wprot(0);                                                                               \
                    site[1] = (uint64_t)bind; /* Lsite_body (RX) */                                            \
                    site[0] = (c)->pc;        /* Lsite_tgt       */                                            \
                    jit_wprot(1);                                                                               \
                }                                                                                               \
            }                                                                                                   \
        }                                                                                                       \
        (c)->ic_site = 0;                                                                                       \
    }

// (2) Post-run_block reason handling. aarch64 has only R_SYSCALL (service + advance) and R_BRANCH (target
// already in pc). The syscall pc-advance divergence lives HERE: aarch64 advances `pc += 4` past the SVC
// on the non-redirect path (x86 will instead rely on rip pre-set in the emitter). `break` exits the
// dispatcher loop into which this expands. Byte-for-byte the prior inline block.
#define G_DISPATCH_REASON(c)                                                                                    \
    if ((c)->reason == R_SYSCALL) {                                                                             \
        if (g_prof) g_prof_sys++;                                                                               \
        service(c);                                                                                             \
        if ((c)->exited) break;                                                                                 \
        if ((c)->redirect)                                                                                      \
            (c)->redirect = 0;                                                                                  \
        else                                                                                                    \
            (c)->pc += 4; /* execve/sigreturn set pc directly */                                               \
    }                                                                                                           \
    /* else R_BRANCH: c->pc already holds the target */
