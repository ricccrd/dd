// dd/runtime/jit -- the host<->guest boundary: entry trampoline + run_guest() dispatcher loop.

// ---------------- host entry trampoline ----------------
// run_block(cpu, code): save host callee-saved into cpu, set env=x28, jump to code.
// The block tail-calls block_return, which restores host state and returns here's
// caller (the dispatcher).
__attribute__((naked)) static void run_block(struct cpu *cpu, void *code) {
    __asm__ volatile(                          // x0=cpu, x1=code
        "str x19, [x0, #288]\n str x20, [x0, #296]\n"
        "str x21, [x0, #304]\n str x22, [x0, #312]\n"
        "str x23, [x0, #320]\n str x24, [x0, #328]\n"
        "str x25, [x0, #336]\n str x26, [x0, #344]\n"
        "str x27, [x0, #352]\n str x28, [x0, #360]\n"
        "str x29, [x0, #368]\n str x30, [x0, #376]\n"
        "str q8, [x0, #896]\n str q9, [x0, #912]\n str q10, [x0, #928]\n str q11, [x0, #944]\n"
        "str q12, [x0, #960]\n str q13, [x0, #976]\n str q14, [x0, #992]\n str q15, [x0, #1008]\n"
        "mov x9, sp\n str x9, [x0, #280]\n"   // host_sp
        "br x1\n");                            // x0=cpu -> emitted prologue
}
__attribute__((naked)) static void block_return(void) {
    __asm__ volatile(                          // x0 == &cpu
        "ldr x19, [x0, #288]\n ldr x20, [x0, #296]\n"
        "ldr x21, [x0, #304]\n ldr x22, [x0, #312]\n"
        "ldr x23, [x0, #320]\n ldr x24, [x0, #328]\n"
        "ldr x25, [x0, #336]\n ldr x26, [x0, #344]\n"
        "ldr x27, [x0, #352]\n ldr x28, [x0, #360]\n"
        "ldr x29, [x0, #368]\n ldr x30, [x0, #376]\n"
        "ldr q8, [x0, #896]\n ldr q9, [x0, #912]\n ldr q10, [x0, #928]\n ldr q11, [x0, #944]\n"
        "ldr q12, [x0, #960]\n ldr q13, [x0, #976]\n ldr q14, [x0, #992]\n ldr q15, [x0, #1008]\n"
        "ldr x9, [x0, #280]\n mov sp, x9\n"   // host sp
        "ret\n");
}


// ---------------- dispatcher ----------------
static void run_guest(struct cpu *c) {
    pthread_setspecific(g_cpu_key, c);    // this thread's cpu, for emitted block exits
    while (!c->exited) {
        if (c->pc == SIGRETURN_PC) { do_sigreturn(c); continue; }   // handler returned -> restore context
        // With threads, the WHOLE cache lookup is under the lock: an unlocked
        // map_host() races map_put() (torn entry) and lacks the acquire barrier
        // that makes a peer thread's freshly-emitted+icache-flushed code visible.
        // Single-threaded skips the lock entirely (g_threaded == 0).
        if (g_threaded) pthread_mutex_lock(&g_jit_lock);
        void *code = map_host(c->pc);
        if (!code) {
            if (g_cp + (1u << 16) > g_cache + CACHE_SZ) {   // near full -> wholesale flush
                if (g_threaded) { fprintf(stderr, "[jit] code cache full with threads (unsupported)\n"); _exit(70); }
                pthread_jit_write_protect_np(0);
                g_cp = g_cache; memset(g_map, 0, sizeof g_map); g_npend = 0;
                memset(g_ibtc, 0, sizeof g_ibtc);    // IBTC bodies point into the cache we just dropped
                c->ssp = 0;                          // §B: shadow host_rets point into the dropped cache too -> clear
                pthread_jit_write_protect_np(1);
            }
            pthread_jit_write_protect_np(0);
            g_emit_start = g_cp;
            code = translate_block(c->pc); g_prof_xlate++;
            sys_icache_invalidate(g_emit_start, (size_t)(g_cp - g_emit_start));   // new block coherent on all cores FIRST
            patch_links_to(c->pc, map_body(c->pc));                              // THEN chain existing blocks to it (still write mode)
            pthread_jit_write_protect_np(1);                                     // back to execute AFTER all cache writes
        }
        if (c->ic_site) {                          // IBTC: insert the indirect target that just missed
            g_prof_miss++;
            // The shared IBTC read (in emitted code) is unlocked, and the per-site fill toggles
            // W^X process-wide -- both race other threads. Under threads, skip the fill: indirect
            // branches fall to the (locked) dispatcher. Correct, slightly slower.
            void *bd = g_threaded ? NULL : map_body(c->pc);
            if (bd) { uint32_t h = (uint32_t)((c->pc >> 2) & (IBTC_N - 1));
                      g_ibtc[h].body = (void *)((uint64_t)bd - 8);          // body_ind; written first
                      __atomic_store_n(&g_ibtc[h].target, c->pc, __ATOMIC_RELEASE);
                      if (c->ic_site != 1) {       // per-site monomorphic cache (literals in JIT cache, W^X)
                          pthread_jit_write_protect_np(0);
                          ((uint64_t *)c->ic_site)[1] = (uint64_t)bd - 8;   // Lsite_body
                          ((uint64_t *)c->ic_site)[0] = c->pc;              // Lsite_tgt
                          pthread_jit_write_protect_np(1);
                      } }
            c->ic_site = 0;
        }
        if (g_threaded) pthread_mutex_unlock(&g_jit_lock);
        if (g_trace) { uint32_t *ci = (uint32_t *)c->pc;
            fprintf(stderr, "[blk] pc=%llx x19=%llx x20=%llx sp=%llx | %08x %08x %08x %08x %08x %08x\n", (unsigned long long)c->pc, (unsigned long long)c->x[19], (unsigned long long)c->x[20], (unsigned long long)c->sp, ci[0], ci[1], ci[2], ci[3], ci[4], ci[5]); }
        c->reason = 0;
        if (g_prof) g_prof_cross++;
        run_block(c, code);
        if (c->reason == R_SYSCALL) { if (g_prof) g_prof_sys++; service(c); if (c->exited) break;
            if (c->redirect) c->redirect = 0; else c->pc += 4; }   // execve/sigreturn set pc directly
        // else R_BRANCH: c->pc already holds the target
        if (g_pending) maybe_deliver_signal(c);                    // async signal -> guest handler
    }
}

