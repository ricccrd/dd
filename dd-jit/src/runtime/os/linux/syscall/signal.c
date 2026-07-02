// Extracted from service(): Signals syscalls. Returns 1 if nr was handled, 0 otherwise. Included by service.c
// after service/helpers.c, before service() — same TU scope (globals + helpers).
// Linux sigaction sa_flags bit (asm-generic, shared by x86-64 + aarch64): a handler installed with
// SA_RESTART asks the kernel to transparently restart a slow syscall it interrupts, rather than failing
// it with EINTR. We record the flag in g_sigact[sig].flags (rt_sigaction, case 134) and consult it here.
#define SA_RESTART_L 0x10000000ull

// Decide whether an interruptible host syscall that just returned EINTR (a host signal fired and
// host_sigh raised a g_pending bit) should be auto-restarted for the guest. POSIX rule: restart iff
// EVERY signal that is pending-and-deliverable now (has a real guest handler and is not blocked by the
// thread mask) was installed with SA_RESTART; if any such handler lacks SA_RESTART the guest must see
// EINTR. With nothing deliverable pending (a SIG_DFL/IGN that host_sigh already actioned, or a spurious
// EINTR) we restart too -- there is no SA_RESTART-less handler whose contract we'd be breaking. The
// awaited handler stays pending and is delivered by the dispatcher's maybe_deliver_signal once the
// restarted syscall finally returns.
static int syscall_should_restart(struct cpu *c) {
    if (__atomic_load_n(&c->exited, __ATOMIC_SEQ_CST)) return 0; // execve teardown: don't re-block, unwind out
    uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST);
    for (int s = 1; s <= 64; s++) {
        uint64_t bit = 1ull << s;
        if (!(p & bit)) continue;
        if (c->sigmask & (1ull << (s - 1))) continue; // blocked -> not delivered now
        if (g_sigact[s].handler <= 1) continue;       // SIG_DFL/IGN -> no guest handler runs
        if (!(g_sigact[s].flags & SA_RESTART_L)) return 0;
    }
    return 1;
}
// An interruptible host syscall failed: should the caller retry it? True iff it was interrupted (EINTR)
// by a signal whose guest handler asked for SA_RESTART (syscall_should_restart). Use as the tail of a
// do/while around the blocking host call so the result variable stays local to each call site.
#define SVC_EINTR_RESTART(c) (errno == EINTR && syscall_should_restart(c))

// ---- #146: EINTR for a BLOCKING "never-restarted" syscall (poll/ppoll/pselect/epoll_pwait) ----
// Per signal(7) these calls are in the set that is NEVER restarted: when a signal HANDLER interrupts them
// they ALWAYS return EINTR, regardless of SA_RESTART -- the handler runs and the syscall does not restart.
// The old SVC_EINTR_RESTART do/while got this wrong: whenever a pending handler had SA_RESTART it restarted
// the host call IN PLACE, which (a) is the wrong semantics (they must return EINTR) and, worse, (b) never
// delivered the handler because it never returned to the dispatcher -- so a forever-blocking call
// (pause()->ppoll(NULL,0,NULL), poll(NULL,0,-1)) plus an SA_RESTART SIGCHLD reaper hung forever (#146).
//
// Correct rule: keep retrying in place ONLY for a SPURIOUS EINTR with nothing to deliver -- an internal/host
// wakeup, or a SIG_DFL/IGN the host already actioned, which a real kernel would not surface to the guest at
// all. The instant a real guest handler is runnable, STOP looping and let the syscall return -EINTR; the
// dispatcher's maybe_deliver_signal then runs the handler (after the syscall returns) and the guest sees
// EINTR -- exactly like Linux. Returns 1 to RETRY the host call, 0 to let it return.
static int svc_poll_retry(struct cpu *c) {
    if (errno != EINTR) return 0;                                // a genuine error -> let it propagate
    if (__atomic_load_n(&c->exited, __ATOMIC_SEQ_CST)) return 0; // execve teardown: stop re-blocking, unwind out
    uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST) | __atomic_load_n(&c->tpending, __ATOMIC_SEQ_CST);
    for (int s = 1; s <= 64; s++) {
        if (!(p & (1ull << s))) continue;
        if (c->sigmask & (1ull << (s - 1))) continue; // blocked -> not delivered now
        if (g_sigact[s].handler > 1) return 0;        // a runnable guest handler -> return EINTR + deliver it
    }
    return 1; // nothing deliverable -> hide this EINTR and re-block (spurious/internal wakeup)
}

// Route a thread-directed signal (tkill/tgkill at `tid`). If it names ANOTHER live thread and the signal
// has a real guest handler, deliver it to exactly that thread (its cpu->tpending) -- a process-wide
// g_pending would let any thread (typically the sender) consume it, which breaks Go's stop-the-world
// preemption (sysmon tgkill's a worker with SIGURG and must stop THAT worker). Otherwise -- self-signal,
// default/ignore disposition, or an unknown/dead tid -- fall back to the existing process-directed path on
// the caller (raise_guest_signal applies the default/ignore action or coalesces into g_pending as before).
static void thread_kill(struct cpu *c, int tid, int sig) {
    if (sig >= 1 && sig <= 64 && tid != cpu_tid(c) && g_sigact[sig].handler > 1 && thread_target_signal(tid, sig))
        return;
    raise_guest_signal(c, sig);
}

static int svc_signal(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (nr) {
    // ===================== Signals — Linux signal numbers -> macOS; kill/sigaction/sigreturn =====================
    // kill(pid,sig)
    case 129:
        if ((int)a0 == container_pid() || (int)a0 <= 0) {
            raise_guest_signal(c, (int)a1);
            G_RET(c) = 0;
        // self / pgrp (PID-ns aware)
        }
        else
            // Cross-process: the target is another dd engine whose host_sigh is installed on the MACOS signal
            // number (rt_sigaction, case 134, installs on sig_l2m(sig)); its host_sigh translates back via
            // sig_m2l. So the sender MUST translate Linux->macOS here too -- else a divergent signal (SIGUSR1=10,
            // SIGUSR2=12, SIGURG=23, ... differ between Linux and macOS) lands on the wrong disposition and is
            // lost. This is exactly the postgres fast-shutdown deadlock: the postmaster's kill(checkpointer,
            // SIGUSR2=12) was delivered as macOS 12 (SIGSYS), the checkpointer never ran ShutdownXLOG, and
            // `pg_ctl -w stop` hung ("server does not shut down"). sig 0 (existence check) maps to 0 unchanged.
            G_RET(c) = kill((pid_t)a0, sig_l2m((int)a1)) < 0 ? (uint64_t)(-errno) : 0;
        break;
    case 130:
        // tkill(tid, sig)
        thread_kill(c, (int)a0, (int)a1);
        G_RET(c) = 0;
        break;
    case 131:
        // tgkill(tgid, tid, sig)
        thread_kill(c, (int)a1, (int)a2);
        G_RET(c) = 0;
        break;
    case 138: { // rt_sigqueueinfo(tgid, sig, siginfo): carry si_code + si_value to the handler's siginfo
        int sig = (int)a1;
        if (sig >= 1 && sig <= 64 && a2) {
            g_sigcode[sig] = *(int *)(a2 + 8);     // siginfo.si_code
            g_sigval[sig] = *(uint64_t *)(a2 + 24); // siginfo.si_value (sival_int/ptr)
        }
        raise_guest_signal(c, sig);
        G_RET(c) = 0;
        break;
    }
    // sigaltstack(new, old)
    case 132: {
        if (a1) {
            // report current (or SS_DISABLE=2 if none)
            *(uint64_t *)(a1 + 0) = c->alt_sp;
            *(uint32_t *)(a1 + 8) = c->alt_sp ? c->alt_flags : 2;
            *(uint64_t *)(a1 + 16) = c->alt_size;
        }
        if (a0) {
            c->alt_sp = *(uint64_t *)(a0 + 0);
            c->alt_flags = *(uint32_t *)(a0 + 8);
            c->alt_size = *(uint64_t *)(a0 + 16);
        }
        G_RET(c) = 0;
        break;
    }
#if G_O_DIRECTORY == 0x10000
    // x86-64 pause(34): wait until a signal bearing a guest handler (unblocked under the CURRENT mask) is
    // pending, then return -EINTR (the dispatcher delivers the handler). It has no aarch64 syscall number --
    // asm-generic libcs lower pause() to ppoll(NULL,0,NULL) (handled by case 73), but x86-64 glibc issues the
    // real pause syscall, which arrived UNMAPPED (CANON_X86ONLY|34) and ENOSYS'd -> a guest pause() returned
    // immediately instead of blocking. pause == rt_sigsuspend with the current mask (no mask change); reuse
    // that exact deliver-in-the-dispatcher discipline (case 133) rather than block on a host syscall.
    case 0x10000 | 34: {
        sigset_t allblk, prev, empty;
        sigfillset(&allblk);
        sigemptyset(&empty);
        sigprocmask(SIG_BLOCK, &allblk, &prev); // close the check/sleep race (see case 133)
        while (!c->exited) {
            uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST);
            int deliv = 0;
            for (int s = 1; s <= 64; s++) {
                uint64_t bit = 1ull << s;
                if (!(p & bit) || (c->sigmask & (1ull << (s - 1)))) continue; // not pending / blocked
                if (g_sigact[s].handler <= 1) { // SIG_DFL/IGN: host already actioned -> consume, keep waiting
                    __atomic_and_fetch(&g_pending, ~bit, __ATOMIC_SEQ_CST);
                    continue;
                }
                deliv = 1; // a real guest handler is runnable -> stop waiting (leave it pending to deliver)
                break;
            }
            if (deliv) break;
            sigsuspend(&empty); // sleep until any host signal (host_sigh sets g_pending); EINTR-returns
        }
        sigprocmask(SIG_SETMASK, &prev, NULL);
        G_RET(c) = (uint64_t)(-EINTR);
        break;
    }
#endif
    // rt_sigsuspend(const sigset_t *unewset, size_t sigsetsize): atomically install the guest's arg
    // mask, wait until a signal that has a guest handler (and is unblocked under that mask) becomes
    // pending, then return -EINTR -- the handler runs and only then does sigsuspend "return" (standard
    // semantics). c->sigmask is a guest sigset_t (bit signo-1); g_pending is 1<<signo.
    //
    // We do NOT build the signal frame here: delivery is left to the dispatcher's maybe_deliver_signal,
    // which fires AFTER the per-arch pc advance past the syscall (x86 pre-advances rip, aarch64 does
    // pc+=4 post-service) -- building a frame inline would re-execute the SVC on aarch64. So we leave the
    // awaited signal pending and arrange c->sigmask so the dispatcher delivers it, then restore the
    // pre-suspend mask (minus the one awaited bit, which must stay unblocked for that delivery; that one
    // bit is the only deviation from a perfect mask restore).
    case 133: {
        uint64_t oldmask = c->sigmask;
        uint64_t newmask = a0 ? *(uint64_t *)a0 : 0;
        c->sigmask = newmask;
        // Block all host signals around the pending check so host_sigh cannot fire between the check and
        // the sleep (lost-wakeup race); sigsuspend(&empty) then atomically unblocks + waits.
        sigset_t allblk, prev, empty;
        sigfillset(&allblk);
        sigemptyset(&empty);
        sigprocmask(SIG_BLOCK, &allblk, &prev);
        int deliv = 0;
        while (!c->exited) {
            uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST);
            deliv = 0;
            for (int s = 1; s <= 64; s++) {
                uint64_t bit = 1ull << s;
                if (!(p & bit) || (newmask & (1ull << (s - 1)))) continue; // not pending / blocked
                uint64_t h = g_sigact[s].handler;
                if (h <= 1) { // SIG_DFL/IGN: host already actioned it -> consume, keep waiting
                    __atomic_and_fetch(&g_pending, ~bit, __ATOMIC_SEQ_CST);
                    continue;
                }
                deliv = s; // a real guest handler is runnable -> stop waiting (leave it PENDING)
                break;
            }
            if (deliv) break;
            sigsuspend(&empty); // sleep until any host signal (host_sigh sets g_pending); EINTR-returns
        }
        sigprocmask(SIG_SETMASK, &prev, NULL); // restore the host signal mask
        c->sigmask = oldmask;
        if (deliv) c->sigmask &= ~(1ull << (deliv - 1)); // keep it unblocked so the dispatcher delivers it
        G_RET(c) = (uint64_t)(-EINTR);
        break;
    }
    // rt_sigtimedwait(const sigset_t *set, siginfo_t *info, const struct timespec *timeout, size_t):
    // SYNCHRONOUSLY dequeue one pending signal from `set` (no handler runs) and return its signo, or
    // -EAGAIN on timeout. Poll g_pending against `set` in short slices (the in-process model has no
    // single host primitive that covers both host-delivered and raise_guest_signal-injected pendings).
    case 137: {
        uint64_t set = a0 ? *(uint64_t *)a0 : 0; // guest sigset_t (bit signo-1)
        struct timespec *to = (struct timespec *)a2;
        // negative/zero timeout -> single non-blocking poll; else a deadline.
        long long budget_ns = to ? (long long)to->tv_sec * 1000000000LL + to->tv_nsec : -1;
        long long waited_ns = 0;
        for (;;) {
            uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST);
            int got = 0;
            for (int s = 1; s <= 64; s++)
                if ((p & (1ull << s)) && (set & (1ull << (s - 1)))) { got = s; break; }
            if (got) {
                __atomic_and_fetch(&g_pending, ~(1ull << got), __ATOMIC_SEQ_CST); // dequeue
                if (a1 && a3 >= 128) {                                            // fill siginfo_t
                    memset((void *)a1, 0, 128);
                    *(int *)(a1 + 0) = got;            // si_signo
                    *(int *)(a1 + 8) = g_sigcode[got]; // si_code
                    *(uint64_t *)(a1 + 24) = g_sigval[got];
                    g_sigcode[got] = 0;
                    g_sigval[got] = 0;
                }
                G_RET(c) = (uint64_t)got;
                break;
            }
            if (budget_ns == 0 || (budget_ns > 0 && waited_ns >= budget_ns) || c->exited) {
                G_RET(c) = (uint64_t)(-EAGAIN);
                break;
            }
            struct timespec slice = {0, 2 * 1000 * 1000}; // 2ms
            nanosleep(&slice, NULL);
            waited_ns += 2 * 1000 * 1000;
        }
        break;
    }
    // rt_sigaction(sig, *act, *old)
    case 134: {
        int sig = (int)a0;
        if (sig < 1 || sig > 64) {
            G_RET(c) = (uint64_t)(-22);
            break;
        }
        if (a2) {
            *(uint64_t *)(a2 + 0) = g_sigact[sig].handler;
            *(uint64_t *)(a2 + 8) = g_sigact[sig].flags;
            *(uint64_t *)(a2 + 16) = g_sigact[sig].mask;
        // aarch64: handler,flags,mask
        }
        if (a1) {
            uint64_t h = *(uint64_t *)(a1 + 0);
            g_sigact[sig].handler = h;
            g_sigact[sig].flags = *(uint64_t *)(a1 + 8);
            g_sigact[sig].mask = *(uint64_t *)(a1 + 16);
            // Synchronous CPU faults (SIGILL/FPE/TRAP/SEGV/BUS) ALWAYS stay on the engine's own host guard
            // (installed at startup): it intercepts the hardware fault and either delivers it to the guest
            // handler recorded in g_sigact above (deliver_guest_fault) or applies the default action
            // (decline -> re-raise). We therefore never forward the guest's disposition to the real host
            // for these -- doing so would UNINSTALL the guard, so a later CPU-feature probe that traps an
            // unsupported instruction (OpenSSL SM3/SM4 + a SIGILL handler) would fault fatally instead of
            // reaching its handler. The bug surfaced across execve: a non-PIE parent (rustup) restoring
            // SIGILL to SIG_DFL left the host guard uninstalled for the exec'd child (cargo). Only ASYNC
            // signals touch the real host disposition. (SIGKILL/SIGSTOP are unmaskable.)
            if (sig != 9 && sig != 19 && !sig_is_sync(sig)) {
                // host(macOS) signo to install on
                int ms = sig_l2m(sig);
                if (h == 0)
                    signal(ms, SIG_DFL);
                else if (h == 1)
                    // honor SIG_IGN (e.g. SIGPIPE)
                    signal(ms, SIG_IGN);
                else {
                    // async: flag pending, deliver in dispatcher
                    struct sigaction sa;
                    memset(&sa, 0, sizeof sa);
                    sa.sa_handler = host_sigh;
                    sigfillset(&sa.sa_mask);
                    sigaction(ms, &sa, NULL);
                }
            }
        }
        G_RET(c) = 0;
        break;
    }
    // rt_sigprocmask(how, *set, *old)
    case 135: {
        // (W4F slow-path counter removed: it lived in x86 emit.c, undefined in the shared/aarch64 TU)
        if (a2) *(uint64_t *)a2 = c->sigmask;
        if (a1) {
            uint64_t set = *(uint64_t *)a1;
            if (a0 == 0)
                // SIG_BLOCK
                c->sigmask |= set;
            else if (a0 == 1)
                // SIG_UNBLOCK
                c->sigmask &= ~set;
            else
                c->sigmask = set;
        // SIG_SETMASK
        }
        // Mirror the terminal-stop signals (SIGTSTP/SIGTTIN/SIGTTOU) onto the REAL host mask. Job control
        // depends on this: bash blocks these three around tcsetpgrp/tcsetattr so a process in a BACKGROUND
        // process group can hand the controlling terminal to a new foreground job without itself being
        // stopped (their default action stops the process). The guest runs IN-PROCESS in the engine, so a
        // guest-only mask is invisible to the kernel -- it would deliver SIG_DFL SIGTTOU and STOP the engine
        // mid-handoff, the tcsetpgrp never completes, and every foreground command freezes (the "no job
        // control / Stopped" bug). Only these three need mirroring (only they stop the process on default
        // disposition); all other signals stay on the engine's async host_sigh + c->sigmask delivery model.
        // Fast path: only touch the host mask when THIS call could change a stop-signal's block state -- i.e.
        // SIG_SETMASK (redefines all) or a set that names one of the three -- so the common SIG_BLOCK/UNBLOCK
        // of SIGCHLD/SIGINT/etc. adds zero host syscalls.
        const uint64_t STOPBITS = (1ull << 19) | (1ull << 20) | (1ull << 21); // SIGTSTP|SIGTTIN|SIGTTOU bits
        if (a1 && (a0 == 2 || (*(uint64_t *)a1 & STOPBITS))) {
            static const int STOPS[3] = {20, 21, 22}; // Linux SIGTSTP, SIGTTIN, SIGTTOU
            sigset_t blk, unblk;
            sigemptyset(&blk);
            sigemptyset(&unblk);
            for (int i = 0; i < 3; i++) {
                int ms = sig_l2m(STOPS[i]);
                if (c->sigmask & (1ull << (STOPS[i] - 1))) sigaddset(&blk, ms);
                else sigaddset(&unblk, ms);
            }
            sigprocmask(SIG_BLOCK, &blk, NULL);
            sigprocmask(SIG_UNBLOCK, &unblk, NULL);
        }
        G_RET(c) = 0;
        break;
    }
    // rt_sigpending(set, sigsetsize)
    case 136: {
        uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST), out = 0;
        for (int s = 1; s <= 64; s++)
            // 1<<N -> sigset_t bit N-1
            if (p & (1ull << s)) out |= (1ull << (s - 1));
        if (a0) *(uint64_t *)a0 = out;
        G_RET(c) = 0;
        break;
    }
    case 139:
        do_sigreturn(c);
        c->redirect = 1;
        // rt_sigreturn (restorer path)
        break;
    default: return 0;
    }
    return 1;
}
