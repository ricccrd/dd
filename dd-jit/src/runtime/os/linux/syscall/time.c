// Extracted from service(): Time — clock_gettime/nanosleep/gettimeofday syscalls. Returns 1 if nr was handled, 0 otherwise. Included by service.c
// after service/helpers.c, before service() — same TU scope (globals + helpers).

// ===================== POSIX per-process timers (timer_create/_settime/_gettime/_delete/_getoverrun)
// macOS has no timer_create(2). Emulate Linux POSIX timers with a single shared kqueue holding one
// EVFILT_TIMER per armed timer (kevent ident = our small-int timer id) plus ONE background thread
// blocked in kevent(). On expiry that thread raises the guest signal through the engine's own
// async-signal path -- set the g_pending bit + poke the signalfd self-pipe, byte-for-byte what
// host_sigh() does for a real host signal -- so maybe_deliver_signal() builds the rt_sigframe at the
// next dispatcher boundary. We NEVER raise a real host signal into the MAP_JIT thread. kqueue can't
// express "first fire at it_value, then every it_interval" in one entry, so when those differ we arm
// a one-shot for it_value and the thread re-arms it periodic on the first fire (two-phase). Remaining
// time (timer_gettime) and overrun are tracked in software from the recorded deadline + kevent .data.
#define GTIMER_MAX 32
#define DD_SI_TIMER (-2) // Linux si_code SI_TIMER (the value the guest's siginfo expects)
struct gtimer {
    int used;            // slot allocated by timer_create, not yet timer_delete'd
    int clockid;         // guest clockid (only used to read "now" for a TIMER_ABSTIME arm)
    int notify;          // sigev_notify: SIGEV_SIGNAL(0)/SIGEV_NONE(1)/SIGEV_THREAD(2)/SIGEV_THREAD_ID(4)
    int signo;           // Linux signal number to raise on expiry (SIGEV_SIGNAL/_THREAD_ID)
    uint64_t sigval;     // sigev_value (carried into the delivered siginfo si_value)
    uint64_t interval_ns;// it_interval (0 => one-shot)
    uint64_t next_ns;    // absolute CLOCK_MONOTONIC ns of the next expiry (0 => disarmed)
    int periodic_armed;  // the kqueue entry is already periodic (no re-arm needed on fire)
    int overrun;         // overrun count of the LAST delivered expiry (timer_getoverrun) [atomic]
};
static struct gtimer g_gtimer[GTIMER_MAX];
static int g_gtimer_kq = -1;
static pthread_t g_gtimer_thr;
static int g_gtimer_thr_up;
static pthread_mutex_t g_gtimer_lk = PTHREAD_MUTEX_INITIALIZER;

static uint64_t gtimer_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
// guest clockid -> macOS clockid (only REALTIME vs MONOTONIC matters for the abstime "now" read)
static clockid_t gtimer_hostclock(int clk) { return (clk == 0 || clk == 5) ? CLOCK_REALTIME : CLOCK_MONOTONIC; }

// arm/re-arm slot `id`'s EVFILT_TIMER. Caller holds g_gtimer_lk. value_ns = delay to the next fire
// (0 => fire asap), interval_ns 0 => one-shot. When value==interval we arm a native periodic timer;
// otherwise a one-shot the drain thread promotes to periodic on the first fire.
static int gtimer_arm(int id, uint64_t value_ns, uint64_t interval_ns) {
    struct kevent kv;
    int periodic = (interval_ns > 0 && value_ns == interval_ns);
    uint16_t fl = EV_ADD | (periodic ? 0 : EV_ONESHOT);
    int64_t d = (int64_t)value_ns;
    if (d < 0) d = 0;
    EV_SET(&kv, (uintptr_t)id, EVFILT_TIMER, fl, NOTE_NSECONDS, d, NULL);
    if (kevent(g_gtimer_kq, &kv, 1, NULL, 0, NULL) < 0) return -errno;
    g_gtimer[id].periodic_armed = periodic;
    return 0;
}
static void gtimer_disarm(int id) {
    struct kevent kv;
    EV_SET(&kv, (uintptr_t)id, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
    kevent(g_gtimer_kq, &kv, 1, NULL, 0, NULL); // ENOENT if the one-shot already fired -> ignore
    g_gtimer[id].next_ns = 0;
    g_gtimer[id].periodic_armed = 0;
}
// fill an itimerspec at `out`: it_interval [0..16), it_value=remaining [16..32). A disarmed timer
// (next_ns==0) reports it_value 0 (POSIX), and a periodic timer past its deadline folds into the period.
static void gtimer_fill_curr(struct gtimer *t, void *out) {
    uint64_t iv = t->interval_ns, rem = 0;
    if (t->next_ns) {
        uint64_t now = gtimer_now_ns();
        if (t->next_ns > now) rem = t->next_ns - now;
        else if (iv) { uint64_t past = now - t->next_ns; rem = iv - (past % iv); }
    }
    uint64_t *o = (uint64_t *)out;
    o[0] = iv / 1000000000ull;  o[1] = iv % 1000000000ull;  // it_interval
    o[2] = rem / 1000000000ull; o[3] = rem % 1000000000ull; // it_value (remaining)
}

static void *gtimer_loop(void *arg) {
    (void)arg;
    for (;;) {
        struct kevent ev;
        int n = kevent(g_gtimer_kq, NULL, 0, &ev, 1, NULL);
        if (n < 0) { if (errno == EINTR) continue; break; } // kq closed -> thread exits
        if (n == 0) continue;
        int id = (int)ev.ident;
        if (id < 0 || id >= GTIMER_MAX) continue;
        struct gtimer *t = &g_gtimer[id];
        pthread_mutex_lock(&g_gtimer_lk);
        if (!t->used || t->next_ns == 0) { pthread_mutex_unlock(&g_gtimer_lk); continue; } // raced delete/disarm
        int fires = (int)ev.data; // kqueue coalesces missed fires into .data
        if (fires < 1) fires = 1;
        if (!t->periodic_armed && t->interval_ns > 0) {
            gtimer_arm(id, t->interval_ns, t->interval_ns); // two-phase: promote to periodic now
            t->next_ns = gtimer_now_ns() + t->interval_ns;
        } else if (t->periodic_armed) {
            t->next_ns = gtimer_now_ns() + t->interval_ns;  // advance bookkeeping deadline
        } else {
            t->next_ns = 0;                                 // pure one-shot is done -> disarmed
        }
        __atomic_store_n(&t->overrun, fires - 1, __ATOMIC_SEQ_CST);
        int notify = t->notify, signo = t->signo;
        uint64_t sv = t->sigval;
        pthread_mutex_unlock(&g_gtimer_lk);
        // SIGEV_NONE: pollable only (timer_gettime/_getoverrun) -- the bookkeeping above is enough.
        if (notify == 1) continue;
        if (signo >= 1 && signo <= 64) {
            // carry SI_TIMER + sigev_value into the handler's siginfo (consumed on delivery)
            g_sigcode[signo] = DD_SI_TIMER;
            g_sigval[signo] = sv;
            __atomic_or_fetch(&g_pending, 1ull << signo, __ATOMIC_SEQ_CST);
            if ((g_sigfd_mask & (1ull << signo)) && g_sigfd_pipe[1] >= 0) {
                char b = (char)signo;
                if (write(g_sigfd_pipe[1], &b, 1) < 0) {} // wake signalfd/epoll
            }
        }
    }
    return NULL;
}
// POSIX: per-process timers are NOT inherited across fork(). Drop the inherited table + the now-dead
// kqueue/drain thread so a forked child starts clean (lazy-recreated on its own first timer_create).
static void gtimer_atfork_child(void) {
    memset(g_gtimer, 0, sizeof g_gtimer);
    g_gtimer_kq = -1;
    g_gtimer_thr_up = 0;
    pthread_mutex_init(&g_gtimer_lk, NULL);
}
// lazily bring up the shared kqueue + drain thread. Caller holds g_gtimer_lk.
static int gtimer_init(void) {
    static int reg = 0;
    if (!reg) { pthread_atfork(NULL, NULL, gtimer_atfork_child); reg = 1; }
    if (g_gtimer_kq < 0) {
        g_gtimer_kq = kqueue();
        if (g_gtimer_kq < 0) return -errno;
        fcntl(g_gtimer_kq, F_SETFD, FD_CLOEXEC);
    }
    if (!g_gtimer_thr_up) {
        if (pthread_create(&g_gtimer_thr, NULL, gtimer_loop, NULL) != 0) return -EAGAIN;
        g_gtimer_thr_up = 1;
    }
    return 0;
}

static int svc_time(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (nr) {
    // ===================== Time — clock_gettime/nanosleep/gettimeofday (Linux clock-id translation)
    // =====================
    case 101:
        nanosleep((const struct timespec *)a0, (struct timespec *)a1);
        G_RET(c) = 0;
        // nanosleep
        break;
    case 113: {
        // clock_gettime -- Linux clockid -> macOS
        clockid_t mc;
        switch ((int)a0) {
        case 0:
        // REALTIME(_COARSE)
        case 5: mc = CLOCK_REALTIME; break;
        case 1:
        case 6:
        // MONOTONIC(_COARSE)/BOOTTIME
        case 7: mc = CLOCK_MONOTONIC; break;
        case 2: mc = CLOCK_PROCESS_CPUTIME_ID; break;
        case 3: mc = CLOCK_THREAD_CPUTIME_ID; break;
        case 4: mc = CLOCK_MONOTONIC_RAW; break;
        default: mc = CLOCK_MONOTONIC; break;
        }
        struct timespec ts;
        clock_gettime(mc, &ts);
        uint64_t *g = (uint64_t *)a1;
        if (g) {
            g[0] = ts.tv_sec;
            g[1] = ts.tv_nsec;
        }
        G_RET(c) = 0;
        break;
    }
    case 114: {
        if (a1) {
            *(uint64_t *)a1 = 0;
            *(uint64_t *)(a1 + 8) = 1;
        }
        G_RET(c) = 0;
        break;
    // clock_getres -> 1ns
    }
    case 115: {
        // clock_nanosleep(clockid, flags, request, remain). macOS has no clock_nanosleep, and TIMER_ABSTIME
        // means "sleep UNTIL the absolute deadline" -- treating it as relative would sleep for ~uptime
        // seconds and hang. Emulate ABSTIME by sleeping (deadline - now); relative falls back to nanosleep.
        int flags = (int)a1;
        const struct timespec *req = (const struct timespec *)a2;
        if (flags & 1) { // TIMER_ABSTIME
            clockid_t mc;
            switch ((int)a0) {
                case 2: mc = CLOCK_PROCESS_CPUTIME_ID; break;
                case 3: mc = CLOCK_THREAD_CPUTIME_ID; break;
                case 0: case 5: mc = CLOCK_REALTIME; break;
                default: mc = CLOCK_MONOTONIC; break; // 1/4/6/7 -> monotonic
            }
            struct timespec now, d;
            // Loop on EINTR re-reading `now` each pass so a signal can't make the guest under-sleep:
            // recompute the remaining (deadline - now) against the ABSOLUTE deadline, not nanosleep's
            // own relative remainder, so accumulated scheduling slop never shortens the sleep.
            for (;;) {
                clock_gettime(mc, &now);
                d.tv_sec = req->tv_sec - now.tv_sec;
                d.tv_nsec = req->tv_nsec - now.tv_nsec;
                if (d.tv_nsec < 0) { d.tv_sec--; d.tv_nsec += 1000000000L; }
                if (d.tv_sec < 0 || (d.tv_sec == 0 && d.tv_nsec <= 0)) break; // deadline passed
                if (nanosleep(&d, NULL) == 0) break;
                if (errno != EINTR) break; // recompute against the absolute deadline and retry
            }
            G_RET(c) = 0; // absolute sleep has no remainder to report
            break;
        }
        nanosleep(req, (struct timespec *)a3);
        G_RET(c) = 0;
        break;
    }
    // times(struct tms*): real CPU accounting. The Linux + macOS struct tms layouts match (4 clock_t
    // fields), and both count in sysconf(_SC_CLK_TCK) ticks, so the host result drops straight in.
    case 153: {
        struct tms tb;
        clock_t r = times(&tb);
        if (a0) *(struct tms *)a0 = tb;
        G_RET(c) = (uint64_t)r;
        break;
    }
    case 169: {
        struct timeval tv;
        gettimeofday(&tv, 0);
        // gettimeofday
        uint64_t *g = (uint64_t *)a0;
        if (g) {
            g[0] = tv.tv_sec;
            g[1] = tv.tv_usec;
        }
        G_RET(c) = 0;
        break;
    }
    // ===================== POSIX per-process timers (aarch64 nrs; x86 normalized to these) ==========
    // 107 timer_create(clockid, sigevent*, timer_t*) -- allocate a slot, record clock + sigevent.
    case 107: {
        pthread_mutex_lock(&g_gtimer_lk);
        int rc = gtimer_init();
        if (rc < 0) { pthread_mutex_unlock(&g_gtimer_lk); G_RET(c) = (uint64_t)rc; break; }
        int id = -1;
        for (int i = 0; i < GTIMER_MAX; i++) if (!g_gtimer[i].used) { id = i; break; }
        if (id < 0) { pthread_mutex_unlock(&g_gtimer_lk); G_RET(c) = (uint64_t)(-EAGAIN); break; }
        struct gtimer *t = &g_gtimer[id];
        memset(t, 0, sizeof *t);
        t->used = 1;
        t->clockid = (int)a0;
        if (a1) {
            // struct sigevent: sigev_value [0..8), sigev_signo [8..12), sigev_notify [12..16)
            uint64_t sigval; int signo, notify;
            memcpy(&sigval, (void *)a1, 8);
            memcpy(&signo, (void *)(a1 + 8), 4);
            memcpy(&notify, (void *)(a1 + 12), 4);
            t->sigval = sigval; t->signo = signo; t->notify = notify;
        } else {
            // POSIX default: SIGEV_SIGNAL, SIGALRM(14), si_value = timer id
            t->notify = 0; t->signo = 14; t->sigval = (uint64_t)id;
        }
        // SIGEV_THREAD(2) needs to run a guest callback in a fresh guest thread -- not expressible
        // from the host syscall layer. glibc lowers SIGEV_THREAD to SIGEV_THREAD_ID(4)+a real-time
        // signal BEFORE the syscall, so we normally never see raw 2; refuse it honestly rather than
        // accept a timer that would silently never fire.
        if (t->notify == 2) { t->used = 0; pthread_mutex_unlock(&g_gtimer_lk); G_RET(c) = (uint64_t)(-ENOSYS); break; }
        pthread_mutex_unlock(&g_gtimer_lk);
        if (a2) memcpy((void *)a2, &id, 4); // kernel writes the int timer id back
        G_RET(c) = 0;
        break;
    }
    // 110 timer_settime(timerid, flags, new*, old*) -- arm/disarm via the itimerspec.
    case 110: {
        int id = (int)a0;
        if (id < 0 || id >= GTIMER_MAX) { G_RET(c) = (uint64_t)(-EINVAL); break; }
        pthread_mutex_lock(&g_gtimer_lk);
        struct gtimer *t = &g_gtimer[id];
        if (!t->used) { pthread_mutex_unlock(&g_gtimer_lk); G_RET(c) = (uint64_t)(-EINVAL); break; }
        if (a3) gtimer_fill_curr(t, (void *)a3); // report the current setting before re-arming
        // itimerspec: it_interval [0..16), it_value [16..32)
        uint64_t ivs = 0, ivn = 0, vls = 0, vln = 0;
        if (a2) {
            memcpy(&ivs, (void *)a2, 8);          memcpy(&ivn, (void *)(a2 + 8), 8);
            memcpy(&vls, (void *)(a2 + 16), 8);   memcpy(&vln, (void *)(a2 + 24), 8);
        }
        if (vls == 0 && vln == 0) { // it_value all-zero => disarm (regardless of it_interval)
            gtimer_disarm(id);
            t->interval_ns = 0;
            pthread_mutex_unlock(&g_gtimer_lk);
            G_RET(c) = 0;
            break;
        }
        uint64_t interval_ns = ivs * 1000000000ull + ivn;
        uint64_t value_ns;
        if (a1 & 1) { // TIMER_ABSTIME: it_value is an absolute deadline in the timer's clock
            struct timespec cn;
            clock_gettime(gtimer_hostclock(t->clockid), &cn);
            uint64_t cnow = (uint64_t)cn.tv_sec * 1000000000ull + (uint64_t)cn.tv_nsec;
            uint64_t deadline = vls * 1000000000ull + vln;
            value_ns = (deadline > cnow) ? (deadline - cnow) : 0; // past deadline -> fire asap
        } else {
            value_ns = vls * 1000000000ull + vln;
        }
        t->interval_ns = interval_ns;
        t->next_ns = gtimer_now_ns() + value_ns;
        int rc = gtimer_arm(id, value_ns, interval_ns);
        pthread_mutex_unlock(&g_gtimer_lk);
        G_RET(c) = rc < 0 ? (uint64_t)rc : 0;
        break;
    }
    // 108 timer_gettime(timerid, curr*) -- remaining time + interval.
    case 108: {
        int id = (int)a0;
        if (id < 0 || id >= GTIMER_MAX) { G_RET(c) = (uint64_t)(-EINVAL); break; }
        pthread_mutex_lock(&g_gtimer_lk);
        struct gtimer *t = &g_gtimer[id];
        if (!t->used) { pthread_mutex_unlock(&g_gtimer_lk); G_RET(c) = (uint64_t)(-EINVAL); break; }
        if (a1) gtimer_fill_curr(t, (void *)a1);
        pthread_mutex_unlock(&g_gtimer_lk);
        G_RET(c) = 0;
        break;
    }
    // 109 timer_getoverrun(timerid) -- overrun count of the last delivered expiry.
    case 109: {
        int id = (int)a0;
        if (id < 0 || id >= GTIMER_MAX || !g_gtimer[id].used) { G_RET(c) = (uint64_t)(-EINVAL); break; }
        G_RET(c) = (uint64_t)(uint32_t)__atomic_load_n(&g_gtimer[id].overrun, __ATOMIC_SEQ_CST);
        break;
    }
    // 111 timer_delete(timerid) -- disarm + free the slot.
    case 111: {
        int id = (int)a0;
        if (id < 0 || id >= GTIMER_MAX) { G_RET(c) = (uint64_t)(-EINVAL); break; }
        pthread_mutex_lock(&g_gtimer_lk);
        struct gtimer *t = &g_gtimer[id];
        if (!t->used) { pthread_mutex_unlock(&g_gtimer_lk); G_RET(c) = (uint64_t)(-EINVAL); break; }
        gtimer_disarm(id);
        t->used = 0;
        pthread_mutex_unlock(&g_gtimer_lk);
        G_RET(c) = 0;
        break;
    }
    default: return 0;
    }
    return 1;
}
