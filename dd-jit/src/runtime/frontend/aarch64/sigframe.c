// frontend/aarch64/sigframe.c -- the aarch64 Linux rt_sigframe (per-arch: register layout differs from
// x86_64). os/linux/signal.c drives delivery (pending/mask/translate) and calls these to build/restore.

// Linux aarch64 rt_sigframe: siginfo(128) then ucontext{flags,link,stack(24),
// sigmask@40,...,mcontext@168}; sigcontext{fault,regs[31]@8,sp@256,pc@264,
// pstate@272,reserved@280}. We stash the guest V-regs in the reserved area.
static void build_signal_frame(struct cpu *c, int sig) {
    if (g_trace)
        fprintf(stderr, "[sig] deliver %d sp=%llx handler=%llx\n", sig, (unsigned long long)c->sp,
                (unsigned long long)g_sigact[sig].handler);
    uint64_t frame = (c->sp - 4688) & ~15ull;
    uint8_t *f = (uint8_t *)frame;
    memset(f, 0, 4688);
    // siginfo.si_signo
    *(int *)(f + 0) = sig;
    *(int *)(f + 8) = g_sigcode[sig];       // si_code (SI_QUEUE for sigqueue, else 0)
    *(uint64_t *)(f + 16) = g_sigaddr[sig]; // si_addr (synchronous fault address; 0 for async)
    *(uint64_t *)(f + 24) = g_sigval[sig];  // si_value (sigqueue's sival_int/ptr)
    g_sigcode[sig] = 0; g_sigval[sig] = 0; g_sigaddr[sig] = 0; // consumed
    uint64_t uc = frame + 128, mc = uc + 168;
    // uc_sigmask (signal mask to restore)
    *(uint64_t *)(uc + 40) = c->sigmask;
    for (int i = 0; i < 31; i++)
        *(uint64_t *)(mc + 8 + i * 8) = c->x[i];
    *(uint64_t *)(mc + 256) = c->sp;
    *(uint64_t *)(mc + 264) = c->pc;
    *(uint64_t *)(mc + 272) = c->nzcv;
    // preserve NEON across the handler
    memcpy((void *)(mc + 280), c->v, sizeof c->v);
    c->x[0] = (uint64_t)sig;
    c->x[1] = frame;
    // handler(signo, siginfo*, ucontext*)
    c->x[2] = uc;
    // return address -> sigreturn
    c->x[30] = SIGRETURN_PC;
    c->sp = frame;
    c->pc = g_sigact[sig].handler;
    c->sigmask |= g_sigact[sig].mask;
    // SA_NODEFER (sigset_t bit N-1)
    if (!(g_sigact[sig].flags & 0x40000000)) c->sigmask |= (1ull << (sig - 1));
}
static void do_sigreturn(struct cpu *c) {
    uint64_t frame = c->sp, uc = frame + 128, mc = uc + 168;
    for (int i = 0; i < 31; i++)
        c->x[i] = *(uint64_t *)(mc + 8 + i * 8);
    c->sp = *(uint64_t *)(mc + 256);
    c->pc = *(uint64_t *)(mc + 264);
    c->nzcv = *(uint64_t *)(mc + 272);
    memcpy(c->v, (void *)(mc + 280), sizeof c->v);
    c->sigmask = *(uint64_t *)(uc + 40);
}

// Synchronous-fault delivery support (driven by os/linux/signal.c's deliver_guest_fault). In a translated
// aarch64 block all NON-stolen guest GPRs live in the matching host x-register, and the guest SP/flags/V
// state is the live host SP/NZCV/V state; the engine-stolen regs (x16/x17/x18/x28/x30) are kept in cpu->x[]
// at every instruction boundary. So reconstruct the guest state by copying the host fault context back into
// cpu, leaving the stolen regs untouched. block_return (jit/dispatch.c, included later) unwinds a block back
// to the dispatcher: it restores the host callee-saved state run_block saved at block entry and returns to
// the run_guest loop, which then sees cpu->pc == handler and runs it.
static void block_return(void);
static int sigframe_capture_fault(struct cpu *c, void *ucv) {
    ucontext_t *uc = (ucontext_t *)ucv;
    uint64_t hpc = (uint64_t)uc->uc_mcontext->__ss.__pc;
    uint64_t lo = (uint64_t)g_cache + g_rw2rx, hi = lo + CACHE_SZ;
    if (hpc < lo || hpc >= hi) return 0;     // host PC outside the code cache -> a genuine engine fault
    uint64_t *X = uc->uc_mcontext->__ss.__x; // __x[0..28], then fp=X[29] lr=X[30] sp=X[31]
    for (int r = 0; r <= 30; r++)
        if (!is_stolen(r)) c->x[r] = X[r];
    c->sp = X[31];
    c->nzcv = uc->uc_mcontext->__ss.__cpsr;
    memcpy(c->v, uc->uc_mcontext->__ns.__v, sizeof c->v);
    return 1;
}
static void sigframe_resume_dispatch(struct cpu *c, void *ucv) {
    ucontext_t *uc = (ucontext_t *)ucv;
    uc->uc_mcontext->__ss.__x[0] = (uint64_t)c; // block_return reads &cpu from x0
    uc->uc_mcontext->__ss.__pc = (uint64_t)block_return;
}
