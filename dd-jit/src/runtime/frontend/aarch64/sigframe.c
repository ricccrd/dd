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
