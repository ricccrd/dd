// Extracted from service(): Time — clock_gettime/nanosleep/gettimeofday syscalls. Returns 1 if nr was handled, 0 otherwise. Included by service.c
// after service/helpers.c, before service() — same TU scope (globals + helpers).
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
    default: return 0;
    }
    return 1;
}
