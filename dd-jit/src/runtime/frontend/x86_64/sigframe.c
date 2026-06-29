// frontend/x86_64/sigframe.c -- the x86-64 Linux rt_sigframe (per-arch register layout). os/linux/
// signal.c drives delivery + owns g_sigact/SIGRETURN_PC/g_pending; these build/restore the frame.

// x86-64 sigcontext gregs index -> guest cpu->r[] index (r8..r15,rdi,rsi,rbp,rbx,rdx,rax,rcx,rsp; then rip,eflags)
static const int GREG2R[16] = {8, 9, 10, 11, 12, 13, 14, 15, 7, 6, 5, 3, 2, 0, 1, 4}; // gregs[0..15]
static uint64_t nzcv_to_eflags(uint64_t nz) {
    uint64_t f = 0x2; // bit1 reserved (always 1)
    if (!((nz >> 29) & 1)) f |= 1u << 0;
    if ((nz >> 30) & 1) f |= 1u << 6; // CF (stored inverted), ZF
    if ((nz >> 31) & 1) f |= 1u << 7;
    if ((nz >> 28) & 1) f |= 1u << 11; // SF, OF
    return f;
}
static uint64_t eflags_to_nzcv(uint64_t f) {
    uint64_t nz = 0;
    if (!(f & 1)) nz |= 1u << 29;
    if (f & (1u << 6)) nz |= 1u << 30; // CF (invert), ZF
    if (f & (1u << 7)) nz |= 1u << 31;
    if (f & (1u << 11)) nz |= 1u << 28; // SF, OF
    return nz;
}
static void build_signal_frame(struct cpu *c, int sig) {
    uint64_t sp = (c->r[4] - 2048) & ~15ull;                        // 16-aligned frame base; uc lives here
    uint64_t uc = sp, mc = uc + 40, info = uc + 512, xs = uc + 768; // ucontext / mcontext(gregs) / siginfo / xmm save
    memset((void *)sp, 0, 2048);
    for (int i = 0; i < 16; i++)
        *(uint64_t *)(mc + i * 8) = c->r[GREG2R[i]];      // gregs[0..15]
    *(uint64_t *)(mc + 16 * 8) = c->rip;                  // gregs[16] = RIP
    *(uint64_t *)(mc + 17 * 8) = nzcv_to_eflags(c->nzcv); // gregs[17] = EFL
    *(uint64_t *)(uc + 296) = c->sigmask;                 // uc_sigmask (restored on sigreturn)
    memcpy((void *)xs, c->v, sizeof c->v);                // preserve guest xmm across the handler
    *(int *)(info + 0) = sig;                             // siginfo.si_signo
    *(int *)(info + 8) = g_sigcode[sig];                  // si_code (SI_QUEUE for sigqueue, else 0)
    *(uint64_t *)(info + 16) = g_sigaddr[sig];            // si_addr (synchronous fault address; 0 for async)
    *(uint64_t *)(info + 24) = g_sigval[sig];             // si_value
    g_sigcode[sig] = 0; g_sigval[sig] = 0; g_sigaddr[sig] = 0; // consumed
    uint64_t rsp = sp - 8;
    *(uint64_t *)rsp = SIGRETURN_PC; // pushed return address
    c->r[7] = (uint64_t)sig;
    c->r[6] = info;
    c->r[2] = uc; // handler(signo, siginfo*, ucontext*) in rdi,rsi,rdx
    c->r[4] = rsp;
    c->rip = g_sigact[sig].handler;
    c->sigmask |= g_sigact[sig].mask;
    if (!(g_sigact[sig].flags & 0x40000000)) c->sigmask |= (1ull << (sig - 1)); // SA_NODEFER off -> block this signal
    if (g_trace)
        fprintf(stderr, "[sig] deliver %d handler=%llx rsp=%llx\n", sig, (unsigned long long)c->rip,
                (unsigned long long)rsp);
}
static void do_sigreturn(struct cpu *c) {
    uint64_t uc = c->r[4], mc = uc + 40, xs = uc + 768; // after the handler's ret, rsp == uc
    for (int i = 0; i < 16; i++)
        c->r[GREG2R[i]] = *(uint64_t *)(mc + i * 8);
    c->rip = *(uint64_t *)(mc + 16 * 8);
    c->nzcv = eflags_to_nzcv(*(uint64_t *)(mc + 17 * 8));
    c->sigmask = *(uint64_t *)(uc + 296);
    memcpy(c->v, (void *)xs, sizeof c->v);
}

// Synchronous-fault delivery support (driven by os/linux/signal.c's deliver_guest_fault; see the matching
// frontend/aarch64/sigframe.c for the model). In a translated x86 block the 16 guest GPRs rax..r15 live in
// host x0..x15 and xmm0..15 in host v0..v15, with cpu pinned in host x28. Reconstruct the guest GPR/xmm
// state from the host fault context (the deferred-flag NZCV is left as last spilled). block_return
// (frontend/x86_64/translate.c) unwinds the block back to the run_guest loop, which runs cpu->rip == handler.
static int sigframe_capture_fault(struct cpu *c, void *ucv) {
    ucontext_t *uc = (ucontext_t *)ucv;
    uint64_t hpc = (uint64_t)uc->uc_mcontext->__ss.__pc;
    uint64_t lo = (uint64_t)g_cache + g_rw2rx, hi = lo + CACHE_SZ;
    if (hpc < lo || hpc >= hi) return 0; // host PC outside the code cache -> a genuine engine fault
    uint64_t *X = uc->uc_mcontext->__ss.__x;
    for (int i = 0; i < 16; i++)
        c->r[i] = X[i];                                   // rax..r15 == host x0..x15
    memcpy(c->v, uc->uc_mcontext->__ns.__v, sizeof c->v); // xmm0..15 == host v0..v15
    return 1;
}
static void sigframe_resume_dispatch(struct cpu *c, void *ucv) {
    ucontext_t *uc = (ucontext_t *)ucv;
    uc->uc_mcontext->__ss.__x[28] = (uint64_t)c; // block_return reads &cpu from x28 (pinned through the block)
    uc->uc_mcontext->__ss.__pc = (uint64_t)block_return;
}
