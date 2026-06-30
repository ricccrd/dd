// Extracted from service(): Event loop -- epoll/eventfd2/timerfd/signalfd4/inotify, emulated on macOS
// kqueue/pipes. Returns 1 if nr was handled, 0 otherwise. Included by service.c after service/net.c,
// before service() -- same TU scope (shares io.c/signal.c fd-redirection state).

// struct epoll_event has a DIFFERENT layout per guest arch: x86-64 forces __attribute__((packed)) so it
// is 12 bytes with `data` at offset 4; every other arch (aarch64/asm-generic) leaves it naturally aligned
// at 16 bytes (4 bytes pad after the u32 events, then `data` at offset 8). Derive both from the same
// G_O_DIRECTORY discriminator io.c uses, so epoll_ctl reads `data` and epoll_pwait writes the out-array at
// the stride/offset the guest's libc/runtime expects (Go's netpoller stores a pointer in `data`).
#if G_O_DIRECTORY == 0x10000
#define G_EPEV_SZ 12u   // x86-64 (packed)
#define G_EPEV_DOFF 4u
#else
#define G_EPEV_SZ 16u   // aarch64 / asm-generic
#define G_EPEV_DOFF 8u
#endif

static int svc_event(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                     uint64_t a4, uint64_t a5) {
    switch (nr) {
    // ===================== Event loop — epoll/eventfd/timerfd/signalfd/inotify (macOS kqueue) =====================
    // eventfd2(initval, flags) -> pipe
    case 19: {
        int fds[2];
        if (pipe(fds) < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        if (a1 & 0x80000) {
            fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            fcntl(fds[1], F_SETFD, FD_CLOEXEC);
            // EFD_CLOEXEC
        }
        if (a1 & 0x800) {
            fcntl(fds[0], F_SETFL, O_NONBLOCK);
            fcntl(fds[1], F_SETFL, O_NONBLOCK);
            // EFD_NONBLOCK
        }
        // writes to the eventfd go to fds[1]; the counter + sema-flag live alongside.
        if (fds[0] < 1024 && fds[1] < 1024) {
            g_eventfd_peer[fds[0]] = fds[1] + 1;
            g_eventfd_sema[fds[0]] = (a1 & 1) != 0; // EFD_SEMAPHORE
            g_eventfd_count[fds[0]] = a0;           // initval
            if (a0 > 0) {
                char b = 1;
                if (write(fds[1], &b, 1) < 0) {}
            } // make it readable
        }
        G_RET(c) = (uint64_t)fds[0];
        break;
    }
    case 20: {
        // epoll_create1(flags) -> kqueue
        int r = kqueue();
        // EPOLL_CLOEXEC
        if (r >= 0 && (a0 & 0x80000)) fcntl(r, F_SETFD, FD_CLOEXEC);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // epoll_ctl(epfd, op, fd, event) -> kevent
    case 21: {
        int op = (int)a1, fd = (int)a2;
        uint32_t ev = 0;
        uint64_t data = (uint64_t)(unsigned)fd;
        if (a3) {
            ev = *(uint32_t *)a3;
            memcpy(&data, (void *)(a3 + G_EPEV_DOFF), 8);
            // struct epoll_event {u32 events; [pad;] u64 data} -- layout per guest arch (see G_EPEV_*)
        }
        // op: 1=ADD 2=DEL 3=MOD ; EPOLLET=0x80000000 -> EV_CLEAR ; EPOLLONESHOT=0x40000000 -> EV_ONESHOT
        uint16_t xf = (uint16_t)((ev & 0x80000000u ? EV_CLEAR : 0) | (ev & 0x40000000u ? EV_ONESHOT : 0));
        int want_rd = (op != 2) && (ev & 0x1); // EPOLLIN
        int want_wr = (op != 2) && (ev & 0x4); // EPOLLOUT
        if (epopt_on() && (int)a0 >= 0 && (int)a0 < 1024 && fd >= 0 && fd < 1024) {
            // W3E fast path: track armed filters, defer the change to the next epoll_wait kevent().
            int ep = (int)a0;
            if (want_rd) {
                ep_push(ep, fd, EVFILT_READ, EV_ADD | xf, (void *)data);
                g_ep_rd[fd] = 1;
            } else if (g_ep_rd[fd]) {
                ep_push(ep, fd, EVFILT_READ, EV_DELETE, (void *)data);
                g_ep_rd[fd] = 0;
            }
            if (want_wr) {
                ep_push(ep, fd, EVFILT_WRITE, EV_ADD | xf, (void *)data);
                g_ep_wr[fd] = 1;
            } else if (g_ep_wr[fd]) {
                ep_push(ep, fd, EVFILT_WRITE, EV_DELETE, (void *)data);
                g_ep_wr[fd] = 0;
            }
            g_ep_os[fd] = (op != 2 && (ev & 0x40000000u)) ? 1 : 0;
            G_RET(c) = 0;
            break;
        }
        // ---- original immediate path (NOEPOLLOPT=1 or fd/epfd out of range) ----
        struct kevent kv[2];
        int n = 0;
        uint16_t base = (op == 2) ? EV_DELETE : EV_ADD;
        if (op == 2 || (ev & 0x1)) {
            EV_SET(&kv[n], fd, EVFILT_READ, base | xf, 0, 0, (void *)data);
            n++;
            // EPOLLIN
        }
        if (op == 2 || (ev & 0x4)) {
            EV_SET(&kv[n], fd, EVFILT_WRITE, base | xf, 0, 0, (void *)data);
            n++;
            // EPOLLOUT
        }
        for (int i = 0; i < n; i++) {
            // per-filter so DEL of an absent one is ignored
            kevent((int)a0, &kv[i], 1, NULL, 0, NULL);
            ep_count();
        }
        G_RET(c) = 0;
        break;
    }
    // epoll_pwait(epfd, events, max, timeout_ms, sigmask)
    case 22: {
        int maxev = (int)a2;
        if (maxev > 256) maxev = 256;
        if (maxev < 0) maxev = 0;
        struct kevent kv[256];
        struct timespec ts, *tp = NULL;
        if ((int)a3 >= 0) {
            ts.tv_sec = (int)a3 / 1000;
            ts.tv_nsec = (long)((int)a3 % 1000) * 1000000L;
            tp = &ts;
        }
        uint8_t *out = (uint8_t *)a1;
        int ep = (int)a0;
        int opt = epopt_on() && ep >= 0 && ep < 1024;
        // W3E: submit the deferred changelist together with the wait in ONE kevent() syscall.
        struct kevent *chg = opt ? g_ep_chg[ep] : NULL;
        int nchg = opt ? g_ep_chgn[ep] : 0;
        int r;
        // SA_RESTART: re-wait when interrupted by a restartable handler. kevent applies the changelist
        // before blocking, so a restart re-waits only (changes already consumed -> pass none).
        do {
            r = kevent(ep, chg, nchg, kv, maxev, tp);
            ep_count();
            chg = NULL;
            nchg = 0;
        } while (r < 0 && SVC_EINTR_RESTART(c));
        if (opt) g_ep_chgn[ep] = 0; // consumed
        if (r < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        int oi = 0;
        for (int i = 0; i < r && oi < maxev; i++) {
            // An EV_ERROR entry is a *changelist* processing result (errno in .data), NOT a readiness
            // event. With correct armed-state tracking these do not occur; skip them if they do.
            if (opt && (kv[i].flags & EV_ERROR)) continue;
            uint32_t ev = (kv[i].filter == EVFILT_READ) ? 0x1u : (kv[i].filter == EVFILT_WRITE) ? 0x4u : 0u;
            // EPOLLHUP
            if (kv[i].flags & EV_EOF) ev |= 0x10u;
            // EPOLLERR (immediate-path semantics preserved when opt is off)
            if (!opt && (kv[i].flags & EV_ERROR)) ev |= 0x8u;
            *(uint32_t *)(out + (size_t)oi * G_EPEV_SZ) = ev;
            memcpy(out + (size_t)oi * G_EPEV_SZ + G_EPEV_DOFF, &kv[i].udata, 8);
            // EPOLLONESHOT: the kernel auto-removed this registration; keep our armed map in sync.
            if (opt && kv[i].ident < 1024 && g_ep_os[kv[i].ident]) {
                if (kv[i].filter == EVFILT_READ)
                    g_ep_rd[kv[i].ident] = 0;
                else if (kv[i].filter == EVFILT_WRITE)
                    g_ep_wr[kv[i].ident] = 0;
            }
            oi++;
        }
        // Safety net: if the combined call returned only change-errors (no readiness) yet the guest
        // asked to block, honor the wait with a clean no-change kevent so it can't busy-spin.
        if (opt && oi == 0 && r > 0 && nchg > 0 && (int)a3 != 0) {
            int r2 = kevent(ep, NULL, 0, kv, maxev, tp);
            ep_count();
            if (r2 < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
            }
            for (int i = 0; i < r2 && oi < maxev; i++) {
                if (kv[i].flags & EV_ERROR) continue;
                uint32_t ev = (kv[i].filter == EVFILT_READ) ? 0x1u : (kv[i].filter == EVFILT_WRITE) ? 0x4u : 0u;
                if (kv[i].flags & EV_EOF) ev |= 0x10u;
                *(uint32_t *)(out + (size_t)oi * G_EPEV_SZ) = ev;
                memcpy(out + (size_t)oi * G_EPEV_SZ + G_EPEV_DOFF, &kv[i].udata, 8);
                if (kv[i].ident < 1024 && g_ep_os[kv[i].ident]) {
                    if (kv[i].filter == EVFILT_READ)
                        g_ep_rd[kv[i].ident] = 0;
                    else if (kv[i].filter == EVFILT_WRITE)
                        g_ep_wr[kv[i].ident] = 0;
                }
                oi++;
            }
        }
        G_RET(c) = (uint64_t)oi;
        break;
    }
    case 26: {
        // inotify_init1(flags) -> kqueue
        int r = kqueue();
        if (r >= 0) {
            if (r < 1024) g_inotify[r] = 1;
            if (a0 & 0x800) fcntl(r, F_SETFL, O_NONBLOCK);
            if (a0 & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC);
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // inotify_add_watch(fd, path, mask) -- kqueue EVFILT_VNODE
    case 27: {
        char pb[4200];
        // confined (realpath gate)
        const char *p = atpath(-100, (const char *)a1, pb, sizeof pb, 0);
        int wfd = open(p, O_EVTONLY);
        if (wfd < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        struct kevent kv;
        EV_SET(&kv, wfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB | NOTE_EXTEND, 0, (void *)(intptr_t)wfd);
        if (kevent((int)a0, &kv, 1, NULL, 0, NULL) < 0) {
            int e = errno;
            close(wfd);
            G_RET(c) = (uint64_t)(-(int64_t)e);
            break;
        }
        // a directory watch: remember the path + a snapshot so read() can diff into IN_CREATE/IN_DELETE+name
        struct stat dst;
        if (wfd >= 0 && wfd < 1024 && stat(p, &dst) == 0 && S_ISDIR(dst.st_mode)) {
            snprintf(g_inotify_wpath[wfd], sizeof g_inotify_wpath[wfd], "%s", p);
            free(g_inotify_snap[wfd]);
            g_inotify_snap[wfd] = dir_snapshot(p);
        }
        G_RET(c) = (uint64_t)wfd;
        break;
        // watch descriptor = the watched fd
    }
    case 28: {
        struct kevent kv;
        // inotify_rm_watch(fd, wd)
        EV_SET(&kv, (int)a1, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);
        kevent((int)a0, &kv, 1, NULL, 0, NULL);
        close((int)a1);
        G_RET(c) = 0;
        break;
    }
    case 72: { // pselect6(nfds, readfds, writefds, exceptfds, timeout(timespec), sigmask) -> pselect.
        // The Linux/macOS fd_set byte-layout is identical (bit N at byte N/8), so pass the sets through.
        struct timespec ts, *tsp = NULL;
        if (a4) {
            ts = *(struct timespec *)a4;
            tsp = &ts;
        }
        int r;
        do { r = pselect((int)a0, (fd_set *)a1, (fd_set *)a2, (fd_set *)a3, tsp, NULL); } while (r < 0 && SVC_EINTR_RESTART(c));
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 73: {
        struct pollfd *fds = (void *)a0;
        // ppoll -> poll
        struct timespec *ts = (void *)a2;
        int tmo = ts ? (int)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000) : -1;
        int r;
        do { r = poll(fds, (nfds_t)a1, tmo); } while (r < 0 && SVC_EINTR_RESTART(c));
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // signalfd4(fd, mask, sizemask, flags)
    case 74: {
        // sigset bit (signo-1) -> g_pending bit signo
        uint64_t lm = a1 ? *(uint64_t *)a1 : 0, pm = 0;
        for (int s = 1; s < 64; s++)
            if (lm & (1ull << (s - 1))) pm |= (1ull << s);
        if (g_sigfd_pipe[0] < 0 && pipe(g_sigfd_pipe) < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        g_sigfd_mask |= pm;
        g_sigfd_read = g_sigfd_pipe[0];
        for (int s = 1; s < 64; s++)
            // make sure the host delivers them
            if ((pm & (1ull << s)) && !sig_is_sync(s)) {
                struct sigaction sa;
                memset(&sa, 0, sizeof sa);
                sa.sa_handler = host_sigh;
                sigaction(sig_l2m(s), &sa, NULL);
            }
        // SFD_CLOEXEC
        if (a3 & 0x80000) fcntl(g_sigfd_pipe[0], F_SETFD, FD_CLOEXEC);
        // SFD_NONBLOCK
        if (a3 & 0x800) fcntl(g_sigfd_pipe[0], F_SETFL, O_NONBLOCK);
        G_RET(c) = (uint64_t)g_sigfd_pipe[0];
        break;
    }
    case 85: {
        // timerfd_create(clockid, flags) -> kqueue
        int r = kqueue();
        if (r >= 0) {
            if (r < 1024) g_timerfd[r] = 1;
            if (a1 & 1) fcntl(r, F_SETFL, O_NONBLOCK);
            // TFD_NONBLOCK=1
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // timerfd_settime(fd, flags, new, old)
    case 86: {
        struct kevent kv;
        uint64_t iv_s = 0, iv_n = 0, vl_s = 0, vl_n = 0;
        if (a2) {
            memcpy(&iv_s, (void *)a2, 8);
            memcpy(&iv_n, (void *)(a2 + 8), 8);
            memcpy(&vl_s, (void *)(a2 + 16), 8);
            memcpy(&vl_n, (void *)(a2 + 24), 8);
        }
        // periodic uses it_interval
        int64_t period_ns = (iv_s || iv_n) ? (int64_t)(iv_s * 1000000000ull + iv_n)
                                           // one-shot uses it_value
                                           : (int64_t)(vl_s * 1000000000ull + vl_n);
        if (period_ns <= 0) {
            EV_SET(&kv, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
            kevent((int)a0, &kv, 1, NULL, 0, NULL);
            G_RET(c) = 0;
            break;
            // disarm
        }
        // no interval -> one-shot
        uint16_t fl = EV_ADD | ((iv_s || iv_n) ? 0 : EV_ONESHOT);
        EV_SET(&kv, 1, EVFILT_TIMER, fl, NOTE_NSECONDS, period_ns, NULL);
        G_RET(c) = kevent((int)a0, &kv, 1, NULL, 0, NULL) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 87: {
        if (a1) memset((void *)a1, 0, 32);
        G_RET(c) = 0;
        break;
        // timerfd_gettime -> best-effort 0
    }
    default:
        return 0;
    }
    // Boundary errno translation (mirrors service_local's trailing m2l_errno that this early return bypasses).
    int64_t ev_rv = (int64_t)G_RET(c);
    if (ev_rv < 0 && ev_rv >= -4095) G_RET(c) = (uint64_t)(-(int64_t)m2l_errno((int)(-ev_rv)));
    return 1;
}
