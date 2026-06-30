// dd/runtime/jit -- the host<->guest boundary: entry trampoline + run_guest() dispatcher loop.

// ---------------- host entry trampoline ----------------
// run_block(cpu, code): save host callee-saved into cpu, set env=x28, jump to code.
// The block tail-calls block_return, which restores host state and returns here's
// caller (the dispatcher).
//
// Per-arch trampolines: aarch64 enters block_return with cpu in x0 (all 31 GPRs are guest regs) and
// saves at offsets #288..#376 (q8..q15 @#896, host_sp@#280). x86 has only 16 guest GPRs, pins cpu in
// x28 for the whole block, and saves at different offsets -- so the x86 frontend supplies its OWN
// run_block/block_return (frontend/x86_64/translate.c, included before this file) and defines
// G_OWN_TRAMPOLINES to suppress these aarch64 ones. (engine-dedup §B.1/§B.3: the register model is the
// one irreducible divergence; the shared loop only CALLS run_block, never bakes its offsets.)
#ifndef G_OWN_TRAMPOLINES
__attribute__((naked)) static void run_block(struct cpu *cpu, void *code) {
    // x0=cpu, x1=code
    __asm__ volatile(
        "str x19, [x0, #288]\n str x20, [x0, #296]\n"
        "str x21, [x0, #304]\n str x22, [x0, #312]\n"
        "str x23, [x0, #320]\n str x24, [x0, #328]\n"
        "str x25, [x0, #336]\n str x26, [x0, #344]\n"
        "str x27, [x0, #352]\n str x28, [x0, #360]\n"
        "str x29, [x0, #368]\n str x30, [x0, #376]\n"
        "str q8, [x0, #896]\n str q9, [x0, #912]\n str q10, [x0, #928]\n str q11, [x0, #944]\n"
        "str q12, [x0, #960]\n str q13, [x0, #976]\n str q14, [x0, #992]\n str q15, [x0, #1008]\n"
        // host_sp
        "mov x9, sp\n str x9, [x0, #280]\n"
        // x0=cpu -> emitted prologue
        "br x1\n");
}
__attribute__((naked)) static void block_return(void) {
    // x0 == &cpu
    __asm__ volatile(
        "ldr x19, [x0, #288]\n ldr x20, [x0, #296]\n"
        "ldr x21, [x0, #304]\n ldr x22, [x0, #312]\n"
        "ldr x23, [x0, #320]\n ldr x24, [x0, #328]\n"
        "ldr x25, [x0, #336]\n ldr x26, [x0, #344]\n"
        "ldr x27, [x0, #352]\n ldr x28, [x0, #360]\n"
        "ldr x29, [x0, #368]\n ldr x30, [x0, #376]\n"
        "ldr q8, [x0, #896]\n ldr q9, [x0, #912]\n ldr q10, [x0, #928]\n ldr q11, [x0, #944]\n"
        "ldr q12, [x0, #960]\n ldr q13, [x0, #976]\n ldr q14, [x0, #992]\n ldr q15, [x0, #1008]\n"
        // host sp
        "ldr x9, [x0, #280]\n mov sp, x9\n"
        "ret\n");
}
#endif // G_OWN_TRAMPOLINES

// ---------------- dispatch seam defaults ----------------
// Hooks the aarch64 frontend does NOT define in frontend/aarch64/dispatch_hooks.h (the seams added for
// engine-dedup PR3/PR4 + opts committed after the design). Their #ifndef defaults below reproduce the
// EXACT aarch64-inline behavior, so the aarch64 engine stays bit-identical; the x86 frontend overrides
// each in frontend/x86_64/dispatch_hooks.h. (The four PR2 seams -- G_DISPATCH_DEBUG / G_SHADOW_CLEAR /
// G_IBTC_FILL / G_DISPATCH_REASON -- are defined by BOTH frontends, so they need no default here.)

// One-time per-thread setup before the loop. aarch64 has none.
#ifndef G_DISPATCH_ENTER
#define G_DISPATCH_ENTER(c) ((void)0)
#endif
// Post-translate chaining. aarch64 chains in the dispatcher (here); x86 chains inside translate_block.
#ifndef G_DISPATCH_CHAIN
#define G_DISPATCH_CHAIN(c) patch_links_to(G_PC(c), map_body(G_PC(c)))
#endif
// Post-translate per-arch step. aarch64 has none; x86 does W6A SMC source-page write-protect.
#ifndef G_AFTER_TRANSLATE
#define G_AFTER_TRANSLATE(c) ((void)0)
#endif
// Per-block JT trace dump (the 5th divergence). aarch64 dumps pc + x19/x20/sp + the first 6 block words.
#ifndef G_TRACE_DUMP
#define G_TRACE_DUMP(c)                                                                                                  \
    if (g_trace) {                                                                                                       \
        uint32_t *ci = (uint32_t *)G_PC(c);                                                                              \
        fprintf(stderr, "[blk] pc=%llx x19=%llx x20=%llx sp=%llx | %08x %08x %08x %08x %08x %08x\n",                     \
                (unsigned long long)G_PC(c), (unsigned long long)(c)->x[19], (unsigned long long)(c)->x[20],            \
                (unsigned long long)(c)->sp, ci[0], ci[1], ci[2], ci[3], ci[4], ci[5]);                                 \
    }
#endif

// ---------------- dispatcher ----------------
static void run_guest(struct cpu *c) {
    // this thread's cpu, for emitted block exits
    pthread_setspecific(g_cpu_key, c);
    // Join the stop-the-world thread registry so a peer's cache-full flush can quiesce us at a safepoint
    // (and so we are enumerated when WE flush). Unregistered after the loop -> an exited thread is never
    // signalled.
    stw_register();
    // Join the tid->thread registry so a tkill()/tgkill() to this thread can find it (thread-directed
    // signal delivery via cpu->tpending); left at loop exit so a dead thread is never targeted.
    thread_register(c);
    // Frontend hook: one-time per-thread entry setup (x86 publishes the 2-way IBTC base; empty on aarch64).
    G_DISPATCH_ENTER(c);
    while (!c->exited) {
        if (G_PC(c) == SIGRETURN_PC) {
            do_sigreturn(c);
            continue;
        // handler returned -> restore context
        }
        // A non-PIE image's un-relocated absolute jump lands on its (unmapped) low link vaddr; redirect it
        // into the biased image so we translate real code instead of faulting on the unmapped low address.
        if (g_nonpie_lo && G_PC(c) >= g_nonpie_lo && G_PC(c) < g_nonpie_hi) G_PC(c) += g_nonpie_bias;
        // Frontend hook: top-of-loop debug instrumentation (x86-only; empty on aarch64).
        G_DISPATCH_DEBUG(c);
        // With threads, the WHOLE cache lookup is under the lock: an unlocked
        // map_host() races map_put() (torn entry) and lacks the acquire barrier
        // that makes a peer thread's freshly-emitted+icache-flushed code visible.
        // Single-threaded skips the lock entirely (g_threaded == 0).
        if (g_threaded) pthread_mutex_lock(&g_jit_lock);
        void *code = map_host(G_PC(c));
        if (!code) {
            uint64_t _t0 = g_prof ? now_ns() : 0;
            // near full -> wholesale flush
            if (g_cp + (1u << 16) > g_cache + CACHE_SZ) {
                if (g_threaded && stw_peers_live()) {
                    // More than one guest thread is live: reusing the arena in place could free code out
                    // from under a peer mid-block. Stop the world and switch to a fresh cache instead
                    // (the old one is retained until peers drift off it). See jit/cache.c.
                    stw_flush();
                } else {
                    // Single-threaded (or every spawned peer has exited): safe wholesale in-place flush.
                    jit_wprot(0);
                    g_cp = g_cache;
                    memset(g_map, 0, sizeof g_map);
                    g_npend = 0;
                    // IBTC bodies point into the cache we just dropped
                    memset(g_ibtc, 0, sizeof g_ibtc);
                    // §B: shadow host_rets point into the dropped cache too -> clear (frontend hook)
                    G_SHADOW_CLEAR(c);
                    jit_wprot(1);
                }
            }
            jit_wprot(0);
            // A3 §B-off: align each new block ENTRY to 16B. §B-off shrinks the per-bl stubs, which
            // shifts where hot loops land in the cache and can deterministically de-align a NEON loop
            // (e.g. sha256, which has no hot returns yet wobbled ~7%). Padding lives BEFORE the entry
            // (branch/IBTC targets the aligned body), so the nops never execute -> zero runtime cost,
            // just stable layout. Gated on §B-off so NOSHADOWTUNE=1 stays byte-identical to baseline.
            if (G_BLOCK_ALIGN) while ((uintptr_t)g_cp & 15) emit32(0xD503201Fu); // nop
            g_emit_start = g_cp;
            code = translate_block(G_PC(c));
            g_prof_xlate++;
            // new block coherent on all cores FIRST (icache is on the RX alias under dual map)
            sys_icache_invalidate(J_RX(g_emit_start), (size_t)(g_cp - g_emit_start));
            // THEN chain existing blocks to it (still write mode). Frontend hook: aarch64 chains here;
            // x86's translate_block already chained internally, so its hook is a no-op.
            G_DISPATCH_CHAIN(c);
            // back to execute AFTER all cache writes
            jit_wprot(1);
            // Frontend hook: post-translate per-arch step (x86 W6A SMC source-page write-protect; empty aarch64).
            G_AFTER_TRANSLATE(c);
            if (g_prof) g_xlate_ns += now_ns() - _t0;
        }
        // ARM-B1 VDBETRACE: a speculative-direct-chain (SDC) JT-dispatch site missed to here (its tag is
        // bit0 of ic_site). (Re)specialize the SDC to the new target, then fall through to ALSO fill the
        // shared-hash IBTC (ic_site=1 = shared-only) so non-speculated targets still hit in-cache.
        // Per-arch seam: aarch64 is ic_site/pc-keyed; x86's IBTC keys differently (no ic_site) -> empty.
        G_VDBE_SDC_FILL(c);
        // IBTC: insert the indirect target that just missed (frontend hook -- per-arch IBTC contract:
        // aarch64 keys on ic_site/body-8/per-site IC literals; x86 will key on ic_miss/plain body).
        G_IBTC_FILL(c);
        // Resolve the RX alias to execute through WHILE STILL HOLDING the lock: a concurrent stop-the-world
        // flush may swap g_rw2rx (and g_cache) the instant we drop it, yet `code` is an address in the cache
        // that was current under the lock -- so J_RX(code) must use the matching g_rw2rx. (Single-threaded
        // takes no lock and cannot race a flush; the computation is identical.)
        void *rxcode = J_RX(code);
        // Publish the generation of the cache we are about to execute so a peer's stop-the-world flush can
        // reclaim a retired cache only once no thread is still running in it (see reclaim_retired). Done
        // under g_jit_lock (a flush holds it) so the value is consistent with g_cache_gen; threaded-only,
        // so the single-thread hot path stays zero-overhead.
        if (g_threaded) atomic_store_explicit(g_my_exec_gen, g_cache_gen, memory_order_relaxed);
        if (g_threaded) pthread_mutex_unlock(&g_jit_lock);
        // Frontend hook: per-block JT trace dump (per-arch register/flag layout). See §A.3 (5th divergence).
        G_TRACE_DUMP(c);
        c->reason = 0;
        if (g_prof) g_prof_cross++;
        // map_host()/translate_block() return RW-alias addresses; execute via the RX alias.
        run_block(c, rxcode);
        // Frontend hook: post-run_block reason handling (aarch64: R_SYSCALL service + pc+=4, else R_BRANCH;
        // x86 adds R_CPUID/x87/DIV/IDIV/99). The per-arch syscall pc-advance convention lives in the hook.
        G_DISPATCH_REASON(c);
        // ARM-B1 IBPROF: an indirect transfer was routed here for logging. c->pc already holds the
        // guest target (treated exactly like R_BRANCH); record (site -> target) and resume.
        // Per-arch seam: aarch64-only (ic_site/pc); x86 -> empty (measurement-only lever, never on by default).
        G_IBPROF_LOG(c);
        // W4E tier-2: a hot self-loop's back-edge counter fired -> recompile+swap it in. pc is already =
        // loop start, so the next iteration of this dispatcher loop runs the folded block. R_TIER2 is
        // disjoint from R_SYSCALL (handled in the reason hook above) so this never double-fires.
        // tier2_promote is a no-op under threads / NOTIER2. NOTE: the x86 frontend's G_DISPATCH_REASON
        // handles R_TIER2 itself (with `continue`), so for the x86 engine this line is never reached;
        // it remains the aarch64 path. Both arches define tier2_promote (per-arch).
        if (c->reason == R_TIER2) tier2_promote(G_PC(c));
        // async signal -> guest handler (process-directed g_pending OR thread-directed cpu->tpending)
        if (g_pending || c->tpending) maybe_deliver_signal(c);
    }
    // Leave the registries: this thread will never execute in the cache again, nor be a signal target.
    thread_unregister(c);
    stw_unregister();
}
