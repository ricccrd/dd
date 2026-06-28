// dd/runtime/jit -- the host<->guest boundary: entry trampoline + run_guest() dispatcher loop.

// ---------------- host entry trampoline ----------------
// run_block(cpu, code): save host callee-saved into cpu, set env=x28, jump to code.
// The block tail-calls block_return, which restores host state and returns here's
// caller (the dispatcher).
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

// ---------------- dispatcher ----------------
static void run_guest(struct cpu *c) {
    // this thread's cpu, for emitted block exits
    pthread_setspecific(g_cpu_key, c);
    while (!c->exited) {
        if (c->pc == SIGRETURN_PC) {
            do_sigreturn(c);
            continue;
        // handler returned -> restore context
        }
        // A non-PIE image's un-relocated absolute jump lands on its (unmapped) low link vaddr; redirect it
        // into the biased image so we translate real code instead of faulting on the unmapped low address.
        if (g_nonpie_lo && c->pc >= g_nonpie_lo && c->pc < g_nonpie_hi) c->pc += g_nonpie_bias;
        // Frontend hook: top-of-loop debug instrumentation (x86-only; empty on aarch64).
        G_DISPATCH_DEBUG(c);
        // With threads, the WHOLE cache lookup is under the lock: an unlocked
        // map_host() races map_put() (torn entry) and lacks the acquire barrier
        // that makes a peer thread's freshly-emitted+icache-flushed code visible.
        // Single-threaded skips the lock entirely (g_threaded == 0).
        if (g_threaded) pthread_mutex_lock(&g_jit_lock);
        void *code = map_host(c->pc);
        if (!code) {
            // near full -> wholesale flush
            if (g_cp + (1u << 16) > g_cache + CACHE_SZ) {
                if (g_threaded) {
                    fprintf(stderr, "[jit] code cache full with threads (unsupported)\n");
                    _exit(70);
                }
                pthread_jit_write_protect_np(0);
                g_cp = g_cache;
                memset(g_map, 0, sizeof g_map);
                g_npend = 0;
                // IBTC bodies point into the cache we just dropped
                memset(g_ibtc, 0, sizeof g_ibtc);
                // §B: shadow host_rets point into the dropped cache too -> clear (frontend hook)
                G_SHADOW_CLEAR(c);
                pthread_jit_write_protect_np(1);
            }
            pthread_jit_write_protect_np(0);
            g_emit_start = g_cp;
            code = translate_block(c->pc);
            g_prof_xlate++;
            // new block coherent on all cores FIRST
            sys_icache_invalidate(g_emit_start, (size_t)(g_cp - g_emit_start));
            // THEN chain existing blocks to it (still write mode)
            patch_links_to(c->pc, map_body(c->pc));
            // back to execute AFTER all cache writes
            pthread_jit_write_protect_np(1);
        }
        // IBTC: insert the indirect target that just missed (frontend hook -- per-arch IBTC contract:
        // aarch64 keys on ic_site/body-8/per-site IC literals; x86 will key on ic_miss/plain body).
        G_IBTC_FILL(c);
        if (g_threaded) pthread_mutex_unlock(&g_jit_lock);
        if (g_trace) {
            uint32_t *ci = (uint32_t *)c->pc;
            fprintf(stderr, "[blk] pc=%llx x19=%llx x20=%llx sp=%llx | %08x %08x %08x %08x %08x %08x\n",
                    (unsigned long long)c->pc, (unsigned long long)c->x[19], (unsigned long long)c->x[20],
                    (unsigned long long)c->sp, ci[0], ci[1], ci[2], ci[3], ci[4], ci[5]);
        }
        c->reason = 0;
        if (g_prof) g_prof_cross++;
        run_block(c, code);
        // Frontend hook: post-run_block reason handling (aarch64: R_SYSCALL service + pc+=4, else R_BRANCH;
        // x86 adds R_CPUID/x87/DIV/IDIV/99). The per-arch syscall pc-advance convention lives in the hook.
        G_DISPATCH_REASON(c);
        // W4E tier-2: a hot self-loop's back-edge counter fired -> recompile+swap it in. pc is already =
        // loop start, so the next iteration of this dispatcher loop runs the folded block. R_TIER2 is
        // disjoint from R_SYSCALL (handled in the hook above) so this never double-fires. tier2_promote is a
        // no-op under threads / NOTIER2. (This TU is the aarch64 dispatcher only -- the x86 engine includes
        // frontend/x86_64/dispatch.c instead -- so R_TIER2/tier2_promote are aarch64-scoped here.)
        if (c->reason == R_TIER2) tier2_promote(c->pc);
        // async signal -> guest handler
        if (g_pending) maybe_deliver_signal(c);
    }
}
