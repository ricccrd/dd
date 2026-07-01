// Extracted from service(): Process & scheduling -- clone/fork/execve/wait/exit, pid/uid/gid identity,
// prctl/futex/caps/sched/affinity. Returns 1 if nr was handled, 0 otherwise. Because its cases call
// service.c-local helpers (nonpie_p/cpu_online_mask/affinity_mask), it is #included after them, before
// service(). NOTE: execve sets c->redirect; svc_done() (the shared tail) skips errno xlate when redirect
// is set, so a redirect's already-Linux G_RET is never re-translated.

// Restore guest GPRs that a per-arch fork/vfork->clone normalization repurposed as clone arguments (the x86
// frontend defines this in legacy.c; other frontends issue clone directly and need no fixup -> no-op).
#ifndef G_FORK_PRESERVE
#define G_FORK_PRESERVE(c) ((void)0)
#endif

// execve env forwarding: serialize the guest's envp array into DD_GUEST_ENV (the "K=V\nK=V..." string
// build_stack reads when laying out the new process stack), so the guest's actual environment crosses the
// re-exec. A NULL envp means the guest passed none -> leave DD_GUEST_ENV (the container defaults) intact.
// Each pointer may be a low non-PIE address, so rebase the array base and every element with nonpie_p(),
// exactly as the argv loop does. setenv() copies the buffer, so it survives the address-space teardown.
static void exec_forward_env(uint64_t envp_guest) {
    if (!envp_guest) return;
    uint64_t *ev = (uint64_t *)nonpie_p(envp_guest);
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) return;
    buf[0] = 0;
    for (int i = 0; ev[i]; i++) {
        const char *e = (const char *)nonpie_p(ev[i]);
        size_t el = strlen(e);
        if (len + el + 2 > cap) {
            cap = (len + el + 2) * 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return;
            }
            buf = nb;
        }
        memcpy(buf + len, e, el);
        len += el;
        buf[len++] = '\n'; // DD_GUEST_ENV record separator (build_stack splits on '\n')
        buf[len] = 0;
    }
    setenv("DD_GUEST_ENV", buf, 1);
    free(buf);
}

// Fill a guest `struct rlimit { rlim_cur; rlim_max; }` for {get,set}rlimit/prlimit64 (cases 163/261).
// Shared so both forms report identical limits. Most resources are unlimited, but a few MUST be finite or
// guests size data structures off them: RLIMIT_STACK(3) reports the conventional 8MB main-stack size, and
// RLIMIT_NOFILE(7) reports a finite fd cap (soft 1024 / hard 1048576, the typical Linux default) -- a guest
// like memcached does calloc(rlim_cur, sizeof(conn)), which overflows if the soft limit is RLIM_INFINITY.
static void svc_fill_rlimit(int resource, uint64_t *o) {
    switch (resource) {
    case 3: // RLIMIT_STACK
        o[0] = 8ull << 20;
        o[1] = ~0ull;
        break;
    case 7: // RLIMIT_NOFILE
        o[0] = 1024;
        o[1] = 1048576;
        break;
    default:
        o[0] = ~0ull; // RLIM_INFINITY
        o[1] = ~0ull;
        break;
    }
}

// Emulate the kernel's close-on-exec sweep. The JIT's execve re-loads the new image IN-PROCESS (no real
// host exec happens -- see case 221), so the kernel never closes FD_CLOEXEC descriptors for us. We must do
// it by hand, or a guest fd the caller opened O_CLOEXEC leaks into the new image. The classic failure this
// fixes: initdb forks `postgres --boot` and feeds it the bootstrap script over a pipe2(O_CLOEXEC); the
// child dup2()s the read end onto stdin and execs, expecting its inherited (CLOEXEC) copy of the WRITE end
// to vanish on exec. Without this sweep that copy survives, so the pipe still has a writer after initdb
// closes its end -> the child's read(stdin) never sees EOF and `running bootstrap script ...` hangs forever.
// Engine-private host fds (the rootfs/volume dir-fds, the timer kqueue, the signal self-pipe) are skipped:
// they back the runtime itself and must survive the emulated exec; closing them would leave dangling fd
// numbers the new guest could reuse, corrupting timer/signal delivery and path confinement.
static int exec_fd_is_engine(int fd) {
    if (fd < 0) return 1;
    if (fd == g_root_fd || fd == g_gtimer_kq || fd == g_sigfd_pipe[0] || fd == g_sigfd_pipe[1]) return 1;
    for (int i = 0; i < g_nvols; i++)
        if (fd == g_vols[i].fd) return 1;
    return 0;
}
// Close the CLOEXEC guest fds among a bounded [0,maxfd) range (proc_pidinfo fallback path only).
static void exec_close_cloexec_scan(int maxfd) {
    if (maxfd < 0 || maxfd > 65536) maxfd = 65536;
    for (int fd = 0; fd < maxfd; fd++) {
        if (exec_fd_is_engine(fd)) continue;
        int fl = fcntl(fd, F_GETFD);
        if (fl >= 0 && (fl & FD_CLOEXEC)) close(fd);
    }
}
#include <libproc.h> // proc_pidinfo(PROC_PIDLISTFDS): enumerate only the OPEN fds (see below)
static void exec_close_cloexec(void) {
    // Sweep only the fds that are actually OPEN, not the whole descriptor table. The daemon raises the
    // soft fd limit very high (getdtablesize() ~= 180K), so the old `for (fd=0; fd<getdtablesize())` scan
    // issued ~180K fcntl(F_GETFD) syscalls -- ~21ms -- on EVERY execve. That single loop dominated the cost
    // of an exec and made process-spawn-heavy guests (make/configure/npm/go/pip fork+exec hundreds to
    // thousands of children) appear to hang: 21ms x thousands of execs = seconds of pure descriptor
    // scanning. PROC_PIDLISTFDS returns just the live descriptors (a couple dozen), so the sweep becomes
    // O(open fds). The real close-on-exec semantics are unchanged: every open non-engine CLOEXEC fd is
    // still closed. Fall back to a bounded linear scan only if proc_pidinfo is unavailable.
    int need = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, NULL, 0);
    if (need <= 0) {
        exec_close_cloexec_scan(getdtablesize());
        return;
    }
    // Over-allocate a little: fds can be opened between the sizing call and the listing call.
    int cap = need + 32 * (int)sizeof(struct proc_fdinfo);
    struct proc_fdinfo *fds = malloc((size_t)cap);
    if (!fds) {
        exec_close_cloexec_scan(getdtablesize());
        return;
    }
    int got = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, fds, cap);
    if (got <= 0) {
        free(fds);
        exec_close_cloexec_scan(getdtablesize());
        return;
    }
    int n = got / (int)sizeof(struct proc_fdinfo);
    for (int i = 0; i < n; i++) {
        int fd = fds[i].proc_fd;
        if (exec_fd_is_engine(fd)) continue;
        int fl = fcntl(fd, F_GETFD);
        if (fl >= 0 && (fl & FD_CLOEXEC)) close(fd);
    }
    free(fds);
}

// ---- runtime credential overlay (USER ns) -------------------------------------------------------
// cuid()/cgid() give the container's CONFIGURED identity (default 0=root); a privileged guest may drop
// to an unprivileged id at runtime (e.g. apt forks /usr/lib/apt/methods/http, which switches to `_apt`
// via setgroups+setresgid/setgid+setresuid/setuid and then VERIFIES the drop took -- and that it can
// NOT regain root). A blanket "set*id always returns 0" left the getters reporting the original id, so
// apt aborted ("cannot switch group"). We track real/effective/saved uid+gid and honour the Linux
// permission model (a euid==0 task is privileged; otherwise a new id must already be one of its three)
// so both the drop AND the regain-must-fail check behave as on Linux. The base is cuid()/cgid(); per
// process (fork inherits the copy, exec re-seeds from the container default), matching the guest's view.
static int g_cred_init = 0;
static int g_ruid, g_euid, g_suid; // real / effective / saved-set uid
static int g_rgid, g_egid, g_sgid; // real / effective / saved-set gid
static void cred_init(void) {
    if (g_cred_init) return;
    g_ruid = g_euid = g_suid = cuid();
    g_rgid = g_egid = g_sgid = cgid();
    g_cred_init = 1;
}
static int cred_euid(void) {
    cred_init();
    return g_euid;
}
static int cred_egid(void) {
    cred_init();
    return g_egid;
}
// An unprivileged task (euid != 0) may only set an id it already holds (real/effective/saved). -1 means
// "leave unchanged". Returns 1 if id is permitted, 0 -> EPERM.
static int uid_permitted(int id) {
    return id == -1 || g_euid == 0 || id == g_ruid || id == g_euid || id == g_suid;
}
static int gid_permitted(int id) {
    return id == -1 || g_euid == 0 || id == g_rgid || id == g_egid || id == g_sgid;
}

static int svc_proc(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5) {
    switch (nr) {
    // ===================== Process & scheduling — clone/exec/wait/ids/prctl/futex/caps/sched =====================
    case 90: {
        if (a1) memset((void *)a1, 0xff, 12);
        G_RET(c) = 0;
        break;
        // capget -> all caps present
    }
    // capset -> ok
    case 91: G_RET(c) = 0; break;
    // chroot(path): re-root the guest WITHIN the rootfs jail. Resolve the target through the active jail to
    // its host backing -- this validates it exists as a directory inside the rootfs and can NEVER name a
    // host path -- then record it as the new chroot prefix. Subsequent absolute guest paths are walked
    // under this prefix yet stay confined to g_root_fd, so the guest cannot escape to the real host fs.
    case 51: {
        char gabs[4200];
        abs_guest(-100, (const char *)nonpie_p(a0), gabs, sizeof gabs); // (AT_FDCWD, path) -> guest-view abs
        char hp[4200];
        const char *h = xresolve_overlay(gabs, hp, sizeof hp); // host backing (honors any chroot already set)
        struct stat st;
        if (stat(h, &st) < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        if (!S_ISDIR(st.st_mode)) {
            G_RET(c) = (uint64_t)(-ENOTDIR);
            break;
        }
        char nc[4200];
        chroot_apply(gabs, nc, sizeof nc);                          // fold under any active chroot -> rootfs-abs
        snprintf(g_chroot, sizeof g_chroot, "%s", nc[1] ? nc : ""); // chroot("/") clears (rootfs IS the root)
        rc_reset(); // drop cached guest->host path mappings -- they predate the re-root
        G_RET(c) = 0;
        break;
    }
    case 93:
        c->exited = 1;
        c->exit_code = (int)a0;
        // exit: end THIS thread
        break;
    // exit_group: end the whole process
    case 94:
        if (getenv("PROF"))
            fprintf(stderr,
                    "[prof] crossings=%llu syscalls=%llu ibtc_miss=%llu branch_cross=%llu translations=%llu lse=%llu "
                    "wx_toggles=%llu dualmap=%d xlate_ms=%.3f mtibtc=%d mtfill=%llu futexq=%d "
                    "fwake_fast=%llu fwake_slow=%llu fwait=%llu\n",
                    (unsigned long long)g_prof_cross, (unsigned long long)g_prof_sys, (unsigned long long)g_prof_miss,
                    (unsigned long long)(g_prof_cross - g_prof_sys - g_prof_miss), (unsigned long long)g_prof_xlate,
                    (unsigned long long)g_lse_n, (unsigned long long)g_wx_toggles, g_dualmap, g_xlate_ns / 1e6,
                    g_mtibtc, (unsigned long long)g_mtfill, g_futexq, (unsigned long long)g_futex_wake_fast,
                    (unsigned long long)g_futex_wake_slow, (unsigned long long)g_futex_wait_n);
        // A3: §B shadow-return coverage. hit-rate = shret_hit / (shret_hit + shret_fb). bl_shadow /
        // bl_leaf show how the depth-gate split call sites at translate time. PROF-only (keep dark).
        if (getenv("PROF")) {
            unsigned long long h = (unsigned long long)g_prof_shret_hit, f = (unsigned long long)g_prof_shret_fb;
            double hr = (h + f) ? 100.0 * (double)h / (double)(h + f) : 0.0;
            fprintf(
                stderr,
                "[prof] shadow_push=%llu shret_hit=%llu shret_fb=%llu hit_rate=%.1f%% bl_shadow=%llu bl_leaf=%llu\n",
                (unsigned long long)g_prof_shpush, h, f, hr, (unsigned long long)g_prof_bl_shadow,
                (unsigned long long)g_prof_bl_leaf);
        }
#ifdef R_REPSTR // W4-C: x86-only rep cmps/scas idiom firing counts
        if (getenv("PROF"))
            fprintf(stderr, "[prof] repstr=%llu repstr_elems=%llu\n", (unsigned long long)g_repstr_n,
                    (unsigned long long)g_repstr_elems);
#endif
#ifdef G_PROF_EXTRA
        G_PROF_EXTRA; // W5B: x86 tier-2 promotion counters
#endif
        ep_prof_dump(); // w3e: flush epoll kevent-syscall counter (atexit is bypassed by _exit)
        ib_dump();      // ARM-B1 IBPROF: indirect-branch traffic + stability report (no-op unless IBPROF)
        vt_dump();      // ARM-B1 VDBETRACE: threading prototype counters (no-op unless VDBETRACE)
        if (g_noexit) { // W3D fork-server prewarm: don't kill the resident parent; unwind run_guest instead
            c->exited = 1;
            c->exit_code = (int)a0;
            break;
        }
#ifdef PCACHE_SAVE_HOOK
        PCACHE_SAVE_HOOK; // opt8: persist the translated arena before the one-shot _exit (DDJIT_PCACHE only)
#endif
        _exit((int)a0);
    case 96:
        G_RET(c) = (uint64_t)getpid();
        // set_tid_address -> returns caller's TID (musl stores it; 0 -> a_crash())
        break;
    case 97:
    // unshare / setns -> ok (no real ns)
    case 268: G_RET(c) = 0; break;
    // futex
    case 98: G_RET(c) = (uint64_t)futex_op((int *)a0, (int)a1 & 0x7f, (int)a2, (struct timespec *)a3); break;
    // set_robust_list
    case 99: G_RET(c) = 0; break;
    // syslog
    case 116: G_RET(c) = 0; break;
    // sched_setaffinity(pid, size, MASK=a2) -- record the requested mask (intersected with the online
    // set) so a later getaffinity reflects the pin; -EINVAL if it selects no online CPU, as on Linux.
    case 122: {
        size_t n = (size_t)a1;
        if (n > sizeof g_affinity) n = sizeof g_affinity;
        if (a2 && n) {
            uint8_t online[sizeof g_affinity];
            cpu_online_mask(online, sizeof online);
            uint8_t want[sizeof g_affinity];
            int any = 0;
            for (size_t i = 0; i < n; i++) {
                want[i] = ((const uint8_t *)a2)[i] & online[i];
                if (want[i]) any = 1;
            }
            if (!any) {
                G_RET(c) = (uint64_t)(-EINVAL);
                break;
            }
            memset(g_affinity, 0, sizeof g_affinity);
            memcpy(g_affinity, want, n);
            g_affinity_set = 1;
        }
        G_RET(c) = 0;
        break;
    }
    case 123: {
        size_t n = (size_t)a1;
        // sched_getaffinity(pid,size,MASK=a2!) -- return the current mask (all online CPUs by default),
        // not just CPU 0, so CPU_COUNT() and tcmalloc's enumeration see the real width (mongod aborts).
        if (n > 128) n = 128;
        if (a2 && n) memcpy((void *)a2, affinity_mask(), n);
        // Return the number of bytes the mask spans (glibc zeroes the remainder); 8 covers <=64 CPUs.
        G_RET(c) = n < 8 ? (uint64_t)n : 8;
        break;
    }
    // sched_yield
    case 124: G_RET(c) = 0; break;
    case 140:
        setpriority((int)a0, (int)a1, (int)a2);
        G_RET(c) = 0;
        // setpriority (best-effort)
        break;
    case 141: {
        errno = 0;
        // getpriority -> Linux raw (20-nice)
        int r = getpriority((int)a0, (int)a1);
        G_RET(c) = (r == -1 && errno) ? (uint64_t)(-errno) : (uint64_t)(20 - r);
        break;
    }
    // setuid(uid): a privileged task sets real+eff+saved; an unprivileged one may only set euid to an id
    // it already holds. Honoured against the credential overlay so apt's _apt drop (and its "can't regain
    // root" check) behave as on Linux. (See cred_init above.)
    case 146: {
        cred_init();
        int u = (int)a0;
        if (!uid_permitted(u)) {
            G_RET(c) = (uint64_t)(-(int64_t)EPERM);
            break;
        }
        if (g_euid == 0)
            g_ruid = g_suid = u;
        g_euid = u;
        G_RET(c) = 0;
        break;
    }
    // setgid(gid): symmetric to setuid above.
    case 144: {
        cred_init();
        int gg = (int)a0;
        if (!gid_permitted(gg)) {
            G_RET(c) = (uint64_t)(-(int64_t)EPERM);
            break;
        }
        if (g_euid == 0)
            g_rgid = g_sgid = gg;
        g_egid = gg;
        G_RET(c) = 0;
        break;
    }
    // setresuid(ruid,euid,suid): each (uid_t)-1 leaves that id unchanged; every requested id must be
    // permitted (privileged, or already held). glibc's seteuid() arrives here as setresuid(-1,euid,-1).
    case 147: {
        cred_init();
        int r = (int)a0, e = (int)a1, s = (int)a2;
        if (!uid_permitted(r) || !uid_permitted(e) || !uid_permitted(s)) {
            G_RET(c) = (uint64_t)(-(int64_t)EPERM);
            break;
        }
        if (r != -1)
            g_ruid = r;
        if (e != -1)
            g_euid = e;
        if (s != -1)
            g_suid = s;
        G_RET(c) = 0;
        break;
    }
    // setresgid(rgid,egid,sgid): symmetric. glibc's setegid() arrives here as setresgid(-1,egid,-1).
    case 149: {
        cred_init();
        int r = (int)a0, e = (int)a1, s = (int)a2;
        if (!gid_permitted(r) || !gid_permitted(e) || !gid_permitted(s)) {
            G_RET(c) = (uint64_t)(-(int64_t)EPERM);
            break;
        }
        if (r != -1)
            g_rgid = r;
        if (e != -1)
            g_egid = e;
        if (s != -1)
            g_sgid = s;
        G_RET(c) = 0;
        break;
    }
    // setreuid(ruid,euid): -1 leaves an id unchanged. The kernel moves saved-uid to the new euid whenever
    // the real uid is changed, or the euid is set to a value other than the previous real uid.
    case 145: {
        cred_init();
        int r = (int)a0, e = (int)a1, old_ruid = g_ruid;
        if (!uid_permitted(r) || !uid_permitted(e)) {
            G_RET(c) = (uint64_t)(-(int64_t)EPERM);
            break;
        }
        if (r != -1)
            g_ruid = r;
        if (e != -1)
            g_euid = e;
        if (r != -1 || (e != -1 && e != old_ruid))
            g_suid = g_euid;
        G_RET(c) = 0;
        break;
    }
    case 148: {
        // getresuid(r,e,s) -- report the overlay so a runtime drop is observed (apt verifies all three).
        cred_init();
        if (a0) *(uint32_t *)a0 = (uint32_t)g_ruid;
        if (a1) *(uint32_t *)a1 = (uint32_t)g_euid;
        if (a2) *(uint32_t *)a2 = (uint32_t)g_suid;
        G_RET(c) = 0;
        break;
    }
    case 150: {
        // getresgid(r,e,s) -- report the overlay (see getresuid above).
        cred_init();
        if (a0) *(uint32_t *)a0 = (uint32_t)g_rgid;
        if (a1) *(uint32_t *)a1 = (uint32_t)g_egid;
        if (a2) *(uint32_t *)a2 = (uint32_t)g_sgid;
        G_RET(c) = 0;
        break;
    }
    // setpgid -- bash job control. The container init has getpid()==1 (container_pid), so bash issues
    // setpgid(0, 1); forwarded verbatim that names launchd (host pid 1) -> EPERM ("initialize_job_control:
    // setpgid: Operation not permitted"). Map the faked PID1 self-reference to the host's own process, and
    // treat a residual EPERM as success -- a container is its own session, so guest process groups are virtual.
    case 154: {
        // Map the guest's view of the init (pid/pgid 1) to its real host pid/group, then do the REAL setpgid.
        // Children already carry real host pids, so they pass straight through and get real process groups.
        // EPERM is benign (the init is a session leader, already its own group leader) -> report success.
        pid_t pid = ((pid_t)a0 == 1 && g_init_hostpid) ? g_init_hostpid : (pid_t)a0;
        pid_t pgid = ((pid_t)a1 == 1 && g_init_hostpid) ? g_init_hostpid : (pid_t)a1;
        int r = setpgid(pid, pgid);
        G_RET(c) = (r < 0 && errno != EPERM) ? (uint64_t)(-errno) : 0;
        break;
    }
    // getpgid / getsid -- translate the init's real host group/session id to the guest's pgid 1 so the guest's
    // identity is self-consistent (getpid 1 == getpgrp 1 == getsid 1). bash then sees itself as session+group
    // leader and initializes job control WITHOUT the setpgid EPERM / "cannot set terminal process group"
    // warning -- it enables job control cleanly, and the real terminal handoff works (see TIOCSPGRP above +
    // the rt_sigprocmask stop-signal mirroring).
    case 155: {
        pid_t r = getpgid((pid_t)a0);
        if (g_init_hostpid && r == g_init_hostpid) r = 1;
        G_RET(c) = (uint64_t)r;
        break;
    }
    case 156: {
        pid_t r = getsid((pid_t)a0);
        if (g_init_hostpid && r == g_init_hostpid) r = 1;
        G_RET(c) = (uint64_t)r;
        break;
    }
    case 158: {
        if (g_gid >= 0) {
            // getgroups -> [effective gid]. Tracking the overlay's egid means apt's drop to _apt's group
            // is reflected here too (it setgroups(1,&_apt_gid) right before switching).
            if ((int)a0 >= 1 && a1) *(gid_t *)a1 = (gid_t)cred_egid();
            G_RET(c) = 1;
            break;
        }
        int r = getgroups((int)a0, (gid_t *)a1);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // setgroups (privileged; ignore)
    case 159: G_RET(c) = 0; break;
    // getrusage(who, *usage) -- a1 is the buffer, not a0!
    case 165: {
        struct rusage ru;
        // Linux RUSAGE_THREAD(1) -> SELF
        int who = ((int)a0 == -1) ? RUSAGE_CHILDREN : RUSAGE_SELF;
        if (a1) {
            uint8_t *d = (uint8_t *)a1;
            // Linux struct rusage layout (18 longs)
            memset(d, 0, 144);
            if (getrusage(who, &ru) == 0) {
                *(int64_t *)(d + 0) = ru.ru_utime.tv_sec;
                *(int64_t *)(d + 8) = ru.ru_utime.tv_usec;
                *(int64_t *)(d + 16) = ru.ru_stime.tv_sec;
                *(int64_t *)(d + 24) = ru.ru_stime.tv_usec;
                // macOS bytes -> Linux KB
                *(int64_t *)(d + 32) = ru.ru_maxrss / 1024;
                *(int64_t *)(d + 64) = ru.ru_minflt;
                *(int64_t *)(d + 72) = ru.ru_majflt;
                *(int64_t *)(d + 88) = ru.ru_inblock;
                *(int64_t *)(d + 96) = ru.ru_oublock;
                *(int64_t *)(d + 120) = ru.ru_nsignals;
                *(int64_t *)(d + 128) = ru.ru_nvcsw;
                *(int64_t *)(d + 136) = ru.ru_nivcsw;
            }
        }
        G_RET(c) = 0;
        break;
    }
    // prctl(option,...)
    case 167: {
        if ((int)a0 == 15) {
            snprintf(g_procname, sizeof g_procname, "%.15s", (const char *)a1);
            G_RET(c) = 0;
            break;
        } // PR_SET_NAME
        if ((int)a0 == 16) {
            snprintf((char *)a1, 16, "%s", g_procname);
            G_RET(c) = 0;
            break;
        } // PR_GET_NAME
        // 0 for known no-ops; EINVAL for unknown (kernel does)
        switch ((int)a0) {
        case 1:
        case 3:
        case 4:
        case 8:
        case 15:
        case 35:
        case 36:
        case 38:
        case 53:
        case 55:
        // PDEATHSIG/DUMPABLE/NAME/SECCOMP/TIMERSLACK/THP/SPECCTRL...
        case 59: G_RET(c) = 0; break;
        // EINVAL -- so feature probes (e.g. magic "AUXV") fail as on Linux
        default: G_RET(c) = (uint64_t)(-22); break;
        }
        break;
    }
    // getpid (PID ns: init -> 1)
    case 172: G_RET(c) = (uint64_t)container_pid(); break;
    case 173:
        G_RET(c) = (container_pid() == 1) ? 0 : (uint64_t)getppid();
        // getppid (init's parent is 0 in the ns)
        break;
    // getuid/geteuid -> container uid (0=root by default), reflecting any runtime drop (apt -> _apt).
    case 174:
        cred_init();
        G_RET(c) = (uint64_t)g_ruid;
        break;
    case 175: G_RET(c) = (uint64_t)cred_euid(); break;
    // getgid/getegid
    case 176:
        cred_init();
        G_RET(c) = (uint64_t)g_rgid;
        break;
    case 177: G_RET(c) = (uint64_t)cred_egid(); break;
    // gettid -- a UNIQUE per-thread id (unlike getpid, which is the shared tgid). The init thread keeps
    // c->tid==0 and reports the container pid (==1, where tid==tgid as on Linux); each spawned thread
    // carries its own id (spawn_thread). A correct gettid is load-bearing for runtimes that key thread
    // state on it (e.g. Go stores it in m.procid and tgkill()s it to preempt) -- collapsing every thread
    // to tid 1 makes their cross-thread signalling target the wrong thread and live-lock.
    case 178: G_RET(c) = (uint64_t)(c->tid ? c->tid : container_pid()); break;
    // clone(flags,stack,ptid,tls,ctid)
    case 220: {
        // CLONE_THREAD: stack arg IS the top
        if (a0 & 0x10000) {
            G_RET(c) = (uint64_t)spawn_thread(c, a0, a1, a3, a2, a4);
            break;
        }
        // fork/vfork: COW copy; child continues. Flush RAM-backed scratch into the real (shared) fds so
        // parent and child see one coherent file via the inherited description, exactly as POSIX requires
        // (the heap-resident buffers would otherwise COW-diverge while the fd stays shared).
        memf_materialize_all();
        pid_t pid = fork();
        if (pid == 0) {
            // clone(CLONE_VM, child_stack): glibc posix_spawn/popen/vfork pass a separate child stack in a1
            // and seed the clone trampoline (fn ptr + args) at its top. We fork() (COW) instead of sharing
            // the VM, but the child MUST run on a1 or glibc reads the trampoline off the parent's SP ->
            // garbage branch (SIGILL — broke initdb). a1==0 for a plain fork (bash), keeping the inherited SP.
            if ((a0 & 0x100) && a1) G_SP(c) = a1;
            // Re-assert MAP_JIT execute mode: the per-thread W^X/APRR state isn't reliable across fork(),
            // so the child's first run_block can instruction-abort fetching from the (non-executable) code
            // cache -> the intermittent fork+exec SIGBUS. pthread_jit_write_protect_np(1) = RX (executable).
            pthread_jit_write_protect_np(1);
            jit_after_fork();  // dual map: COW split the RW/RX aliases -> rebuild a fresh aliased cache
            G_SHADOW_RESET(c); // §B: child's pre-fork host_rets crossed run_block -> drop, use IBTC
            rc_reset();        // S2: drop the inherited (COW) path-resolution cache so the child can never
                               // serve a guest->host mapping that the parent populated before the FS diverged
            g_ndirs = 0;       // the getdents DIR* cache is the PARENT's -- closedir'ing inherited handles
                               // (on the child's close) crashes; drop it so the child re-fdopendir's fresh
#ifdef DD_HAS_MACH_EXC
            // The CRASHDBG Mach exception port + its receiver thread do NOT survive fork, so a crash in the
            // child silently dies. Clear the inherited task exception port so a fault falls through to the
            // POSIX diag_crash handler (which IS inherited) and reports fault=/pc=.
            if (getenv("CRASHDBG"))
                task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION,
                                         MACH_PORT_NULL, EXCEPTION_DEFAULT, 0);
#endif
        }
        // parent: pid, child: 0
        G_RET(c) = pid < 0 ? (uint64_t)(-errno) : (uint64_t)pid;
        // A fork/vfork that was normalized to clone repurposed the guest's arg registers; put them back so
        // the syscall preserves every GPR but rax, as the real kernel does (no-op for a genuine clone).
        G_FORK_PRESERVE(c);
        break;
    }
    // execve(path, argv, envp)
    case 221: {
        memf_materialize_all(); // non-CLOEXEC scratch fds survive exec -> flush RAM into the real files
        char pb[4200];
        const char *p =
            // follow symlink rootfs-relative (busybox applets), through the overlay (upper then lowers)
            xresolve_overlay((const char *)a0, pb, sizeof pb);
        if (access(p, F_OK) != 0) {
            G_RET(c) = (uint64_t)(-2);
            break;
            // ENOENT
        }
        char *argv[256];
        int ac = 0;
        uint64_t *gv = (uint64_t *)a1; // a1 (argv array base) already nonpie_p()'d at the top redirect
        while (gv && gv[ac] && ac < 255) {
            argv[ac] = (char *)nonpie_p(gv[ac]); // each argv[] element may itself be a low-image pointer
            ac++;
        }
        argv[ac] = NULL;
        // Forward the guest's ACTUAL environment across the exec: build_stack rebuilds the new process env
        // from DD_GUEST_ENV, so serialize envp (a2) into it NOW while guest memory is still mapped. A guest
        // that set/modified env vars (FOO=bar, a tweaked PATH) thus sees them survive; a NULL envp keeps the
        // container's DD_GUEST_ENV defaults (a2 is NOT rebased by the dispatch redirect, unlike a0/a1).
        exec_forward_env(a2);
        // Capture the guest-absolute exec path NOW (a0 is still mapped) so /proc/self/exe can name the new
        // image after the teardown below. ld.so resolves a binary's $ORIGIN (DT_RUNPATH) via readlink of
        // /proc/self/exe; a stale value makes an exec'd dynamic binary fail to find its own libraries (e.g.
        // rustup's proxy execs the real rustc, whose RUNPATH $ORIGIN/../lib must point into the toolchain).
        char gexe[4200];
        abs_guest(-100, (const char *)a0, gexe, sizeof gexe);
        // shebang: exec the #! interpreter instead (parse_shebang is shared with the initial loader)
        char sh_interp[256], sh_arg[256], shpb[4200];
        if (parse_shebang(p, sh_interp, sizeof sh_interp, sh_arg, sizeof sh_arg) == 1) {
            snprintf(gexe, sizeof gexe, "%s", sh_interp); // a script exec: /proc/self/exe names the interpreter
            char *na[258];
            int ni = 0;
            // [interp, (optarg), scriptpath, args...]
            na[ni++] = sh_interp;
            if (sh_arg[0]) na[ni++] = sh_arg;
            // the guest script path (interp re-opens it)
            na[ni++] = (char *)a0;
            for (int i = 1; i < ac && ni < 256; i++)
                na[ni++] = argv[i];
            na[ni] = NULL;
            // load the interpreter, not the script -- through the overlay (the #! interp, e.g. /bin/sh, may
            // live only in a read-only lower in a fresh container; xresolve_exec sees the upper alone -> ENOENT)
            p = xresolve_overlay(sh_interp, shpb, sizeof shpb);
            if (access(p, F_OK) != 0) {
                G_RET(c) = (uint64_t)(-2);
                break;
            }
            for (int i = 0; i <= ni; i++)
                argv[i] = na[i];
            ac = ni;
        }
        // Committed to the exec now (all ENOENT early-returns are behind us): emulate the kernel's
        // close-on-exec sweep. No real host exec runs below -- we re-load the new image in this same
        // process -- so FD_CLOEXEC fds must be closed by hand or they leak into the new program.
        exec_close_cloexec();
        // Tear down the inherited guest address space before loading the new image: a post-fork exec
        // otherwise keeps the parent's DENSE layout, and load_elf must bias a non-PIE ET_EXEC off its
        // fixed vaddr (__PAGEZERO blocks the low 4 GB) -> its baked absolute refs collide -> SIGSEGV.
        // argv + path live in guest memory we're about to munmap, so copy them to the host heap first.
        char *xpath = strdup(p);
        char *xargv[256];
        for (int i = 0; i < ac && i < 255; i++)
            xargv[i] = strdup(argv[i]);
        xargv[ac < 255 ? ac : 255] = NULL;
        gmap_reset_all();
        g_nonpie_lo = g_nonpie_hi = 0; // reset; load_elf re-sets it iff the new main image is non-PIE
        p = xpath;
        for (int i = 0; i < ac && i < 255; i++)
            argv[i] = xargv[i];
        argv[ac < 255 ? ac : 255] = NULL;
        struct loaded lm;
        load_elf(p, &lm);
        uint64_t jump = lm.entry, at_base = 0;
        char interp[256];
        if (elf_interp(p, interp, sizeof interp) == 0) {
            char ib[4200];
            // follow+confine ld.so symlink (through the overlay)
            const char *ih = xresolve_overlay(interp, ib, sizeof ib);
            struct loaded li;
            load_elf(ih, &li);
            jump = li.entry;
            at_base = li.base;
        }
        g_cp = g_cache;
        memset(g_map, 0, sizeof g_map);
        // flush old translations
        g_npend = 0;
        memset(g_ibtc, 0, sizeof g_ibtc);
        // execve is a wholesale code-cache flush (g_cp reset + g_map/g_ibtc zeroed above), so it must ALSO
        // run the per-arch wholesale-flush hook the dispatcher uses (jit/dispatch.c) -- not just the lighter
        // fork/exec G_SHADOW_RESET. On x86 that hook drops the 2-way g_xibtc (G_SHADOW_RESET is a NO-OP there,
        // so g_xibtc was surviving execve); on aarch64 it resets the §B shadow stack. Without it a forked
        // child that execve's a new image (apt http method / gzip / cc1 / git child) keeps the OLD image's
        // g_xibtc entries -- keyed by guest PC the new image REUSES, bodies pointing into the freed cache --
        // and an indirect branch resolves into garbage host code -> SIGSEGV/SIGBUS (#176 / #117 / #155).
        G_SHADOW_CLEAR(c);
        // POSIX execve resets CAUGHT signal handlers to SIG_DFL (SIG_IGN stays ignored). Without this, a
        // handler the calling shell installed (e.g. busybox sh's SIGCHLD job-control handler) survives into
        // the new image and is later delivered to a now-garbage handler address -> crash (redis/valkey run
        // via `sh -c …`). handler>1 == a real caught handler; 0=DFL, 1=IGN.
        for (int s = 1; s < 65; s++)
            if (g_sigact[s].handler > 1) {
                g_sigact[s].handler = 0;
                g_sigact[s].flags = 0;
                g_sigact[s].mask = 0;
            }
        uint8_t *heap = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        gmap_add((uint64_t)heap, 256u << 20);
        brk_lo = brk_cur = (uint64_t)heap;
        brk_hi = brk_lo + (256u << 20);
        uint64_t sp = build_stack(ac, argv, &lm, at_base);
        free(xpath);
        for (int i = 0; i < ac && i < 255; i++)
            free(xargv[i]);
        snprintf(g_exe_path_store, sizeof g_exe_path_store, "%s", gexe); // /proc/self/exe -> the new image
        g_exe_path = g_exe_path_store;
        G_RESET_REGS(c);
        c->nzcv = 0;
        G_TLS(c) = 0;
        G_SP(c) = sp;
        G_PC(c) = jump;
        // jump to new program; don't advance pc
        c->redirect = 1;
        break;
    }
    // wait4(pid, *status, opts, *rusage)
    case 260: {
        int st = 0;
        pid_t r;
        // SA_RESTART: a wait interrupted by a handler that asked to restart (e.g. a SIGCHLD reaper, or
        // gcc's driver) must transparently retry instead of failing the guest with EINTR.
        do { r = wait4((pid_t)(int)a0, &st, (int)a2, (struct rusage *)a3); } while (r < 0 && SVC_EINTR_RESTART(c));
        if (r < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        // WIFSIGNALED: macOS termsig -> Linux
        if ((st & 0x7f) != 0 && (st & 0x7f) != 0x7f) st = (st & ~0x7f) | (sig_m2l(st & 0x7f) & 0x7f);
        // WIFSTOPPED: macOS stopsig -> Linux
        else if ((st & 0xff) == 0x7f)
            st = (st & ~0xff00) | ((sig_m2l((st >> 8) & 0xff) & 0xff) << 8);
        if (a1) *(int *)a1 = st;
        G_RET(c) = (uint64_t)r;
        break;
    }
    case 261: {
        if (a3) {
            // prlimit64(pid,res,new,OLD): old=a3!
            svc_fill_rlimit((int)a1, (uint64_t *)a3);
        }
        G_RET(c) = 0;
        break;
    }
    // clone3(clone_args*, size)
    case 435: {
        // clone3(clone_args*, size): a hostile/buggy guest can pass a bad args pointer or a junk size;
        // validate BEFORE any deref so it returns an errno instead of faulting the engine. -EINVAL if size
        // is below the VER0 clone_args (we read only its first 64 bytes) or implausibly large; -EFAULT if
        // the args struct isn't mapped.
        if (a1 < 64 || a1 > 4096) { G_RET(c) = (uint64_t)(int64_t)(-EINVAL); break; }
        if (!host_range_mapped(a0, a1)) { G_RET(c) = (uint64_t)(int64_t)(-EFAULT); break; }
        uint64_t *ca = (uint64_t *)a0;
        uint64_t flags = ca[0];
        // CLONE_THREAD: sp = stack + stack_size
        if (flags & 0x10000) {
            G_RET(c) = (uint64_t)spawn_thread(c, flags, ca[5] + ca[6], ca[7], ca[3], ca[2]);
            break;
        }
        pid_t pid = fork();
        // §B: same -- child drops the inherited shadow; S2: and the inherited path-resolution cache
        if (pid == 0) {
            jit_after_fork(); // dual map: rebuild the child's aliased cache (COW split RW/RX)
            G_SHADOW_RESET(c);
            rc_reset();
        }
        G_RET(c) = pid < 0 ? (uint64_t)(-errno) : (uint64_t)pid;
        break;
    }
    default:
        return 0;
    }
    return svc_done(c); // boundary errno xlate (host macOS -> Linux); see helpers.c svc_done
}
