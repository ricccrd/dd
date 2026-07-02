// Extracted from service(): the long tail of uncommon/low-traffic syscalls + Docker seccomp-profile
// parity -- bpf/userfaultfd/io_uring/ptrace (-EPERM like the default profile), flock, mq_*, sched_*,
// {get,set}itimer, cap{get,set}, adjtimex, fs id setters, getcpu, readahead, etc. Returns 1 if nr was
// handled, 0 otherwise. Included by service.c AFTER its local helpers (svc_adjtimex/pidfd_*/mq_* it
// calls) and before service() -- same TU scope.

static int svc_rare(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5) {
    switch (nr) {
    // ===================== seccomp / sandboxing parity =====================
    // Docker's default seccomp profile BLOCKS these with EPERM (they need elevated caps the container
    // lacks); real Linux returns -EPERM, so a probe sees "Operation not permitted". We don't emulate
    // the feature -- replicate the blocked-syscall result so guests that probe for it agree with Linux.
    case 280: // bpf(2)            -- needs CAP_BPF/CAP_SYS_ADMIN
    case 282: // userfaultfd(2)    -- blocked by default profile
    case 425: // io_uring_setup(2) -- blocked by default profile
        G_RET(c) = (uint64_t)(-EPERM);
        break;
    // seccomp(2): a guest installing its OWN allow-all filter (SECCOMP_SET_MODE_FILTER) self-sandboxes;
    // accept the install as a no-op (we don't actually enforce a BPF filter) so the call SUCCEEDS like
    // on Linux instead of failing with ENOSYS. SET_MODE_STRICT is likewise accepted but not enforced.
    case 277: { // seccomp(op, flags, args)
        unsigned op = (unsigned)a0;
        G_RET(c) = (op == 0 /*STRICT*/ || op == 1 /*FILTER*/) ? 0 : (uint64_t)(-EINVAL);
        break;
    }
    // ptrace(2): no real tracing under the JIT, but a guest calling PTRACE_TRACEME on itself (the
    // common anti-debug / "am I traced?" primitive) just expects success. Accept TRACEME; deny the
    // rest with -EPERM (the same result an unprivileged process gets when it cannot attach).
    case 117: // ptrace(request, pid, addr, data)
        G_RET(c) = (a0 == 0 /*PTRACE_TRACEME*/) ? 0 : (uint64_t)(-EPERM);
        break;

    case 279: { // memfd_create(name, flags) -> an anonymous file: a tmpfile, unlinked immediately
        char tn[] = "/tmp/.ddmemfdXXXXXX";
        int fd = mkstemp(tn);
        if (fd >= 0) {
            unlink(tn);
            if (a1 & 1) fcntl(fd, F_SETFD, FD_CLOEXEC); // MFD_CLOEXEC
        }
        G_RET(c) = fd < 0 ? (uint64_t)(-errno) : (uint64_t)fd;
        break;
    }
    // flock(fd, op): BSD whole-file advisory lock. Serviced on a private companion file (see dd_flock in
    // helpers.c) so it stays INDEPENDENT of fcntl POSIX record locks -- on macOS both would otherwise share
    // one per-vnode lock list and spuriously conflict with each other. Linux LOCK_SH/EX/UN/NB match the host.
    case 32: G_RET(c) = dd_flock((int)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0; break;
    // close_range(first, last, flags): close every fd in [first,last]. CLOSE_RANGE_CLOEXEC(4) sets
    // FD_CLOEXEC instead of closing; CLOSE_RANGE_UNSHARE(2) (file-table unshare) is a no-op here.
    case 436: {
        unsigned first = (unsigned)a0, last = (unsigned)a1;
        int flags = (int)a2;
        if (first > last) {
            G_RET(c) = (uint64_t)(int64_t)(-EINVAL);
            break;
        }
        long maxfd = sysconf(_SC_OPEN_MAX);
        if (maxfd <= 0 || maxfd > 65536) maxfd = 65536;
        if (last >= (unsigned)maxfd) last = (unsigned)maxfd - 1;
        if (!(flags & 4)) engine_fd_vacate_range(first, last); // relocate engine fds out of the actual-close range
        for (unsigned fd = first; fd <= last; fd++) {
            if (flags & 4) { // CLOSE_RANGE_CLOEXEC
                int fl = fcntl((int)fd, F_GETFD);
                if (fl >= 0) fcntl((int)fd, F_SETFD, fl | FD_CLOEXEC);
            } else {
                if (exec_fd_is_engine((int)fd)) continue; // never close a (relocated) live engine fd
                close((int)fd);
            }
        }
        G_RET(c) = 0;
        break;
    }
    // pidfd_open(pid, flags): no macOS pidfd -> back it with a real fd and record the target pid.
    case 434: {
        pid_t pid = (pid_t)a0;
        if (pid <= 0 || (pid != container_pid() && kill(pid, 0) < 0 && errno == ESRCH)) {
            G_RET(c) = (uint64_t)(int64_t)(-ESRCH);
            break;
        }
        int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        pidfd_register(fd, pid);
        G_RET(c) = (uint64_t)fd;
        break;
    }
    // pidfd_send_signal(pidfd, sig, siginfo, flags): resolve the pidfd back to its pid, then deliver.
    // sig 0 is the existence check. Self/own-pgrp signals raise into the guest (mirrors kill, case 129).
    case 424: {
        pid_t pid;
        if (pidfd_lookup((int)a0, &pid) < 0) {
            G_RET(c) = (uint64_t)(int64_t)(-EBADF);
            break;
        }
        int sig = (int)a1;
        if (pid == container_pid() || pid <= 0) {
            if (sig != 0) raise_guest_signal(c, sig);
            G_RET(c) = 0;
        } else {
            // Cross-process: translate Linux->macOS signo (the target dd engine listens on the macOS number;
            // see kill, case 129). Untranslated, a divergent signal (SIGUSR1/2, SIGURG, ...) is lost.
            G_RET(c) = kill(pid, sig_l2m(sig)) < 0 ? (uint64_t)(-errno) : 0;
        }
        break;
    }
    // mq_open(name, oflag, mode, attr): find-or-create the named queue, hand back a real fd bound to it.
    case 180: {
        const char *name = (const char *)a0;
        int oflag = (int)a1;
        const long *at = (const long *)a3; // struct mq_attr: {flags, maxmsg, msgsize, curmsgs, ...}
        if (!name) {
            G_RET(c) = (uint64_t)(int64_t)(-EINVAL);
            break;
        }
        int qi = mq_find(name);
        if (qi < 0) {
            if (!(oflag & 0x40)) { // O_CREAT
                G_RET(c) = (uint64_t)(int64_t)(-ENOENT);
                break;
            }
            for (int i = 0; i < MQ_MAXQ; i++)
                if (!g_mqq[i].used) {
                    qi = i;
                    break;
                }
            if (qi < 0) {
                G_RET(c) = (uint64_t)(int64_t)(-ENOSPC);
                break;
            }
            memset(&g_mqq[qi], 0, sizeof g_mqq[qi]);
            g_mqq[qi].used = 1;
            snprintf(g_mqq[qi].name, sizeof g_mqq[qi].name, "%s", name);
            g_mqq[qi].maxmsg = (at && at[1] > 0) ? at[1] : 10;
            g_mqq[qi].msgsize = (at && at[2] > 0) ? at[2] : 8192;
            if (g_mqq[qi].maxmsg > MQ_MAXMSG) g_mqq[qi].maxmsg = MQ_MAXMSG;
        } else if ((oflag & 0x40) && (oflag & 0x80)) { // O_CREAT | O_EXCL
            G_RET(c) = (uint64_t)(int64_t)(-EEXIST);
            break;
        }
        int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        g_mqq[qi].refs++;
        mq_bind(fd, qi);
        G_RET(c) = (uint64_t)fd;
        break;
    }
    // mq_unlink(name): mark removed; freed once the last descriptor is gone.
    case 181: {
        int qi = mq_find((const char *)a0);
        if (qi < 0) {
            G_RET(c) = (uint64_t)(int64_t)(-ENOENT);
            break;
        }
        g_mqq[qi].unlinked = 1;
        mq_maybe_free(qi);
        G_RET(c) = 0;
        break;
    }
    // mq_timedsend(mqdes, msg, len, prio, abs_timeout): insert highest-priority-first, FIFO within a prio.
    case 182: {
        int qi = mq_qof((int)a0);
        if (qi < 0) {
            G_RET(c) = (uint64_t)(int64_t)(-EBADF);
            break;
        }
        struct mq_queue *q = &g_mqq[qi];
        size_t len = (size_t)a2;
        unsigned prio = (unsigned)a3;
        if ((long)len > q->msgsize) {
            G_RET(c) = (uint64_t)(int64_t)(-EMSGSIZE);
            break;
        }
        if (q->n >= q->maxmsg) {
            G_RET(c) = (uint64_t)(int64_t)(-EAGAIN); // no blocking sender
            break;
        }
        char *buf = malloc(len ? len : 1);
        if (!buf) {
            G_RET(c) = (uint64_t)(int64_t)(-ENOMEM);
            break;
        }
        memcpy(buf, (const void *)a1, len);
        int pos = q->n;
        while (pos > 0 && q->msg[pos - 1].prio < prio) {
            q->msg[pos] = q->msg[pos - 1];
            pos--;
        }
        q->msg[pos].prio = prio;
        q->msg[pos].len = len;
        q->msg[pos].data = buf;
        q->n++;
        G_RET(c) = 0;
        break;
    }
    // mq_timedreceive(mqdes, msg, len, prio*, abs_timeout): pop the head (highest priority, oldest first).
    case 183: {
        int qi = mq_qof((int)a0);
        if (qi < 0) {
            G_RET(c) = (uint64_t)(int64_t)(-EBADF);
            break;
        }
        struct mq_queue *q = &g_mqq[qi];
        if ((long)(size_t)a2 < q->msgsize) {
            G_RET(c) = (uint64_t)(int64_t)(-EMSGSIZE);
            break;
        }
        if (q->n == 0) {
            G_RET(c) = (uint64_t)(int64_t)(-EAGAIN); // no blocking receiver
            break;
        }
        struct mq_qmsg m = q->msg[0];
        for (int j = 1; j < q->n; j++)
            q->msg[j - 1] = q->msg[j];
        q->n--;
        memcpy((void *)a1, m.data, m.len);
        if (a3) *(unsigned *)a3 = m.prio;
        free(m.data);
        G_RET(c) = (uint64_t)m.len;
        break;
    }
    // mq_getsetattr(mqdes, newattr, oldattr): report flags/maxmsg/msgsize/curmsgs; flag-set is ignored.
    case 185: {
        int qi = mq_qof((int)a0);
        if (qi < 0) {
            G_RET(c) = (uint64_t)(int64_t)(-EBADF);
            break;
        }
        struct mq_queue *q = &g_mqq[qi];
        if (a2) {
            long *o = (long *)a2;
            o[0] = 0;
            o[1] = q->maxmsg;
            o[2] = q->msgsize;
            o[3] = q->n;
        }
        G_RET(c) = 0;
        break;
    }
    // setsid(): new session / process-group leader
    case 157: {
        pid_t s = setsid();
        G_RET(c) = s < 0 ? (uint64_t)(-errno) : (uint64_t)s;
        break;
    }
    // scheduling: stub with sane SCHED_OTHER values (real-time priorities aren't offered)
    case 118:                      // sched_setparam
    case 119: G_RET(c) = 0; break; // sched_setscheduler -> ok (ignored)
    case 120: G_RET(c) = 0; break; // sched_getscheduler -> SCHED_OTHER(0)
    case 121:
        if (a1) *(int *)a1 = 0;
        G_RET(c) = 0;
        break;                                                 // sched_getparam -> priority 0
    case 125: G_RET(c) = (a0 == 1 || a0 == 2) ? 99 : 0; break; // sched_get_priority_max: FIFO/RR=99 else 0
    case 126: G_RET(c) = (a0 == 1 || a0 == 2) ? 1 : 0; break;  // sched_get_priority_min: FIFO/RR=1 else 0
    case 127:                                                  // sched_rr_get_interval -> a nominal 100ms slice
        if (a1) {
            ((struct timespec *)a1)->tv_sec = 0;
            ((struct timespec *)a1)->tv_nsec = 100000000L;
        }
        G_RET(c) = 0;
        break;
    // mlockall/munlockall: no macOS equivalent; the guest's "don't swap" intent is a safe no-op
    case 230:
    case 231: G_RET(c) = 0; break;
    // NUMA memory-policy syscalls (mbind/{set,get}_mempolicy/migrate_pages/move_pages). The host is a
    // single NUMA node and these are advisory placement hints, so accept them as permissive no-ops --
    // e.g. R/OpenBLAS calls mbind(2) on its large matrix buffers at startup. (arm64-normalized numbers;
    // x86-64 237/238/239/256/279 are mapped to these by sysmap.h.)
    case 235: G_RET(c) = 0; break; // mbind          -> success, no-op
    case 237: G_RET(c) = 0; break; // set_mempolicy  -> success, no-op
    case 238:                      // migrate_pages
    case 239: G_RET(c) = 0; break; // move_pages     -> success, no-op (nothing moved)
    case 450: G_RET(c) = 0; break; // set_mempolicy_home_node -> success, no-op (same NUMA-hint family)
    // get_mempolicy(mode*, nodemask, maxnode, addr, flags): report the default policy. If the guest
    // passed a mode pointer, write MPOL_DEFAULT(0) -- but validate it first (host_addr_mapped, thread.c)
    // so a bad pointer returns -EFAULT to the guest rather than faulting the engine.
    case 236: {
        int *mode = (int *)a0;
        if (mode) {
            if (!host_addr_mapped((uintptr_t)mode)) {
                G_RET(c) = (uint64_t)(int64_t)(-EFAULT);
                break;
            }
            *mode = 0; // MPOL_DEFAULT
        }
        G_RET(c) = 0;
        break;
    }
    // getitimer/setitimer: wrap the host (ITIMER_* + struct itimerval layouts match Linux<->macOS)
    case 102: G_RET(c) = getitimer((int)a0, (struct itimerval *)a1) < 0 ? (uint64_t)(-errno) : 0; break;
    case 103:
        G_RET(c) =
            setitimer((int)a0, (const struct itimerval *)a1, (struct itimerval *)a2) < 0 ? (uint64_t)(-errno) : 0;
        break;
    case 112: G_RET(c) = (uint64_t)(-1); break; // clock_settime: container has no CAP_SYS_TIME -> EPERM
    case 143: G_RET(c) = setregid((gid_t)a0, (gid_t)a1) < 0 ? (uint64_t)(-errno) : 0; break; // setregid
    case 151: G_RET(c) = (uint64_t)cuid(); break; // setfsuid -> previous fsuid (container uid)
    case 152: G_RET(c) = (uint64_t)cgid(); break; // setfsgid -> previous fsgid
    case 168:                                     // getcpu(cpu, node, tcache) -> cpu 0 / node 0
        if (a0) *(unsigned *)a0 = 0;
        if (a1) *(unsigned *)a1 = 0;
        G_RET(c) = 0;
        break;
    case 213: G_RET(c) = 0; break; // readahead: advisory, no-op
    case 274:
        G_RET(c) = 0;
        break; // sched_setattr -> ok (ignored)
    // preadv2/pwritev2: flags (a5) ignored; offset in a3 (pos_high a4 is 0 on LP64)
    case 286: {
        if (memf_get((int)a0)) {
            ssize_t r = memf_preadv(g_memf[(int)a0], (const struct iovec *)a1, (int)a2, (off_t)a3, 0);
            G_RET(c) = r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
            break;
        }
        ssize_t r = preadv((int)a0, (const struct iovec *)a1, (int)a2, (off_t)a3);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 287: {
        if (memf_get((int)a0)) {
            const struct iovec *iv = (const struct iovec *)a1;
            off_t end = (off_t)a3;
            for (int i = 0; i < (int)a2; i++)
                end += iv[i].iov_len;
            if (memf_room_or_spill((int)a0, end)) {
                ssize_t r = memf_pwritev(g_memf[(int)a0], iv, (int)a2, (off_t)a3, 0);
                G_RET(c) = r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
                break;
            }
        }
        ssize_t r = pwritev((int)a0, (const struct iovec *)a1, (int)a2, (off_t)a3);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // waitid(idtype, id, infop, options): host waitid into a macOS siginfo, then hand-build the guest's
    // Linux siginfo (layout + signal/status numbers differ). idtype P_ALL/P_PID/P_PGID (0/1/2) match.
    case 95: {
        siginfo_t si;
        memset(&si, 0, sizeof si);
        int lopt = (int)a3, mopt = 0;
        // Linux wait-option bits -> macOS bits (only WNOHANG/WEXITED share a value)
        if (lopt & 0x00000001) mopt |= WNOHANG;    // WNOHANG
        if (lopt & 0x00000002) mopt |= WSTOPPED;   // Linux WSTOPPED(2) -> macOS WSTOPPED
        if (lopt & 0x00000004) mopt |= WEXITED;    // WEXITED
        if (lopt & 0x00000008) mopt |= WCONTINUED; // Linux WCONTINUED(8) -> macOS WCONTINUED
        if (lopt & 0x01000000) mopt |= WNOWAIT;    // Linux WNOWAIT -> macOS WNOWAIT
        int r = waitid((idtype_t)(int)a0, (id_t)a1, &si, mopt);
        if (r < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        uint8_t *gi = (uint8_t *)a2;
        if (gi) {
            // Linux siginfo_t is 128 bytes; zero it (also the WNOHANG "no child" case -> si_pid stays 0)
            memset(gi, 0, 128);
            if (si.si_pid != 0) {
                int code = si.si_code, status = si.si_status;
                // si_status carries a signal number for kill/dump/stop/cont -> translate macOS->Linux
                if (code == CLD_KILLED || code == CLD_DUMPED || code == CLD_STOPPED || code == CLD_CONTINUED)
                    status = sig_m2l(status);
                *(int *)(gi + 0) = 17;   // si_signo = Linux SIGCHLD
                *(int *)(gi + 4) = 0;    // si_errno
                *(int *)(gi + 8) = code; // si_code (CLD_* values match Linux<->macOS)
                *(int *)(gi + 16) = (int)si.si_pid;
                *(int *)(gi + 20) = (int)si.si_uid;
                *(int *)(gi + 24) = status; // si_status
            }
        }
        G_RET(c) = 0;
        break;
    }
    // truncate(path, length): resolve the guest path through the overlay (same helper execve uses), then
    // truncate by host path. Evict the stat cache so the new size is observed.
    case 45: {
        if (jail_ro_at(-100, (const char *)a0)) {
            G_RET(c) = (uint64_t)(int64_t)(-EROFS);
            break;
        }
        char pb[4200];
        const char *p = xresolve_overlay((const char *)a0, pb, sizeof pb);
        int r = truncate(p, (off_t)a1);
        if (r >= 0) mc_evict(p);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // getrlimit(resource, rlim) / setrlimit(resource, rlim): alias prlimit64 (case 261, svc_fill_rlimit).
    // RLIMIT_STACK(3) reports 8MB, RLIMIT_NOFILE(7) a finite fd cap, everything else unlimited; setrlimit is
    // accepted (no-op).
    case 163:
        if (a1) svc_fill_rlimit((int)a0, (uint64_t *)a1);
        G_RET(c) = 0;
        break;
    case 164:
        G_RET(c) = 0;
        break; // setrlimit -> accepted

    // adjtimex(2)/clock_adjtime(2): read-only query fills struct timex + TIME_OK; setting -> EPERM.
    case 266: { // clock_adjtime(clk_id, timex)
        int r = svc_adjtimex((uint8_t *)a1);
        G_RET(c) = r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
        break;
    }
    case 171: { // adjtimex(timex)
        int r = svc_adjtimex((uint8_t *)a0);
        G_RET(c) = r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
        break;
    }
    // sched_getattr(pid, attr, size, flags): report a SCHED_OTHER profile. Zero the caller's struct, then
    // fill size + sched_policy=SCHED_OTHER(0); nice/priority stay 0. (sched_setattr is case 274, ignored.)
    case 275: {
        if (a1) {
            size_t sz = (size_t)a2;
            if (sz == 0 || sz > 48) sz = 48; // kernel struct sched_attr is 48+ bytes; cap to a sane size
            memset((void *)a1, 0, sz);
            *(uint32_t *)(a1 + 0) = (uint32_t)sz; // sched_attr.size
            *(uint32_t *)(a1 + 4) = 0;            // sched_attr.sched_policy = SCHED_OTHER
        }
        G_RET(c) = 0;
        break;
    }
    // mlock2(addr, len, flags): no macOS equivalent for "keep resident"; safe no-op (like mlockall)
    case 284: G_RET(c) = 0; break;
    // rt_tgsigqueueinfo(tgid, tid, sig, siginfo): thread-targeted sibling of rt_sigqueueinfo (case 138).
    // Carry si_code + si_value to the guest handler's siginfo, then raise the signal to the guest.
    case 240: {
        int sig = (int)a2;
        if (sig >= 1 && sig <= 64 && a3) {
            g_sigcode[sig] = *(int *)(a3 + 8);      // siginfo.si_code
            g_sigval[sig] = *(uint64_t *)(a3 + 24); // siginfo.si_value (sival_int/ptr)
        }
        raise_guest_signal(c, sig);
        G_RET(c) = 0;
        break;
    }

    // x86-only `time` (x86 nr 201, no aarch64 equivalent) — glibc has no vDSO here and issues the raw
    // syscall on a hot path (redis server-cron / per-command clock). Serve it directly: seconds since epoch,
    // optionally written through the result pointer in a0. Without this it spams the unhandled-syscall log
    // on every call (a per-op fprintf + ENOSYS), which both breaks the guest's clock and tanks throughput.
#ifdef CANON_X86ONLY
    case (CANON_X86ONLY | 201): { // time (x86-only nr 201; no aarch64 equivalent) -- redis hot clock path
        time_t t = time(NULL);
        if (a0) *(int64_t *)a0 = (int64_t)t;
        G_RET(c) = (uint64_t)(int64_t)t;
        break;
    }
#endif
    default:
        return 0;
    }
    return svc_done(c); // boundary errno xlate (host macOS -> Linux); see helpers.c svc_done
}
