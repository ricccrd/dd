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
#define G_IBTC_FILL(c)                                                                                          \
    if ((c)->ic_site) {                                                                                         \
        g_prof_miss++;                                                                                          \
        /* shared IBTC read + per-site fill toggle W^X process-wide -> both race peers; skip when threaded */  \
        void *bd = g_threaded ? NULL : map_body((c)->pc);                                                      \
        if (bd) {                                                                                               \
            uint32_t h = (uint32_t)(((c)->pc >> 2) & (IBTC_N - 1));                                             \
            g_ibtc[h].body = (void *)((uint64_t)bd - 8); /* body_ind; written first */                         \
            __atomic_store_n(&g_ibtc[h].target, (c)->pc, __ATOMIC_RELEASE);                                    \
            if ((c)->ic_site != 1) { /* per-site monomorphic cache (literals in JIT cache, W^X) */             \
                pthread_jit_write_protect_np(0);                                                                \
                ((uint64_t *)(c)->ic_site)[1] = (uint64_t)bd - 8; /* Lsite_body */                             \
                ((uint64_t *)(c)->ic_site)[0] = (c)->pc;          /* Lsite_tgt  */                             \
                pthread_jit_write_protect_np(1);                                                                \
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
