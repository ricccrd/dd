// dd/runtime/os/linux -- signal delivery (Linux<->macOS signal-number translation; sigframe build).

// ---------------- signals ----------------
// Handlers are process-wide; the blocked mask is per-thread (cpu->sigmask).
// Async signals set a pending bit from a tiny host handler; the dispatcher then
// builds a Linux rt_sigframe on the guest stack and redirects to the handler.
static struct {
    uint64_t handler, flags, mask;
} g_sigact[65];
// bitmask of pending signals (1<<signo)
static volatile uint64_t g_pending;
// sentinel lr: handler return -> sigreturn
#define SIGRETURN_PC 0xFFFFFFFFFFF0ull

static int sig_is_sync(int s) {
    return s == 4 || s == 5 || s == 7 || s == 8 || s == 11;
// ILL TRAP BUS FPE SEGV (Linux nums)
}
// Signal numbers diverge: Linux SIGUSR1=10/CHLD=17/BUS=7/SYS=31/USR2=12/URG=23/IO=29/STOP=19/
// CONT=18/TSTP=20 vs macOS 30/20/10/12/31/16/23/17/19/18. Translate at the host boundary.
static int sig_l2m(int s) {
    static const unsigned char T[32] = {0,  1,  2,  3,  4,  5,  6,  10, 8,  9,  30, 11, 31, 13, 14, 15,
                                        16, 20, 19, 17, 18, 21, 22, 16, 24, 25, 26, 27, 28, 23, 30, 12};
    return (s >= 1 && s <= 31) ? T[s] : s;
}
static int sig_m2l(int s) {
    static const unsigned char T[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  7,  11, 31, 13, 14, 15,
                                        23, 19, 20, 18, 17, 21, 22, 29, 24, 25, 26, 27, 28, 29, 10, 12};
    return (s >= 1 && s <= 31) ? T[s] : s;
}
// signalfd self-pipe (write end poked from host_sigh)
static int g_sigfd_pipe[2] = {-1, -1};
// its read end (the guest's signalfd)
static int g_sigfd_read = -1;
// signals routed to the signalfd (1<<signo)
static volatile uint64_t g_sigfd_mask;
static void host_sigh(int sig) {
    // host(macOS) signo -> Linux
    int ls = sig_m2l(sig);
    __atomic_or_fetch(&g_pending, 1ull << ls, __ATOMIC_SEQ_CST);
    if ((g_sigfd_mask & (1ull << ls)) && g_sigfd_pipe[1] >= 0) {
        char b = (char)ls;
        if (write(g_sigfd_pipe[1], &b, 1) < 0) {}
    // wake signalfd/epoll
    }
}

// build_signal_frame + do_sigreturn are per-arch (the sigframe register layout) -> frontend/<arch>/sigframe.c
static void build_signal_frame(struct cpu *c, int sig);
static void do_sigreturn(struct cpu *c);
static void maybe_deliver_signal(struct cpu *c) {
    uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST);
    for (int sig = 1; sig <= 64; sig++) {
        uint64_t bit = 1ull << sig;
        // sigmask is sigset_t (bit N-1)
        if (!(p & bit) || (c->sigmask & (1ull << (sig - 1)))) continue;
        uint64_t h = g_sigact[sig].handler;
        if (h <= 1) {
            __atomic_and_fetch(&g_pending, ~bit, __ATOMIC_SEQ_CST);
            continue;
        // DFL/IGN: host did it
        }
        if (__atomic_fetch_and(&g_pending, ~bit, __ATOMIC_SEQ_CST) & bit) {
            build_signal_frame(c, sig);
            return;
        }
    }
}
// A signal aimed at our own process (raise/abort/pthread_kill). Deliver it through our
// own machinery instead of a real host signal (host signals into a MAP_JIT thread are
// fragile): a guest handler -> pending bit; otherwise apply the default action here.
static void raise_guest_signal(struct cpu *c, int sig) {
    if (sig < 1 || sig > 64) return;
    uint64_t h = g_sigact[sig].handler;
    if (h > 1) {
        __atomic_or_fetch(&g_pending, 1ull << sig, __ATOMIC_SEQ_CST);
        return;
    // custom handler
    }
    // SIG_IGN
    if (h == 1) return;
    // blocked: make pending (signalfd / deliver on unblock)
    if (c && (c->sigmask & (1ull << (sig - 1)))) {
        __atomic_or_fetch(&g_pending, 1ull << sig, __ATOMIC_SEQ_CST);
        if ((g_sigfd_mask & (1ull << sig)) && g_sigfd_pipe[1] >= 0) {
            char b = (char)sig;
            if (write(g_sigfd_pipe[1], &b, 1) < 0) {}
        }
        return;
    }
    // SIGCHLD/CONT/URG/WINCH: ignore
    if (sig == 17 || sig == 18 || sig == 23 || sig == 28) return;
    signal(sig_l2m(sig), SIG_DFL);
    // default: die BY the signal (host signo)
    raise(sig_l2m(sig));
    c->exited = 1;
    // fallback if raise returns / signo invalid on host
    c->exit_code = 128 + sig;
}

// Linux mmap flags -> macOS.
static int mmap_flags(int lf) {
    int f = 0;
    if (lf & 0x01) f |= MAP_SHARED;
    if (lf & 0x02) f |= MAP_PRIVATE;
    if (lf & 0x10) f |= MAP_FIXED;
    if (lf & 0x20) f |= MAP_ANON;
    return f;
}
