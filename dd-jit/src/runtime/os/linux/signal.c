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
// rt_sigqueueinfo extras carried to the handler's siginfo: si_code + si_value (consumed on delivery)
static int g_sigcode[65];
static uint64_t g_sigval[65];
// synchronous-fault address carried to the handler's siginfo (si_addr; consumed on delivery, 0 for async)
static uint64_t g_sigaddr[65];
// sentinel lr: handler return -> sigreturn
#define SIGRETURN_PC 0xFFFFFFFFFFF0ull

static int sig_is_sync(int s) {
    return s == 4 || s == 5 || s == 7 || s == 8 || s == 11;
// ILL TRAP BUS FPE SEGV (Linux nums)
}
// Does signal `sig`'s DEFAULT action terminate the process (Term or Core)? False for the signals whose
// default action is ignore (CHLD/CONT/URG/WINCH) or stop (STOP/TSTP/TTIN/TTOU); true for every other
// deliverable signal (HUP/INT/QUIT/TERM/USRn/PIPE/ALRM/SEGV/... and the realtime signals 32..64).
static int sig_default_terminates(int sig) {
    switch (sig) {
    case 17: // SIGCHLD  -- ignore
    case 18: // SIGCONT  -- continue (no-op on delivery)
    case 23: // SIGURG   -- ignore
    case 28: // SIGWINCH -- ignore
    case 19: // SIGSTOP  -- stop
    case 20: // SIGTSTP  -- stop
    case 21: // SIGTTIN  -- stop
    case 22: // SIGTTOU  -- stop
        return 0;
    default: return sig >= 1 && sig <= 64;
    }
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
// per-arch (the host<->guest register model differs): on a synchronous fault inside translated code,
// reconstruct the guest register state from the host fault context (returns 1 iff the faulting host PC is
// in the code cache), and steer the host context back into the dispatcher so a guest handler can run.
static int sigframe_capture_fault(struct cpu *c, void *ucv);
static void sigframe_resume_dispatch(struct cpu *c, void *ucv);
static void maybe_deliver_signal(struct cpu *c) {
    // Two sources: g_pending (process-directed -- any thread may take it) and c->tpending (thread-directed
    // via tkill/tgkill -- only THIS thread). Consider both; coalescing a process- and thread-directed
    // instance of the same (non-realtime) signal into one delivery is the correct Linux semantics.
    uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST) | __atomic_load_n(&c->tpending, __ATOMIC_SEQ_CST);
    for (int sig = 1; sig <= 64; sig++) {
        uint64_t bit = 1ull << sig;
        // sigmask is sigset_t (bit N-1)
        if (!(p & bit) || (c->sigmask & (1ull << (sig - 1)))) continue;
        uint64_t h = g_sigact[sig].handler;
        if (h <= 1) {
            // No guest handler -- clear this pending instance from both queues.
            __atomic_and_fetch(&g_pending, ~bit, __ATOMIC_SEQ_CST);
            __atomic_and_fetch(&c->tpending, ~bit, __ATOMIC_SEQ_CST);
            // A SIG_DFL signal whose default action TERMINATES, still pending at the container init, was NOT
            // already actioned by the host: real Linux protects a PID-namespace init from an unhandled fatal
            // signal, so it lingered (e.g. the guest blocked it inside its handler, reset the disposition to
            // SIG_DFL, then re-raised it to exit -- exactly node's SignalExit / mongosh path). dd's init is
            // just the container entrypoint, not an init that must survive, so take the default action and end
            // the container with 128+signo (the code `docker run` reports for a PID 1 killed by a signal).
            // SIG_IGN (h==1) and the default-ignore/stop signals stay dropped here.
            if (h == 0 && container_pid() == 1 && sig_default_terminates(sig)) {
                c->exited = 1;
                c->exit_code = 128 + sig;
                return;
            }
            continue;
        }
        // Claim from both queues (clear unconditionally so the coalesced signal is delivered exactly once),
        // then run the guest handler on this thread.
        uint64_t had_t = __atomic_fetch_and(&c->tpending, ~bit, __ATOMIC_SEQ_CST) & bit;
        uint64_t had_p = __atomic_fetch_and(&g_pending, ~bit, __ATOMIC_SEQ_CST) & bit;
        if (had_t || had_p) {
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
    // Unhandled fatal signal aimed at the container init: real Linux would protect a PID-namespace init and
    // drop it, but dd's init is just the entrypoint -- take the default action and end the container with
    // 128+signo (what `docker run` reports for a PID 1 killed by a signal) rather than raising a real host
    // signal that kills the engine BY the signal. The stop signals keep the host path below (job control
    // mirrors them onto the host mask, so a real host stop is the correct default action).
    if (container_pid() == 1 && sig_default_terminates(sig)) {
        c->exited = 1;
        c->exit_code = 128 + sig;
        return;
    }
    signal(sig_l2m(sig), SIG_DFL);
    // default: a non-init guest process IS the engine process -- a real host signal both terminates it and
    // yields the correct WIFSIGNALED status to its parent's waitpid (host signo).
    raise(sig_l2m(sig));
    c->exited = 1;
    // fallback if raise returns / signo invalid on host
    c->exit_code = 128 + sig;
}

// A synchronous CPU fault (SIGSEGV/SIGBUS) taken inside translated code is the GUEST's own fault. If the
// guest installed a handler for it, reconstruct the guest register state from the host fault context,
// synthesize the Linux siginfo (si_addr = the guest fault address), queue the signal, and steer the host
// context back into the dispatcher so the handler runs and sigreturn/siglongjmp resumes. Called from the
// per-arch SIGSEGV/SIGBUS guard AFTER its own engine-managed fixups (non-PIE data-ref / SMC / lazy map)
// decline. `hostsig` is the macOS signo; returns 1 iff the fault was routed to a guest handler.
//
// We deliberately do NOT build the guest sigframe here: this host handler runs on the faulting thread's
// stack, which on the aarch64 frontend IS the guest stack (the block's host SP == guest SP), so writing the
// frame inline would clobber the live handler stack. Instead we mark the signal pending and hand control
// back to run_guest -- its maybe_deliver_signal builds the frame in the engine's own stack context (the
// exact, already-tested async-delivery path). A synchronous fault cannot be ignored or masked, so force it
// deliverable first.
static int deliver_guest_fault(int hostsig, siginfo_t *si, void *ucv) {
    int sig = sig_m2l(hostsig);
    if (sig < 1 || sig > 64 || !ucv) return 0;
    // SIG_DFL/SIG_IGN: not the guest's to handle -> let the guard re-raise (a real crash).
    if (g_sigact[sig].handler <= 1) return 0;
    struct cpu *c = (struct cpu *)pthread_getspecific(g_cpu_key);
    if (!c) return 0;
    // Not a fault inside translated code -> a genuine engine fault; never mask it as a guest signal.
    if (!sigframe_capture_fault(c, ucv)) return 0;
    g_sigaddr[sig] = si ? (uint64_t)si->si_addr : 0;
    // Linux si_code for a hardware fault: SIGBUS -> BUS_ADRERR(2), else SEGV_MAPERR(1).
    g_sigcode[sig] = (sig == 7) ? 2 : 1;
    c->sigmask &= ~(1ull << (sig - 1)); // a sync fault forces delivery even if the guest blocked it
    c->reason = R_BRANCH;               // resume as a plain branch (no stale syscall/special-op handling)
    __atomic_or_fetch(&g_pending, 1ull << sig, __ATOMIC_SEQ_CST);
    sigframe_resume_dispatch(c, ucv);
    return 1;
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
