// Extracted from service(): Filesystem -- open/openat/stat*/dir/link/perm/xattr/cwd/access, every path
// confined to the rootfs jail (overlay copy-up, /proc/self/exe synth). Returns 1 if nr was handled, 0
// otherwise. Included by service.c AFTER its local helpers (overlay_*/proc_self_exe/synth_str_fd/
// cpu_range_str it calls) and before service() -- same TU scope.

// A terminal-control syscall (tcsetpgrp/tcsetattr) issued by a process that is in a BACKGROUND process
// group raises SIGTTOU on the whole group; with the default disposition that STOPS it. During job-control
// handoff a shell's pipeline child briefly sits in a background group between its setpgid() and the
// parent's tcsetpgrp(), so a foreground command can be SIGTTOU-stopped before it even execs (the
// "[1]+ Stopped  ls | cat" hang -- the engine's in-process children lose this race more readily than a
// real kernel does). POSIX guarantees that when SIGTTOU is blocked the call simply succeeds and NO signal
// is generated -- which is exactly what a correct shell does around these calls (bash's give_terminal_to).
// So block SIGTTOU on the host for the duration of the REAL call: it never fakes the operation (the real
// tcsetpgrp/tcsetattr still runs on the real pty) and is a no-op when the guest already blocked it.
static void tty_ctl_block(sigset_t *saved) {
    sigset_t blk;
    sigemptyset(&blk);
    sigaddset(&blk, SIGTTOU);
    sigprocmask(SIG_BLOCK, &blk, saved);
}
static void tty_ctl_restore(const sigset_t *saved) { sigprocmask(SIG_SETMASK, saved, NULL); }

// Overlay getdents64 snapshot cache (case 61): the merged cross-layer listing for a directory fd is taken
// once on the first getdents call and consumed across the many small reads libc makes. Keyed by guest
// fd+1 (0 == free). A slot MUST be invalidated on close() -- ovldents_drop, called from case 57 -- so a
// reused fd re-snapshots a fresh directory rather than serving the previous one's leftover tail. Without
// that, a directory read partially then closed poisoned the next directory opened on the same fd, which
// silently truncated postgres initdb's template1->template0/postgres copy (dropping ~1/4 of the catalog,
// e.g. PG_VERSION -> "base/5 is not a valid data directory" on the first client connect).
// nm/ty are heap-allocated by overlay_readdir (it grows them to the real entry count -- no 1024 cap, so
// large directories no longer truncate, #179) and owned until freed (ovldents_free). Indexed DIRECTLY by
// guest fd (the getdents call site guarantees fd in [0,1024)); a former 16-slot table with slot-0 eviction
// broke deep `find`: a recursive walk keeps one open dir fd per level, so past 16 concurrent overlay dirs
// an ancestor's snapshot was evicted and its next getdents re-snapshotted from pos 0 -> re-descended the
// same subtree forever (loop threshold was exactly depth 16, #199).
static struct {
    int taken; // 1 = this fd's snapshot is live
    int n, pos;
    char (*nm)[256];
    uint8_t *ty;
} g_ovldents[1024];
static void ovldents_free(int i) {
    free(g_ovldents[i].nm);
    free(g_ovldents[i].ty);
    g_ovldents[i].nm = NULL;
    g_ovldents[i].ty = NULL;
    g_ovldents[i].taken = 0;
    g_ovldents[i].n = g_ovldents[i].pos = 0;
}
static void ovldents_drop(int fd) {
    if (fd >= 0 && fd < 1024 && g_ovldents[fd].taken)
        ovldents_free(fd);
}

// POSIX shm / named semaphores live under /dev/shm, for which the rootfs has no tmpfs; glibc backs them
// with files there (shm_open -> /dev/shm/<name>, sem_open -> a temp /dev/shm/sem.<rnd> then link()ed to
// /dev/shm/sem.<name>). openat (case 56) redirects these to a stable host file /tmp/.ddshm-<name> so the
// page is real and MAP_SHARED across fork. The link/rename/unlink that COMPLETE glibc's create dance must
// use the SAME backing, but the rootfs branches of those handlers resolve via jail_at into the (empty)
// <rootfs>/dev/shm and ENOENT -- breaking sem_open (python multiprocessing). Returns the host backing path
// for a /dev/shm/<name> guest path (into buf), or NULL otherwise. Same name-flattening as the openat redirect.
static const char *shm_hostpath(const char *guest, char *buf, size_t n) {
    if (!guest || strncmp(guest, "/dev/shm/", 9)) return NULL;
    int m = snprintf(buf, n, "/tmp/.ddshm-%s", guest + 9);
    if (m > (int)n - 1) m = (int)n - 1;
    for (int i = 12; i < m; i++)
        if (buf[i] == '/') buf[i] = '_';
    return buf;
}

static int svc_fs(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                  uint64_t a4, uint64_t a5) {
    switch (nr) {
    // ===================== Filesystem — open/stat/dir/link/perm/xattr/cwd, all path-confined to the rootfs jail
    // =====================
    case 5:
    case 6:
    // setxattr/lsetxattr/fsetxattr -> ignore
    case 7: G_RET(c) = 0; break;
    case 8:
    case 9:
    // getxattr/... -> ENODATA (no such attr)
    case 10: G_RET(c) = (uint64_t)(-ENODATA); break;
    case 11:
    case 12:
    // listxattr/... -> empty list
    case 13: G_RET(c) = 0; break;
    case 14:
    case 15:
    // removexattr/... -> ok
    case 16: G_RET(c) = 0; break;
    case 17: {
        if (g_rootfs) {
            // getcwd -> the GUEST cwd (not the host path)
            size_t l = strlen(g_cwd);
            if (a0 && l + 1 <= a1) {
                if (!host_range_mapped((uintptr_t)a0, l + 1)) { G_RET(c) = (uint64_t)(int64_t)(-EFAULT); break; }
                memcpy((void *)a0, g_cwd, l + 1);
                G_RET(c) = l + 1;
            } else
                G_RET(c) = (uint64_t)(-ERANGE);
            break;
        }
        if (a0 && !host_range_mapped((uintptr_t)a0, (size_t)a1)) { G_RET(c) = (uint64_t)(int64_t)(-EFAULT); break; }
        if (getcwd((char *)a0, (size_t)a1))
            G_RET(c) = strlen((char *)a0) + 1;
        else
            G_RET(c) = (uint64_t)(-errno);
        break;
    }
    // ioctl(fd, req, arg) -- Linux req# -> macOS
    case 29: {
        int fd = (int)a0;
        // Truncate the ioctl request to 32 bits (Linux `cmd` is unsigned int). musl declares ioctl's request
        // as `int`, so a read-direction request with the direction bit set (e.g. TIOCGPTN 0x80045430) arrives
        // SIGN-EXTENDED as 0xffffffff80045430 and would miss its switch case -> ENOTTY (glibc zero-extends, so
        // it worked there). This makes both forms match, fixing musl tmux/script/openpty and any high-bit ioctl. (#219)
        unsigned long rq = (uint32_t)a1;
        void *arg = (void *)a2;
        switch (rq) {
        case 0x5401: {
            struct termios t;
            if (tcgetattr(fd, &t) < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
                // TCGETS
            }
            termios_m2l(&t, (uint8_t *)arg);
            G_RET(c) = 0;
            break;
        }
        case 0x5402:
        case 0x5403:
        case 0x5404: {
            struct termios t;
            // TCSETS/W/F
            termios_l2m((const uint8_t *)arg, &t);
            int act = rq == 0x5402 ? TCSANOW : rq == 0x5403 ? TCSADRAIN : TCSAFLUSH;
            sigset_t sv;
            tty_ctl_block(&sv); // a bg-group tcsetattr would otherwise SIGTTOU-stop the caller
            G_RET(c) = tcsetattr(fd, act, &t) < 0 ? (uint64_t)(-errno) : 0;
            tty_ctl_restore(&sv);
            break;
        }
        case 0x802c542a: {
            struct termios t;
            if (tcgetattr(fd, &t) < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
                // TCGETS2 (glibc aarch64 uses this)
            }
            termios_m2l(&t, (uint8_t *)arg);
            *(uint32_t *)((uint8_t *)arg + 36) = (uint32_t)cfgetispeed(&t);
            *(uint32_t *)((uint8_t *)arg + 40) = (uint32_t)cfgetospeed(&t);
            G_RET(c) = 0;
            break;
        }
        case 0x402c542b:
        case 0x402c542c:
        case 0x402c542d: {
            struct termios t;
            // TCSETS2/W2/F2
            termios_l2m((const uint8_t *)arg, &t);
            cfsetispeed(&t, *(uint32_t *)((const uint8_t *)arg + 36));
            cfsetospeed(&t, *(uint32_t *)((const uint8_t *)arg + 40));
            int act = rq == 0x402c542b ? TCSANOW : rq == 0x402c542c ? TCSADRAIN : TCSAFLUSH;
            sigset_t sv;
            tty_ctl_block(&sv); // a bg-group tcsetattr would otherwise SIGTTOU-stop the caller
            G_RET(c) = tcsetattr(fd, act, &t) < 0 ? (uint64_t)(-errno) : 0;
            tty_ctl_restore(&sv);
            break;
        }
        case 0x5413:
            G_RET(c) = ioctl(fd, TIOCGWINSZ, arg) < 0 ? (uint64_t)(-errno) : 0;
            // TIOCGWINSZ (struct same)
            break;
        // TIOCSWINSZ
        case 0x5414: G_RET(c) = ioctl(fd, TIOCSWINSZ, arg) < 0 ? (uint64_t)(-errno) : 0; break;
        case 0x80045430:
            if (arg && fd >= 0 && fd < 1024) *(uint32_t *)arg = (uint32_t)fd;
            G_RET(c) = 0;
            // TIOCGPTN -> pts# = master fd
            break;
        // TIOCSPTLCK (unlockpt done at open)
        case 0x40045431: G_RET(c) = 0; break;
        // TIOCGPTPEER (_IO('T',0x41) == 0x5441; no direction bit, so it arrives unchanged under both musl
        // and glibc) -- glibc's openpty() opens the slave in a SINGLE call from the master fd instead of the
        // ptsname()+open() dance. `fd` is the master and (as TIOCGPTN reports) IS the pts#, so ptsname(fd)
        // resolves the host slave device -- the exact path the `/dev/pts/N` open uses. `arg` carries open
        // flags (O_RDWR|O_NOCTTY, glibc may OR in O_CLOEXEC 0x80000); open the slave and RETURN the new fd,
        // like a dup/open. (musl's openpty takes a different ptsname route and never issues this.)
        case 0x5441: {
            char *sn = ptsname(fd);
            if (!sn) { G_RET(c) = (uint64_t)(int64_t)(-(errno ? errno : EINVAL)); break; }
            int mf = ((int)a2 & 0x3) | O_NOCTTY;      // access mode (shared values) + no controlling tty
            if (a2 & 0x80000) mf |= O_CLOEXEC;        // honor Linux O_CLOEXEC on the returned fd
            int s = open(sn, mf);
            G_RET(c) = s < 0 ? (uint64_t)(-errno) : (uint64_t)s;
            break;
        }
        case 0x5421: {
            // FIONBIO
            int on = arg ? *(int *)arg : 0, fl = fcntl(fd, F_GETFL);
            fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK);
            G_RET(c) = fcntl(fd, F_SETFL, fl) < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        // FIONREAD
        case 0x541b: G_RET(c) = ioctl(fd, FIONREAD, arg) < 0 ? (uint64_t)(-errno) : 0; break;
        // FIOCLEX
        case 0x5451: G_RET(c) = fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ? (uint64_t)(-errno) : 0; break;
        case 0x5450: {
            int fl = fcntl(fd, F_GETFD);
            G_RET(c) = fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC) < 0 ? (uint64_t)(-errno) : 0;
            break;
            // FIONCLEX
        }
        // TIOCGPGRP/TIOCSPGRP -- REAL job control. The guest's children are real host processes (clone = host
        // fork) in the engine's session (the daemon's login_tty made the engine the pty's session leader), so
        // the kernel's own pty foreground-group machinery applies to them: a child placed in the foreground
        // really IS the fg group -> not SIGTTIN/SIGTTOU-frozen, and Ctrl-C/Ctrl-Z reach it. Two things make it
        // work: (1) here we virtualize only the INIT's identity -- the guest sees getpid()==1 while its real
        // host pgid is g_init_hostpid -- translating just that pair and passing real child pgids straight
        // through to the real host tcget/tcsetpgrp; (2) rt_sigprocmask mirrors the terminal-stop signals onto
        // the host mask, so bash's background tcsetpgrp handoff isn't SIG_DFL-stopped by the host kernel.
        case 0x540f: { // tcgetpgrp
            pid_t fg = isatty(fd) ? tcgetpgrp(fd) : -1;
            if (fg <= 0) fg = getpgrp();
            if (g_init_hostpid && fg == g_init_hostpid) fg = 1; // init's real group -> guest pgid 1
            if (arg) *(int *)arg = (int)fg;
            G_RET(c) = 0;
            break;
        }
        case 0x5410: { // tcsetpgrp
            pid_t pg = arg ? *(int *)arg : 0;
            if (pg == 1 && g_init_hostpid) pg = g_init_hostpid; // guest pgid 1 -> init's real host group
            if (isatty(fd) && pg > 0) {
                // A pipeline leader calls tcsetpgrp while still in a background group (the parent shell sets
                // the foreground group concurrently); without blocking SIGTTOU here the host kernel would
                // STOP it mid-handoff -> the foreground command freezes ("[1]+ Stopped"). Block SIGTTOU so
                // the real tcsetpgrp installs the fg group cleanly (kernel still routes ^C/^Z afterwards).
                sigset_t sv;
                tty_ctl_block(&sv);
                (void)tcsetpgrp(fd, pg);
                tty_ctl_restore(&sv);
            }
            G_RET(c) = 0; // never surface an error -> bash never warns
            break;
        }
        // TIOCSCTTY -- acquire the controlling terminal for real when `fd` is a tty (best effort; the
        // login_tty in the daemon usually already did this for the session leader), then report success so
        // an interactive shell's job-control setup never warns.
        case 0x540e:
            if (isatty(fd)) (void)ioctl(fd, TIOCSCTTY, 0);
            G_RET(c) = 0;
            break;
        // ENOTTY
        default: G_RET(c) = (uint64_t)(-25); break;
        }
        break;
    }
    // mknodat(dirfd, path, mode, dev)
    case 33: {
        if (jail_ro_at((int)a0, (const char *)a1)) {
            G_RET(c) = (uint64_t)(int64_t)(-EROFS);
            break;
        }
        if (g_rootfs) {
            char fin[512];
            int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, 1);
            if (pfd < 0) {
                G_RET(c) = (uint64_t)(int64_t)pfd;
                break;
            }
            int r = mknodat(pfd, fin, (mode_t)a2, (dev_t)a3), e = errno;
            char dp[4200];
            if (r >= 0 && fcntl(pfd, F_GETPATH, dp) == 0) {
                char hp[4400];
                snprintf(hp, sizeof hp, "%s/%s", dp, fin);
                mc_evict(hp);
                ac_evict(hp);
                if (newfile_stamp_wanted()) newfile_stamp_path(hp, 1); // #255: dropped-cred creator owns it
            }
            close(pfd);
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : 0;
            break;
        }
        char pb[4200];
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb, 0);
        int r = mknodat(ATFD(a0), p, (mode_t)a2, (dev_t)a3);
        if (r >= 0) {
            mc_evict(p);
            ac_evict(p);
            if (newfile_stamp_wanted()) newfile_stamp_path(p, 1); // #255
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // mkdirat(dirfd, path, mode) -- confined
    case 34: {
        if (jail_ro_at((int)a0, (const char *)a1)) {
            G_RET(c) = (uint64_t)(int64_t)(-EROFS);
            break;
        }
        if (g_rootfs) {
            char fin[512];
            int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, 1);
            if (pfd < 0) {
                G_RET(c) = (uint64_t)(int64_t)pfd;
                break;
            }
            int r = mkdirat(pfd, fin, (mode_t)a2), e = errno;
            char dp[4200];
            if (r >= 0 && fcntl(pfd, F_GETPATH, dp) == 0) {
                char hp[4400];
                snprintf(hp, sizeof hp, "%s/%s", dp, fin);
                mc_evict(hp);
                ac_evict(hp);
                if (newfile_stamp_wanted()) newfile_stamp_path(hp, 1); // #255: dropped-cred creator owns the dir
            }
            close(pfd);
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : 0;
            break;
        }
        char pb[4200];
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb, 0);
        int r = mkdirat(ATFD(a0), p, (mode_t)a2);
        mc_evict(p);
        // namespace change -> evict
        ac_evict(p);
        if (r >= 0 && newfile_stamp_wanted()) newfile_stamp_path(p, 1); // #255
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // unlinkat(dirfd, path, flags) -- confined
    case 35: {
        // shm/sem files are flat host files under /tmp (see shm_hostpath); sem_unlink/shm_unlink and glibc's
        // temp-file cleanup must hit that backing, not the jail's <rootfs>/dev/shm. AT_REMOVEDIR never applies.
        char shb[300];
        const char *shp = shm_hostpath((const char *)a1, shb, sizeof shb);
        if (shp) {
            G_RET(c) = unlink(shp) < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        if (jail_ro_at((int)a0, (const char *)a1)) {
            G_RET(c) = (uint64_t)(int64_t)(-EROFS);
            break;
        }
        // RAM-backed scratch adoption: SQLite et al. open a temp file O_CREAT|O_EXCL then unlink it while
        // still open (delete-on-close). After this unlink drops its last link the file is anonymous, so we
        // may adopt it into RAM. Cheap pre-filter (avoid the fd scan on ordinary unlinks): a temp-dir path
        // or the sqlite "etilqs_" prefix, and not a directory removal. dev/ino is captured (per branch,
        // through the same resolution the unlink uses) right before the unlink and matched after.
        int try_adopt = 0;
        if (!memf_disabled() && !(a2 & 0x200)) {
            char gp[4200];
            abs_guest((int)a0, (const char *)a1, gp, sizeof gp);
            const char *base = strrchr(gp, '/');
            base = base ? base + 1 : gp;
            try_adopt = !strncmp(gp, "/tmp/", 5) || !strncmp(gp, "/var/tmp/", 9) || strstr(base, "etilqs_") != 0;
        }
        // OVERLAY: delete. A name a read-only lower still provides must be MASKED with a .wh.NAME whiteout
        // (overlay_whiteout also drops any upper copy) so it stays hidden. An UPPER-ONLY file has no lower to
        // mask, so it is simply removed with NO whiteout -- a spurious .wh.NAME would otherwise linger in the
        // parent and hide a later re-create of that same name (apt's http method deletes partial/X after a
        // failed fetch, then re-creates and renames it -> the stale whiteout ENOENTed the rename source).
        if (g_rootfs && g_nlower) {
            char gp[4200];
            abs_guest((int)a0, (const char *)a1, gp, sizeof gp);
            char host[4300];
            if (!overlay_resolve(gp, host, sizeof host, 1)) {
                G_RET(c) = (uint64_t)(-2);
                break;
                // ENOENT
            }
            // Enforce rmdir/unlink type semantics against the MERGED target BEFORE touching it. The
            // non-overlay branches pass AT_REMOVEDIR straight to unlinkat() so the kernel does this, but
            // the overlay path used remove()/overlay_whiteout() which pick unlink-vs-rmdir by the target's
            // OWN type -- so rmdir() wrongly succeeded on a regular file (and unlink() on a directory). dpkg
            // probes a control file's type with `rmdir(f) == 0`: the wrongly-successful rmdir deleted the
            // file and made dpkg abort "package control info contained directory" (#254). Match Linux:
            // rmdir a non-directory -> ENOTDIR; unlink a directory -> EISDIR.
            struct stat lst;
            int isdir = lstat(host, &lst) == 0 && S_ISDIR(lst.st_mode);
            if ((a2 & 0x200) && !isdir) { G_RET(c) = (uint64_t)(int64_t)(-ENOTDIR); break; }
            if (!(a2 & 0x200) && isdir) { G_RET(c) = (uint64_t)(int64_t)(-EISDIR); break; }
            if (overlay_lower_has(gp)) {
                overlay_whiteout(gp);
                G_RET(c) = 0;
            } else {
                // upper-only -> remove with the CORRECT op (rmdir for a dir, unlink for a file) so the
                // kernel still enforces ENOTDIR/EISDIR/ENOTEMPTY exactly as Linux would.
                int r = (a2 & 0x200) ? rmdir(host) : unlink(host);
                G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            }
            // Invalidate the stat/access/readlink caches for the removed path: `host` is the merged-resolve
            // host path, the SAME key case 79/48 memoize under, so a follow-up `test -e`/stat sees it gone
            // (mirrors the non-overlay branch below). Without this a removed upper entry kept reporting as
            // present via a stale mc_ hit even though it no longer appears in a readdir.
            mc_evict(host);
            ac_evict(host);
            rl_evict(host);
            break;
        }
        if (g_rootfs) {
            char fin[512];
            int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, 1);
            if (pfd < 0) {
                G_RET(c) = (uint64_t)(int64_t)pfd;
                break;
            }
            uint64_t adev = 0, aino = 0;
            if (try_adopt) {
                struct stat ps;
                if (fstatat(pfd, fin, &ps, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(ps.st_mode)) {
                    adev = (uint64_t)ps.st_dev;
                    aino = (uint64_t)ps.st_ino;
                }
            }
            // AT_REMOVEDIR: linux 0x200
            int r = unlinkat(pfd, fin, (a2 & 0x200) ? AT_REMOVEDIR : 0), e = errno;
            char dp[4200];
            if (r >= 0 && fcntl(pfd, F_GETPATH, dp) == 0) {
                char hp[4400];
                snprintf(hp, sizeof hp, "%s/%s", dp, fin);
                mc_evict(hp);
                ac_evict(hp);
                rl_evict(hp);
            }
            close(pfd);
            if (r >= 0 && aino) memf_try_adopt(adev, aino);
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : 0;
            break;
        }
        char pb[4200];
        // unlink: never follow the final symlink (remove the link itself, not its target).
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb, 1);
        uint64_t adev = 0, aino = 0;
        if (try_adopt) {
            struct stat ps;
            if (fstatat(ATFD(a0), p, &ps, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(ps.st_mode)) {
                adev = (uint64_t)ps.st_dev;
                aino = (uint64_t)ps.st_ino;
            }
        }
        int r = unlinkat(ATFD(a0), p, (a2 & 0x200) ? AT_REMOVEDIR : 0);
        mc_evict(p);
        ac_evict(p);
        rl_evict(p);
        if (r >= 0 && aino) memf_try_adopt(adev, aino);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // symlinkat(target, newdirfd, linkpath) -- the link is CREATED at (newdirfd, linkpath)
    case 36: {
        if (jail_ro_at((int)a1, (const char *)a2)) {
            G_RET(c) = (uint64_t)(int64_t)(-EROFS);
            break;
        }
        const char *target =
            // target is the link CONTENT (unresolved); follow-time confinement guards it
            (const char *)a0;
        if (g_rootfs) {
            char fin[512];
            int pfd = jail_at((int)a1, (const char *)a2, fin, sizeof fin, 1);
            if (pfd < 0) {
                G_RET(c) = (uint64_t)(int64_t)pfd;
                break;
            }
            int r = symlinkat(target, pfd, fin), e = errno;
            close(pfd);
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : 0;
            break;
        }
        char pb[4200];
        const char *p = atpath((int)a1, (const char *)a2, pb, sizeof pb, 0);
        G_RET(c) = symlinkat(target, ATFD(a1), p) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // linkat(odir,opath,ndir,npath,flags) -- writes both ends (new link + source link count)
    case 37: {
        // glibc's sem_open/shm_open creation links a temp /dev/shm/sem.<rnd> to the final /dev/shm/<name>;
        // both ends are shm-backed host files under /tmp, so link them directly (the jail branch below would
        // resolve them into the empty <rootfs>/dev/shm and ENOENT).
        char lob[300], lnb[300];
        const char *loh = shm_hostpath((const char *)a1, lob, sizeof lob);
        const char *lnh = shm_hostpath((const char *)a3, lnb, sizeof lnb);
        if (loh && lnh) {
            G_RET(c) = link(loh, lnh) < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        if (jail_ro_at((int)a0, (const char *)a1) || jail_ro_at((int)a2, (const char *)a3)) {
            G_RET(c) = (uint64_t)(int64_t)(-EROFS);
            break;
        }
        int fl = (a4 & 0x400) ? AT_SYMLINK_FOLLOW : 0;
        if (g_rootfs) {
            // both ends confined via TOCTOU-free resolver
            char ofin[512], nfin[512];
            int opfd = jail_at((int)a0, (const char *)a1, ofin, sizeof ofin, 1);
            if (opfd < 0) {
                G_RET(c) = (uint64_t)(int64_t)opfd;
                break;
            }
            int npfd = jail_at((int)a2, (const char *)a3, nfin, sizeof nfin, 1);
            if (npfd < 0) {
                close(opfd);
                G_RET(c) = (uint64_t)(int64_t)npfd;
                break;
            }
            int r = linkat(opfd, ofin, npfd, nfin, fl), e = errno;
            close(opfd);
            close(npfd);
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : 0;
            break;
        }
        char ob[4200], nb[4200];
        const char *op = atpath((int)a0, (const char *)a1, ob, sizeof ob, 0);
        const char *np = atpath((int)a2, (const char *)a3, nb, sizeof nb, 0);
        G_RET(c) = linkat(ATFD(a0), op, ATFD(a2), np, fl) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 38:
    // renameat(38) / renameat2(276): translate the renameat2 flags onto macOS renameatx_np --
    // RENAME_NOREPLACE(1)->RENAME_EXCL (fail if dst exists), RENAME_EXCHANGE(2)->RENAME_SWAP (atomic swap).
    case 276: {
        if (jail_ro_at((int)a0, (const char *)a1) || jail_ro_at((int)a2, (const char *)a3)) {
            G_RET(c) = (uint64_t)(int64_t)(-EROFS);
            break;
        }
        unsigned int rxflags = 0;
        if (nr == 276) {
            int lf = (int)a4;
            if (lf & 1) rxflags |= RENAME_EXCL;
            if (lf & 2) rxflags |= RENAME_SWAP;
        }
        // shm/sem create that renames (rather than links) a temp /dev/shm file to the final name: both ends
        // are shm-backed host files under /tmp, so rename them directly (the jail branch would ENOENT them).
        char rob[300], rnb[300];
        const char *roh = shm_hostpath((const char *)a1, rob, sizeof rob);
        const char *rnh = shm_hostpath((const char *)a3, rnb, sizeof rnb);
        if (roh && rnh) {
            G_RET(c) = renameatx_np(AT_FDCWD, roh, AT_FDCWD, rnh, rxflags) < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        if (g_rootfs) {
            // both ends confined (TOCTOU-free). Copy a lower-only SOURCE up first so renameatx_np finds it
            // in the writable upper (jail_at already materializes the dest's upper parent via overlay_mkparents).
            overlay_copyup_at((int)a0, (const char *)a1);
            char ofin[512], nfin[512];
            int opfd = jail_at((int)a0, (const char *)a1, ofin, sizeof ofin, 1);
            if (opfd < 0) {
                G_RET(c) = (uint64_t)(int64_t)opfd;
                break;
            }
            int npfd = jail_at((int)a2, (const char *)a3, nfin, sizeof nfin, 1);
            if (npfd < 0) {
                close(opfd);
                G_RET(c) = (uint64_t)(int64_t)npfd;
                break;
            }
            char dp[4200];
            if (fcntl(opfd, F_GETPATH, dp) == 0) {
                char hp[4400];
                snprintf(hp, sizeof hp, "%s/%s", dp, ofin);
                mc_evict(hp);
                ac_evict(hp);
            }
            int r = renameatx_np(opfd, ofin, npfd, nfin, rxflags), e = errno;
            close(opfd);
            close(npfd);
            // Overlay: a plain move (not RENAME_EXCHANGE) of a file the image lower still provides leaves the
            // copied-up upper source moved away but the lower copy exposed -> the source would re-appear. Drop
            // a whiteout at the source so it stays gone (real overlayfs rename semantics). No-op outside overlay.
            if (r == 0 && !(rxflags & RENAME_SWAP)) {
                char sgp[4200];
                abs_guest((int)a0, (const char *)a1, sgp, sizeof sgp);
                if (overlay_lower_has(sgp)) overlay_whiteout(sgp);
            }
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : 0;
            break;
        }
        char ob[4200], nb[4200];
        const char *op = atpath((int)a0, (const char *)a1, ob, sizeof ob, 0);
        const char *np = atpath((int)a2, (const char *)a3, nb, sizeof nb, 0);
        G_RET(c) = renameatx_np(ATFD(a0), op, ATFD(a2), np, rxflags) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 40:
    case 39:
    // mount / umount2 / pivot_root -> ok
    case 41: G_RET(c) = 0; break;
    case 43:
    case 44: {
        // statfs(path,buf)/fstatfs(fd,buf): wrap the host call, then TRANSLATE the macOS struct statfs
        // into the Linux struct statfs layout (all 8-byte fields on 64-bit; f_fsid is two 32-bit words).
        struct statfs hs;
        int r;
        if (nr == 43) {
            char pb[4200];
            const char *p = atpath(-100, (const char *)a0, pb, sizeof pb, 0);
            r = statfs(p, &hs);
        } else {
            r = fstatfs((int)a0, &hs);
        }
        if (r < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        uint8_t *b = (uint8_t *)a1;
        if (!host_range_mapped((uintptr_t)a1, 120)) { G_RET(c) = (uint64_t)(int64_t)(-EFAULT); break; }
        memset(b, 0, 120);
        *(int64_t *)(b + 0) = 0x01021994;              // f_type (TMPFS_MAGIC; geometry is what matters)
        *(int64_t *)(b + 8) = (int64_t)hs.f_bsize;     // f_bsize
        *(uint64_t *)(b + 16) = (uint64_t)hs.f_blocks; // f_blocks
        *(uint64_t *)(b + 24) = (uint64_t)hs.f_bfree;  // f_bfree
        *(uint64_t *)(b + 32) = (uint64_t)hs.f_bavail; // f_bavail
        *(uint64_t *)(b + 40) = (uint64_t)hs.f_files;  // f_files
        *(uint64_t *)(b + 48) = (uint64_t)hs.f_ffree;  // f_ffree
        *(int32_t *)(b + 56) = hs.f_fsid.val[0];       // f_fsid[0]
        *(int32_t *)(b + 60) = hs.f_fsid.val[1];       // f_fsid[1]
        *(int64_t *)(b + 64) = 255;                    // f_namelen (NAME_MAX)
        *(int64_t *)(b + 72) = (int64_t)hs.f_bsize;    // f_frsize
        *(int64_t *)(b + 80) = 0;                      // f_flags
        G_RET(c) = 0;
        break;
    }
    case 46: {
        // ftruncate on a RAM-backed scratch file (spill past the cap)
        if (memf_get((int)a0) && memf_room_or_spill((int)a0, (off_t)a1)) {
            struct memf *m = g_memf[(int)a0];
            off_t len = (off_t)a1;
            if (len < 0) {
                G_RET(c) = (uint64_t)(-EINVAL);
                break;
            }
            if ((size_t)len > m->size) {
                if (memf_reserve(m, (size_t)len)) {
                    G_RET(c) = (uint64_t)(-ENOMEM);
                    break;
                }
                atomic_fetch_add(&g_memf_total, (uint64_t)len - m->size);
            } else {
                atomic_fetch_sub(&g_memf_total, m->size - (uint64_t)len);
                if ((size_t)len < m->cap) memset(m->buf + len, 0, m->size - (size_t)len); // re-zero shrunk tail
            }
            m->size = (size_t)len;
            G_RET(c) = 0;
            break;
        }
        int r = ftruncate((int)a0, (off_t)a1);
        fd_evict((int)a0);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
        // ftruncate
    }
    case 47: {
        // fallocate(fd,mode,offset,len). FALLOC_FL_PUNCH_HOLE(2)|KEEP_SIZE(1): deallocate+zero a range
        // via macOS F_PUNCHHOLE (file stays the same size, the range reads as zeros).
        memf_materialize((int)a0); // rare on scratch: flush RAM cache, then use the host fallocate path
        int mode = (int)a1;
        off_t off = (off_t)a2, len = (off_t)a3;
        if (mode & 2) {
#ifdef F_PUNCHHOLE
            struct fpunchhole fph;
            memset(&fph, 0, sizeof fph);
            fph.fp_offset = off;
            fph.fp_length = len;
            int r = fcntl((int)a0, F_PUNCHHOLE, &fph);
            fd_evict((int)a0);
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
#else
            G_RET(c) = (uint64_t)(-EOPNOTSUPP);
#endif
            break;
        }
        struct stat s;
        // plain fallocate: extend (no shrink)
        off_t end = off + len;
        if (fstat((int)a0, &s) == 0 && s.st_size < end && ftruncate((int)a0, end) < 0) {}
        fd_evict((int)a0);
        G_RET(c) = 0;
        break;
    }
    case 49: {
        char pb[4200];
        // chdir (confined; tracks guest cwd)
        const char *p = atpath(-100, (const char *)a0, pb, sizeof pb, 0);
        if (chdir(p) < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        // Track the guest cwd from the host path the dir resolved to (handles the upper, any lower, or a
        // volume) -- relative/"."/AT_FDCWD resolution joins g_cwd, so a stale value sends `ls` to the wrong dir.
        if (g_rootfs) guest_from_host(p, g_cwd, sizeof g_cwd);
        G_RET(c) = 0;
        break;
    }
    case 50: {
        if (fchdir((int)a0) < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
            // fchdir (tracks guest cwd)
        }
        if (g_rootfs && (int)a0 >= 0 && (int)a0 < 1024 && g_fdpath[(int)a0][0])
            guest_from_host(g_fdpath[(int)a0], g_cwd, sizeof g_cwd);
        G_RET(c) = 0;
        break;
    }
    // fchmod(fd, mode)
    case 52: G_RET(c) = fchmod((int)a0, (mode_t)a1) < 0 ? (uint64_t)(-errno) : 0; break;
    case 53:
    // fchmodat(dirfd,path,mode,flags) / fchmodat2
    case 452: {
        if (jail_ro_at((int)a0, (const char *)a1)) {
            G_RET(c) = (uint64_t)(int64_t)(-EROFS);
            break;
        }
        if (g_rootfs) {
            overlay_copyup_at((int)a0, (const char *)a1); // bring a lower-only target up so jail_at finds it
            char fin[512];
            int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, 0);
            if (pfd < 0) {
                G_RET(c) = (uint64_t)(int64_t)pfd;
                break;
            }
            int r = fchmodat(pfd, fin, (mode_t)a2, 0), e = errno;
            char dp[4200];
            if (r >= 0 && fcntl(pfd, F_GETPATH, dp) == 0) {
                char hp[4400];
                snprintf(hp, sizeof hp, "%s/%s", dp, fin);
                mc_evict(hp);
            }
            close(pfd);
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : 0;
            break;
        }
        char pb[4200];
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb, 0);
        int r = fchmodat(ATFD(a0), p, (mode_t)a2, 0);
        if (r >= 0) mc_evict(p);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // fchownat(dirfd,path,uid,gid,flags) -- best-effort (rootless)
    case 54: {
        if (jail_ro_at((int)a0, (const char *)a1)) {
            G_RET(c) = (uint64_t)(int64_t)(-EROFS);
            break;
        }
        if (g_rootfs) {
            overlay_copyup_at((int)a0, (const char *)a1); // bring a lower-only target up so jail_at finds it
            char fin[512];
            int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, (a4 & 0x100) ? 1 : 0);
            if (pfd < 0) {
                G_RET(c) = (uint64_t)(int64_t)pfd;
                break;
            }
            int nofollow = (a4 & 0x100) ? 1 : 0;
            fchownat(pfd, fin, (uid_t)a2, (gid_t)a3, nofollow ? AT_SYMLINK_NOFOLLOW : 0);
            // BUG #181: the host chown is a rootless no-op; persist the guest-set owner as an xattr on
            // the backing file so a later stat reports it (not the #156 cuid/cgid default). -1 = keep.
            char dp[4200];
            if (fcntl(pfd, F_GETPATH, dp) == 0) {
                char hp[4400];
                snprintf(hp, sizeof hp, "%s/%s", dp, fin);
                chown_xattr_set_path(hp, (int)(int32_t)(uint32_t)a2, (int)(int32_t)(uint32_t)a3, nofollow);
            }
            close(pfd);
            G_RET(c) = 0;
            break;
            // EPERM on the host -> faked OK
        }
        char pb[4200];
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb, 0);
        fchownat(ATFD(a0), p, (uid_t)a2, (gid_t)a3, 0);
        chown_xattr_set_path(p, (int)(int32_t)(uint32_t)a2, (int)(int32_t)(uint32_t)a3, 0);
        G_RET(c) = 0;
        break;
    }
    case 55: {
        fchown((int)a0, (uid_t)a1, (gid_t)a2);
        chown_xattr_set_fd((int)a0, (int)(int32_t)(uint32_t)a1, (int)(int32_t)(uint32_t)a2);
        G_RET(c) = 0;
        break;
        // fchown(fd,uid,gid) -- best-effort
    }
    // openat2(dirfd, path, open_how*, size): unpack open_how { u64 flags; u64 mode; u64 resolve; } into
    // the openat arg positions, then share the full openat path (O_* xlate, overlay, jail). The RESOLVE_*
    // restriction flags are advisory here -- the rootfs jail already confines every resolution.
    case 437: {
        uint64_t *how = (uint64_t *)a2;
        a2 = how ? how[0] : 0; // open_how.flags -> openat flags
        a3 = how ? how[1] : 0; // open_how.mode  -> openat mode
    } /* fall through to openat */
    case 56: {
        // openat -- Linux O_* -> macOS O_* (they differ!)
        int lf = (int)a2, mf = lf & 0x3;
        // Read-only bind mount: any write-intent open (O_WRONLY/O_RDWR/O_CREAT/O_TRUNC/O_APPEND, incl.
        // O_TMPFILE which carries O_RDWR) under an `-v …:ro` volume fails EROFS -- exactly as the kernel
        // rejects a write-open on a read-only mount. A pure O_RDONLY open still succeeds. Checked up front
        // so neither O_TMPFILE nor the memoized open-cache walk below can slip a write through.
        if (((lf & 3) || (lf & 0x40) || (lf & 0x200) || (lf & 0x400)) && jail_ro_at((int)a0, (const char *)a1)) {
            G_RET(c) = (uint64_t)(int64_t)(-EROFS);
            break;
        }
        // O_TMPFILE (the __O_TMPFILE bit 0x400000 is arch-independent): create an unnamed, auto-cleaned
        // regular file inside the named directory by making one + immediately unlinking it (macOS has no
        // O_TMPFILE). The fd is a normal RW file with link count 0.
        if (lf & 0x400000) {
            char pb[4200];
            const char *dir = atpath((int)a0, (const char *)a1, pb, sizeof pb, 0);
            int dfd = open(dir, O_RDONLY | O_DIRECTORY);
            if (dfd < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
            }
            int fd = -1, e = ENOENT;
            for (int t = 0; t < 64; t++) {
                char nm[40];
                snprintf(nm, sizeof nm, ".dd_tmpfile_%d_%d", (int)getpid(), rand());
                fd = openat(dfd, nm, O_CREAT | O_EXCL | O_RDWR, (mode_t)(a3 ? a3 : 0600));
                e = errno;
                if (fd >= 0) {
                    unlinkat(dfd, nm, 0);
                    break;
                }
                if (e != EEXIST) break;
            }
            close(dfd);
            if (fd >= 0 && fd < 1024) {
                g_fdpath[fd][0] = 0;   // anonymous: no tracked path
                memf_attach(fd, 0, 0); // O_TMPFILE is unambiguously private scratch -> back it with RAM
            }
            G_RET(c) = fd < 0 ? (uint64_t)(-(int64_t)e) : (uint64_t)fd;
            break;
        }
        {
            // synthesize /proc/* (macOS has no /proc)
            const char *rp = (const char *)a1;
            // Resolve a RELATIVE target to its guest-absolute path so the /proc checks below fire even when
            // the guest opened e.g. "stat" or "<pid>/stat" relative to a /proc cwd (busybox top xchdir's to
            // /proc, then opens "<pid>/stat"). Absolute paths are untouched -> zero change for those callers;
            // a resolved non-/proc relative path matches none of the synth checks and the real open (which
            // uses the original a1) is unaffected.
            char gpb_syn[4200];
            if (rp && rp[0] != '/') {
                abs_guest((int)a0, rp, gpb_syn, sizeof gpb_syn);
                rp = gpb_syn;
            }
            // abs_guest emits "/<gdir>/<name>", so a gdir tracked as "/proc" (a materialized proc dir fd)
            // yields a leading "//proc/..." -- collapse it so the /proc checks below match. This is what
            // makes htop's relative openat(pid_dirfd, "stat"/"task"/...) re-enter the /proc synthesis.
            while (rp && rp[0] == '/' && rp[1] == '/')
                rp++;
            // opendir("/proc"): materialize the process table (numeric pid dir per live container process
            // + the synthesized static files) so getdents enumerates the whole container -- `ps`/top/htop
            // read this to find processes. Without it the empty rootfs /proc dir yielded an empty table.
            if (rp && g_rootfs && (!strcmp(rp, "/proc") || !strcmp(rp, "/proc/"))) {
                int d = proc_root_dir_open();
                if (d >= 0) {
                    G_RET(c) = (uint64_t)d;
                    break;
                }
                // else fall through to the real (empty) rootfs /proc
            }
            if (rp && !strncmp(rp, "/proc/", 6)) {
                // /proc/<pid>, /proc/<pid>/task, /proc/<pid>/task/<tid> as DIRECTORIES: materialize a temp
                // dir so opendir/getdents work and htop can descend (it opens each pid as an O_DIRECTORY fd
                // and reads task/<tid>/stat). Per-pid FILES return -2 -> served by proc_open below.
                int pd = proc_dir_try_open(rp);
                if (pd != -2) {
                    if (pd >= 0 && (lf & 0x80000)) fcntl(pd, F_SETFD, FD_CLOEXEC); // honor O_CLOEXEC
                    G_RET(c) = pd < 0 ? (uint64_t)(-errno) : (uint64_t)pd;
                    break;
                }
                // /proc/[self|pid]/exe -> open the actual guest executable (the magic symlink target)
                char ep[1024];
                if (proc_self_exe(rp, ep, sizeof ep)) {
                    char hb[4200];
                    const char *hp = xresolve_overlay(ep, hb, sizeof hb);
                    int ef = open(hp, O_RDONLY);
                    if (ef >= 0 && (lf & 0x80000)) fcntl(ef, F_SETFD, FD_CLOEXEC); // honor O_CLOEXEC
                    G_RET(c) = ef < 0 ? (uint64_t)(-errno) : (uint64_t)ef;
                    break;
                }
                // /proc/[self|pid]/auxv (rustix/libc read it)
                if (strstr(rp, "/auxv")) {
                    char tn[] = "/tmp/.ddauxvXXXXXX";
                    int afd = mkstemp(tn);
                    if (afd >= 0) {
                        unlink(tn);
                        if (write(afd, g_auxv_data, g_auxv_len) < 0) {}
                        lseek(afd, 0, SEEK_SET);
                    }
                    G_RET(c) = afd < 0 ? (uint64_t)(-errno) : (uint64_t)afd;
                    break;
                }
                // cpuinfo/meminfo/stat/mounts/uptime/loadavg/version
                int pf = proc_open(rp);
                if (pf != -2) {
                    G_RET(c) = pf < 0 ? (uint64_t)(-errno) : (uint64_t)pf;
                    break;
                }
            }
            // cgroup v2 limit files (JVM/Go self-size on these)
            if (rp && !strncmp(rp, "/sys/fs/cgroup/", 15)) {
                int pf = proc_open(rp);
                if (pf != -2) {
                    G_RET(c) = pf < 0 ? (uint64_t)(-errno) : (uint64_t)pf;
                    break;
                }
            }
            // CPU topology sysfs: glibc __get_nprocs and tcmalloc NumPossibleCPUs read these to size
            // their per-CPU structures; an empty/missing file makes mongod abort.
            if (rp && !strncmp(rp, "/sys/devices/system/cpu/", 24)) {
                const char *leaf = rp + 24;
                if (!strcmp(leaf, "online") || !strcmp(leaf, "possible") || !strcmp(leaf, "present")) {
                    char rng[32];
                    cpu_range_str(rng, sizeof rng);
                    int d = synth_str_fd(rng);
                    G_RET(c) = d < 0 ? (uint64_t)(-errno) : (uint64_t)d;
                    break;
                }
            }
            // device nodes -> host devices (rootfs has no real /dev)
            if (rp && !strncmp(rp, "/dev/", 5)) {
                const char *hd = dev_node_hostpath(rp);
                if (hd) {
                    int d = open(hd, mf);
                    G_RET(c) = d < 0 ? (uint64_t)(-errno) : (uint64_t)d;
                    break;
                }
            }
        }
        if (lf & 0x40) mf |= O_CREAT;
        if (lf & 0x80) mf |= O_EXCL;
        if (lf & 0x200) mf |= O_TRUNC;
        if (lf & 0x400) mf |= O_APPEND;
        if (lf & 0x800) mf |= O_NONBLOCK;
        if (lf & G_O_DIRECTORY) mf |= O_DIRECTORY;
        if (lf & 0x80000) mf |= O_CLOEXEC;
        // #255: when a runtime-dropped process (gosu postgres) O_CREATs a file, the new inode must be
        // owned by its current fsuid/fsgid, not the cuid/cgid default. Only meaningful when O_CREAT is
        // set AND a cred drop makes the stamp differ from the default; the pre-existence probe (so we
        // never re-own a file merely OPENED with O_CREAT) then runs only in that rare dropped case.
        int nf_want = (lf & 0x40) && newfile_stamp_wanted();
        {
            // /proc/self/fd/N -> reopen what host fd N points at. Linux reopen gives a FRESH file
            // description (offset 0, access narrowed to the requested mode), so prefer reopening by the
            // F_GETPATH path with the guest's flags; for fds with no path (pipe/socket/anon) fall back to
            // dup(N), which at least hands back a working, equivalent fd. /dev/std{in,out,err} map to
            // fd 0/1/2 here (open-only; their readlink stays the on-disk symlink so `ls -l /dev` works).
            int pfn = procfd_num((const char *)a1);
            if (pfn < 0) pfn = dev_std_fd((const char *)a1);
            if (pfn >= 0) {
                memf_materialize(pfn); // reopen-by-fd would expose the real file -> flush RAM cache first
                char gp[4200];
                int r = -1;
                if (fcntl(pfn, F_GETPATH, gp) == 0 && gp[0]) r = open(gp, mf & ~(O_EXCL | O_CREAT), (mode_t)a3);
                if (r < 0) r = dup(pfn); // anonymous/pipe/socket fd -> share the description
                if (r >= 0) {
                    char tp[4200];
                    if (fcntl(r, F_GETPATH, tp) == 0) fd_setpath(r, tp);
                }
                G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
                break;
            }
        }
        {
            // POSIX shm: glibc shm_open opens /dev/shm/<name>; the rootfs has no tmpfs, so back it with a
            // real host file (MAP_SHARED + fork share it). Flatten any subdirs into the single filename.
            char hp[300];
            const char *sp = shm_hostpath((const char *)a1, hp, sizeof hp);
            if (sp) {
                int d = open(sp, mf, (mode_t)a3);
                G_RET(c) = d < 0 ? (uint64_t)(-errno) : (uint64_t)d;
                break;
            }
        }
        {
            // pty: /dev/ptmx -> posix_openpt; /dev/pts/N -> slave
            const char *rp = (const char *)a1;
            if (rp && !strcmp(rp, "/dev/ptmx")) {
                int m = posix_openpt(O_RDWR | O_NOCTTY);
                if (m >= 0) {
                    grantpt(m);
                    unlockpt(m);
                }
                G_RET(c) = m < 0 ? (uint64_t)(-errno) : (uint64_t)m;
                break;
            }
            if (rp && !strncmp(rp, "/dev/pts/", 9) && rp[9] >= '0' && rp[9] <= '9') {
                char *sn = ptsname(atoi(rp + 9));
                if (!sn) {
                    G_RET(c) = (uint64_t)(int64_t)(-2);
                    break;
                    // ENOENT
                }
                int s = open(sn, mf);
                G_RET(c) = s < 0 ? (uint64_t)(-errno) : (uint64_t)s;
                break;
            }
        }
        // OVERLAY: resolve across layers (upper shadows lowers)
        if (g_rootfs && g_nlower) {
            char gp[4200];
            abs_guest((int)a0, (const char *)a1, gp, sizeof gp);
            char host[4300];
            // O_WRONLY/O_RDWR/O_CREAT -> write
            int isw = (lf & 3) || (lf & 0x40);
            if (isw)
                // copy-up the lower file (or upper path to create)
                overlay_copyup(gp, host, sizeof host);
            else
                overlay_resolve(gp, host, sizeof host, (lf & G_O_NOFOLLOW) != 0);
            // #255: after copy-up, `host` (the upper path) exists iff the file was already present in the
            // overlay -> a missing upper means this open will CREATE it fresh; stamp its owner post-open.
            int nf_new = nf_want && access(host, F_OK) != 0;
            int r = open(host, mf | ((lf & G_O_NOFOLLOW) ? O_NOFOLLOW : 0), (mode_t)a3);
            if (r >= 0 && nf_new) newfile_stamp_fd(r);
            if (r >= 0) {
                char gpa[4200];
                int have_canon = fcntl(r, F_GETPATH, gpa) == 0;
                if (have_canon) {
                    fd_setpath(r, gpa);
                    if (isw) {
                        mc_evict(gpa);
                        rl_evict(gpa);
                        ac_evict(gpa);
                    }
                }
                // Remember the guest dir for merged getdents. Derive it from the fd's CANONICAL host path
                // (F_GETPATH already resolved `.`/`..`/symlinks per component) rather than the raw guest
                // arg: a `..` out of a nested bind mount (e.g. `/mnt/..`) keeps a mount-point component that
                // lives ONLY in the writable upper, so re-resolving the raw path per layer finds it in no
                // lower and enumerates the upper alone -- the merged root listing then dropped every
                // lower-only entry (bin, lib, usr...). The canonical path folds `/mnt/..` back to the rootfs
                // root, so overlay_readdir enumerates every layer. NOT for a bind-mount volume dir (its own
                // jail, in no layer): it must list via plain readdir of the host fd; tagging it overlay ->
                // overlay_readdir misses it -> an empty `ls` on the mount.
                char gdir[4200];
                if (have_canon) guest_from_host(gpa, gdir, sizeof gdir);
                else snprintf(gdir, sizeof gdir, "%s", gp);
                if (r < 1024 && !jail_is_vol(gdir)) snprintf(g_ovldir[r], sizeof g_ovldir[r], "%s", gdir);
            }
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
            break;
        }
        // TOCTOU-free per-component resolve in the jail
        if (g_rootfs) {
            // W4D: openat resolution cache. Memoizes the guest-abs-path -> canonical host path that the
            // jail walk below produces, so a REPEATED open of the same path collapses the ~6-syscall
            // per-component walk to a single open(host, O_NOFOLLOW). The real open ALWAYS still runs (no
            // fabricated existence/contents); a stale entry can only ever be the wrong PATH, which the
            // shared g_res_epoch (bumped above on every FS mutation, incl. this case's O_CREAT) prevents.
            // EXCLUDE O_CREAT/O_EXCL/O_TRUNC (mutating/creating) and O_DIRECTORY (deep-host-path reopen
            // regressed; see optimization-research/w4d-openat.md). Kill switch: W4_NOOPENCACHE=1.
            int cacheable = !(lf & (0x40 | 0x80 | 0x200 | G_O_DIRECTORY));
            char gkey[4200], hostc[4200];
            if (cacheable) abs_guest((int)a0, (const char *)a1, gkey, sizeof gkey);
            if (cacheable && oc_lookup(gkey, hostc, sizeof hostc)) {
                // ONE atomic open replaces the per-component walk; hostc is already canonical+symlink-free.
                int r = open(hostc, mf | O_NOFOLLOW, (mode_t)a3);
                int e = errno;
                if (r >= 0) {
                    fd_setpath(r, hostc);
                    if (lf & 3) { // write-open: keep the metadata caches coherent (same as the walk path)
                        mc_evict(hostc);
                        rl_evict(hostc);
                        ac_evict(hostc);
                    }
                }
                G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : (uint64_t)r;
                break;
            }
            char fin[512];
            // resolve following the final symlink unless the guest asked O_NOFOLLOW (per-arch bit)
            int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, (lf & G_O_NOFOLLOW) != 0);
            if (pfd < 0) {
                G_RET(c) = (uint64_t)(int64_t)pfd;
                break;
            }
            // fin is resolved -> O_NOFOLLOW safe
            // #255: probe pre-existence (relative to the resolved parent) so we stamp ONLY a fresh create.
            int nf_new = nf_want && faccessat(pfd, fin, F_OK, AT_SYMLINK_NOFOLLOW) != 0;
            int r = openat(pfd, fin, mf | O_NOFOLLOW, (mode_t)a3);
            int e = errno;
            close(pfd);
            if (r >= 0 && nf_new) newfile_stamp_fd(r);
            if (r >= 0) {
                char gp[4200];
                // canonical host path for tracking
                if (fcntl(r, F_GETPATH, gp) == 0) {
                    fd_setpath(r, gp);
                    if ((lf & 3) || (lf & 0x40) || (lf & 0x200)) {
                        mc_evict(gp);
                        rl_evict(gp);
                        ac_evict(gp);
                    }
                    // W4D: memoize this walk's result (gp = F_GETPATH = canonical in-jail host path) so the
                    // next open of the same guest path is a single open(). oc_store re-checks in-jail+epoch.
                    if (cacheable) oc_store(gkey, gp);
                }
            }
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : (uint64_t)r;
            break;
        }
        char pb[4200];
        // no jail
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb, 0);
        int nf_new = nf_want && faccessat(ATFD(a0), p, F_OK, AT_SYMLINK_NOFOLLOW) != 0; // #255: stamp only fresh
        int r = openat(ATFD(a0), p, mf, (mode_t)a3);
        if (r >= 0 && nf_new) newfile_stamp_fd(r);
        if (r >= 0) {
            fd_setpath(r, p);
            if ((lf & 3) || (lf & 0x40) || (lf & 0x200)) {
                mc_evict(p);
                rl_evict(p);
                ac_evict(p);
            }
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 57: {
        int cf = (int)a0;
        engine_fd_vacate(cf); // guest close must not clobber an engine-private fd (g_root_fd etc.) on this number
        if (cf >= 0 && cf < 1024) {
            if (g_eventfd_peer[cf]) {
                close(g_eventfd_peer[cf] - 1);
                g_eventfd_peer[cf] = 0;
            }
            g_timerfd[cf] = 0;
            g_ovldir[cf][0] = 0;
            g_lo_port[cf] = 0;
            g_sock_stream[cf] = 0;
            g_sock_dgram[cf] = 0;
            // SEQPACKET/O_DIRECT-pipe EOF: this end is backed by a DGRAM socket whose peer would otherwise
            // never see EOF on macOS. Inject a zero-length datagram so a blocked peer recv wakes and returns
            // 0 (it queues after any pending data, preserving order), then drop the marker. (See case 199.)
            seq_send_eof(cf);
            g_sock_seqpacket[cf] = 0;
            g_br_port[cf] = 0;
            g_br_ip[cf] = 0;
            g_eventfd_count[cf] = 0;
            g_eventfd_sema[cf] = 0;
            ep_fd_reset(cf); // w3e: drop epoll armed-state (kqueue auto-removes a closed fd)
            flock_on_close(cf); // release any flock this fd held on its companion (before the real fd is closed)
            // reap eventfd peer / timerfd / overlay dir / loopback
        }
        memf_close(cf);   // release any RAM-backed scratch buffer
        dirs_drop(cf);    // invalidate the getdents DIR* cache so a reused fd re-opendir's
        ovldents_drop(cf); // invalidate the overlay getdents snapshot so a reused fd re-snapshots (not stale tail)
        int r = close(cf);
        fd_clear(cf);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
        // close: -errno on fail
    }
    // getdents64
    case 61: {
        int fd = (int)a0;
        // OVERLAY: merged listing across layers
        if (g_nlower && fd >= 0 && fd < 1024 && g_ovldir[fd][0]) {
            // snapshot cache is indexed directly by guest fd (no slot table -> no eviction thrash, #199)
            if (!g_ovldents[fd].taken) {
                g_ovldents[fd].taken = 1;
                g_ovldents[fd].pos = 0;
                g_ovldents[fd].n = overlay_readdir(g_ovldir[fd], &g_ovldents[fd].nm, &g_ovldents[fd].ty);
            }
            uint8_t *out = (uint8_t *)a1;
            size_t o = 0;
            while (g_ovldents[fd].pos < g_ovldents[fd].n) {
                const char *nm = g_ovldents[fd].nm[g_ovldents[fd].pos];
                size_t nl = strlen(nm), lr = (19 + nl + 1 + 7) & ~7ull;
                if (o + lr > (size_t)a2) break;
                uint8_t *ld = out + o;
                *(uint64_t *)(ld + 0) = g_ovldents[fd].pos + 1;
                *(uint64_t *)(ld + 8) = o + lr;
                *(uint16_t *)(ld + 16) = (uint16_t)lr;
                *(ld + 18) = g_ovldents[fd].ty[g_ovldents[fd].pos];
                memcpy(ld + 19, nm, nl);
                ld[19 + nl] = 0;
                o += lr;
                g_ovldents[fd].pos++;
            }
            // exhausted -> free the snapshot (releases the heap arrays too)
            if (o == 0) ovldents_free(fd);
            G_RET(c) = (uint64_t)o;
            break;
        }
        DIR *dir = NULL;
        for (int i = 0; i < g_ndirs; i++)
            if (g_dirs[i].fd == fd) {
                dir = g_dirs[i].d;
                break;
            }
        if (!dir) {
            dir = fdopendir(dup(fd));
            if (!dir) {
                G_RET(c) = (uint64_t)(-errno);
                break;
            }
            if (g_ndirs < 64) {
                g_dirs[g_ndirs].fd = fd;
                g_dirs[g_ndirs].d = dir;
                g_ndirs++;
            }
        }
        uint8_t *out = (uint8_t *)a1;
        size_t o = 0;
        struct dirent *de;
        long pos = telldir(dir);
        while ((de = readdir(dir))) {
            size_t nl = strlen(de->d_name), lr = (19 + nl + 1 + 7) & ~7ull;
            if (o + lr > (size_t)a2) {
                seekdir(dir, pos);
                break;
            }
            uint8_t *ld = out + o;
            *(uint64_t *)(ld + 0) = de->d_ino;
            *(uint64_t *)(ld + 8) = o + lr;
            *(uint16_t *)(ld + 16) = (uint16_t)lr;
            *(ld + 18) = de->d_type;
            memcpy(ld + 19, de->d_name, nl);
            ld[19 + nl] = 0;
            o += lr;
            pos = telldir(dir);
        }
        G_RET(c) = o;
        break;
    }
    // readlinkat
    case 78: {
        const char *p = (const char *)a1;
        char *buf = (char *)a2;
        size_t bs = (size_t)a3;
        // /proc/self (and /proc/thread-self) are magic symlinks to the caller's own pid -- readlink returns
        // the decimal pid. `ls -l /proc` readlinks it now that /proc lists a "self" entry.
        if (p && (!strcmp(p, "/proc/self") || !strcmp(p, "/proc/thread-self"))) {
            char num[16];
            int l = snprintf(num, sizeof num, "%d", container_pid());
            if ((size_t)l > bs) l = (int)bs;
            memcpy(buf, num, (size_t)l);
            G_RET(c) = (uint64_t)l;
            break;
        }
        // /proc/self/fd/N -> the path host fd N currently points at (recovered via F_GETPATH on macOS).
        int pfn = procfd_num(p);
        if (pfn >= 0) {
            char gp[4200];
            if (fcntl(pfn, F_GETPATH, gp) != 0) {
                G_RET(c) = (uint64_t)(-errno); // bad fd -> EBADF
                break;
            }
            // map the host path back into the guest's view (strip the rootfs prefix if jailed)
            const char *gpath =
                (g_rootfs && !strncmp(gp, g_rootfs_canon, g_rootfs_canon_len)) ? gp + g_rootfs_canon_len : gp;
            if (!gpath[0]) gpath = "/";
            size_t l = strlen(gpath);
            if (l > bs) l = bs;
            memcpy(buf, gpath, l);
            G_RET(c) = l;
            break;
        }
        char ep[1024];
        if (proc_self_exe(p, ep, sizeof ep)) {
            size_t l = strlen(ep);
            if (l > bs) l = bs;
            memcpy(buf, ep, l);
            G_RET(c) = l;
        } else {
            char pb[4200];
            // Resolve through atpath (overlay-aware, nofollow=read the link itself, dirfd-relative confined):
            // a bare xlate() only consults the writable upper, so readlink of a lower-only path (e.g. a
            // PATH-launched binary in a read-only image layer) hit a non-existent upper path and returned
            // ENOENT instead of EINVAL -- breaking musl/glibc realpath(), which readlinks each path prefix
            // and treats ENOENT as "no such path" (PostgreSQL find_my_exec: "could not resolve path ...").
            const char *rp = atpath((int)a0, p, pb, sizeof pb, 1);
            int rc, len;
            if (rl_lookup(rp, &rc, buf, bs, &len)) {
                G_RET(c) = rc < 0 ? (uint64_t)(int64_t)rc : (uint64_t)len;
                break;
            }
            ssize_t r = readlink(rp, buf, bs);
            rl_store(rp, r < 0 ? -errno : (int)r, buf, r < 0 ? 0 : (int)r);
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        }
        break;
    }
    case 79: {
        struct stat s;
        // newfstatat(dfd, path, buf, flags)
        char pb[4200];
        // AT_SYMLINK_NOFOLLOW (0x100): lstat -- resolve the final component WITHOUT following it.
        const char *raw = (const char *)a1, *p = atpath((int)a0, raw, pb, sizeof pb, (a3 & 0x100) ? 1 : 0);
        {
            const char *gp = (g_rootfs && !strncmp(p, g_rootfs_canon, g_rootfs_canon_len)) ? p + g_rootfs_canon_len : p;
            char ep[1024];
            if (proc_self_exe(gp, ep, sizeof ep)) {
                struct stat es;
                if (a3 & 0x100) { // lstat: report the magic symlink itself
                    memset(&es, 0, sizeof es);
                    es.st_mode = S_IFLNK | 0777;
                    es.st_size = (off_t)strlen(ep);
                    es.st_nlink = 1;
                    fill_linux_stat((uint8_t *)a2, &es, NULL, -1); // synth /proc/self/exe symlink
                    G_RET(c) = 0;
                    break;
                }
                // stat (follow): stat the actual executable file through the jail
                char hb[4200];
                const char *hp = xresolve_overlay(ep, hb, sizeof hb);
                if (stat(hp, &es) == 0) {
                    fill_linux_stat((uint8_t *)a2, &es, hp, -1);
                    G_RET(c) = 0;
                    break;
                }
                // file unexpectedly missing -> fall through to the generic ENOENT path
            }
            if (synth_stat(gp, (uint8_t *)a2)) {
                G_RET(c) = 0;
                break;
            }
            // synthesized /proc or /sys file
        }
        // cacheable: named path, follow
        if (raw && raw[0] && !(a3 & 0x100)) {
            int rc;
            if (!mc_lookup(p, &rc, &s)) {
                int r = fstatat(ATFD(a0), p, &s, 0);
                rc = r < 0 ? -errno : 0;
                mc_store(p, rc, &s);
            }
            if (rc == 0) fill_linux_stat((uint8_t *)a2, &s, p, -1);
            G_RET(c) = (uint64_t)(int64_t)rc;
            break;
        }
        // AT_EMPTY_PATH -> fstat(dfd)
        int empty_self = (raw && !raw[0] && (a3 & 0x1000));
        int r = (empty_self && memf_get((int)a0)) ? memf_fstat((int)a0, &s)
                : empty_self                      ? fstat((int)a0, &s)
                                                  : fstatat(ATFD(a0), p, &s, AT_SYMLINK_NOFOLLOW);
        if (r < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        // guest-chown xattr lives on the host backing file: read via fd for AT_EMPTY_PATH, else by path
        fill_linux_stat((uint8_t *)a2, &s, empty_self ? NULL : p, empty_self ? (int)a0 : -1);
        G_RET(c) = 0;
        break;
    }
    case 80: {
        // fstat(fd, buf)
        struct stat s;
        int sr = memf_get((int)a0) ? memf_fstat((int)a0, &s) : fstat((int)a0, &s);
        if (sr < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        fill_linux_stat((uint8_t *)a1, &s, NULL, (int)a0);
        G_RET(c) = 0;
        break;
    }
    case 81:
        sync();
        G_RET(c) = 0;
        // sync
        break;
    // syncfs(fd): no macOS syncfs -> flush this fd then sync the system. RAM-backed scratch is a no-op.
    case 267:
        if (!memf_get((int)a0)) {
            fsync((int)a0);
            sync();
        }
        G_RET(c) = 0;
        break;
    // utimensat(dirfd, path, times, flags)
    case 88: {
        struct timespec *ts = (struct timespec *)a2;
        if (!a1) {
            G_RET(c) = futimens((int)a0, ts) < 0 ? (uint64_t)(-errno) : 0;
            break;
            // path NULL -> futimens(fd)
        }
        if (jail_ro_at((int)a0, (const char *)a1)) {
            G_RET(c) = (uint64_t)(int64_t)(-EROFS);
            break;
        }
        if (g_rootfs) {
            overlay_copyup_at((int)a0, (const char *)a1); // bring a lower-only target up so jail_at finds it
            char fin[512];
            int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, (a3 & 0x100) ? 1 : 0);
            if (pfd < 0) {
                G_RET(c) = (uint64_t)(int64_t)pfd;
                break;
            }
            int r = utimensat(pfd, fin, ts, (a3 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0), e = errno;
            char dp[4200];
            if (r >= 0 && fcntl(pfd, F_GETPATH, dp) == 0) {
                char hp[4400];
                snprintf(hp, sizeof hp, "%s/%s", dp, fin);
                mc_evict(hp);
                // mtime changed
            }
            close(pfd);
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : 0;
            break;
        }
        char pb[4200];
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb, 0);
        int r = utimensat(ATFD(a0), p, ts, (a3 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0);
        if (r >= 0) mc_evict(p);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // umask -> old mask
    case 166: G_RET(c) = (uint64_t)umask((mode_t)a0); break;
    // fadvise64 -- advisory no-op
    case 223: G_RET(c) = 0; break;
    case 291: {
        struct stat s;
        // statx(dfd, path, flags, mask, buf)
        char pb[4200];
        int nofollow = (a2 & 0x100); // AT_SYMLINK_NOFOLLOW: stat the link itself, don't dereference
        const char *raw = (const char *)a1;
        // Validate the guest pointers before any deref: a bad path or result buffer must return -EFAULT,
        // not fault the engine (guest memory is identity-mapped host memory). atpath/raw[0] read the path;
        // the 256-byte struct statx is written to a4 below.
        if (raw && !host_addr_mapped((uintptr_t)raw)) { G_RET(c) = (uint64_t)(int64_t)(-EFAULT); break; }
        if (!host_range_mapped((uintptr_t)a4, 256)) { G_RET(c) = (uint64_t)(int64_t)(-EFAULT); break; }
        const char *p = atpath((int)a0, raw, pb, sizeof pb, nofollow);
        int rc, empty = (raw && !raw[0] && (a2 & 0x1000));
        const char *gp = (g_rootfs && !strncmp(p, g_rootfs_canon, g_rootfs_canon_len)) ? p + g_rootfs_canon_len : p;
        char ep[1024];
        if (proc_self_exe(gp, ep, sizeof ep)) {
            // /proc/[self|pid]/exe magic symlink -> the running executable
            if (nofollow) {
                memset(&s, 0, sizeof s);
                s.st_mode = S_IFLNK | 0777;
                s.st_size = (off_t)strlen(ep);
                s.st_nlink = 1;
                rc = 0;
            } else {
                char hb[4200];
                const char *hp = xresolve_overlay(ep, hb, sizeof hb);
                rc = stat(hp, &s) == 0 ? 0 : -errno;
            }
        } else if (synth_stat_raw(gp, &s)) {
            rc = 0;
            // synth /proc or /sys -> fill from s below
        }
        // cacheable (only the follow case -- the path cache doesn't distinguish follow vs nofollow)
        else if (raw && raw[0] && !empty && !nofollow) {
            if (!mc_lookup(p, &rc, &s)) {
                int rr = fstatat(ATFD(a0), p, &s, 0);
                rc = rr < 0 ? -errno : 0;
                mc_store(p, rc, &s);
            }
        } else {
            int rr = (empty && memf_get((int)a0)) ? memf_fstat((int)a0, &s)
                     : empty                      ? fstat((int)a0, &s)
                                                  : fstatat(ATFD(a0), p, &s, nofollow ? AT_SYMLINK_NOFOLLOW : 0);
            rc = rr < 0 ? -errno : 0;
        }
        if (rc < 0) {
            G_RET(c) = (uint64_t)(int64_t)rc;
            break;
        }
        uint8_t *d = (uint8_t *)a4;
        // struct statx (correct offsets)
        memset(d, 0, 256);
        *(uint32_t *)(d + 0) = 0x17ff;
        // stx_mask (BTIME|basic), stx_blksize
        *(uint32_t *)(d + 4) = 4096;
        // stx_nlink @16
        *(uint32_t *)(d + 16) = s.st_nlink ? s.st_nlink : 1;
        *(uint32_t *)(d + 20) = s.st_uid;
        // stx_uid@20 stx_gid@24
        *(uint32_t *)(d + 24) = s.st_gid;
        // stx_mode @28  <-- was @36 (the bug)
        *(uint16_t *)(d + 28) = (uint16_t)s.st_mode;
        // stx_ino @32
        *(uint64_t *)(d + 32) = s.st_ino;
        // stx_size @40
        *(uint64_t *)(d + 40) = (uint64_t)s.st_size;
        // stx_blocks @48
        *(uint64_t *)(d + 48) = (uint64_t)s.st_blocks;
        *(int64_t *)(d + 64) = s.st_atime;
        // stx_atime@64 stx_ctime@96
        *(int64_t *)(d + 96) = s.st_ctime;
        // stx_mtime @112 (sec)
        *(int64_t *)(d + 112) = s.st_mtime;
        G_RET(c) = 0;
        break;
    }
    // name_to_handle_at(dfd, path, file_handle*, mount_id*, flags): macOS has no FS file handles, so
    // synthesize a stable 16-byte handle from st_dev+st_ino (round-trips file identity). file_handle is
    // { u32 handle_bytes; i32 handle_type; u8 f_handle[]; }; handle_bytes is the buffer size on input
    // and is rewritten to the produced size (-EOVERFLOW if the caller's buffer is too small).
    case 264: {
        uint8_t *fh = (uint8_t *)a2;
        if (!fh || !host_range_mapped((uintptr_t)a2, 4)) { // handle_bytes read/write below
            G_RET(c) = (uint64_t)(int64_t)(-EFAULT);
            break;
        }
        int empty = (a4 & 0x1000);     // AT_EMPTY_PATH
        int nofollow = !(a4 & 0x400);  // default: don't dereference the final symlink (AT_SYMLINK_FOLLOW=0x400)
        struct stat s;
        char pb[4200];
        int rr;
        if (empty && memf_get((int)a0)) rr = memf_fstat((int)a0, &s);
        else if (empty) rr = fstat((int)a0, &s);
        else {
            const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb, nofollow);
            rr = fstatat(ATFD(a0), p, &s, nofollow ? AT_SYMLINK_NOFOLLOW : 0);
        }
        if (rr < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        const uint32_t need = 16; // dev(8) + ino(8)
        if (*(uint32_t *)(fh + 0) < need) {
            *(uint32_t *)(fh + 0) = need;
            G_RET(c) = (uint64_t)(int64_t)(-EOVERFLOW);
            break;
        }
        if (!host_range_mapped((uintptr_t)a2, need + 8)) { G_RET(c) = (uint64_t)(int64_t)(-EFAULT); break; }
        uint64_t dev = (uint64_t)s.st_dev, ino = (uint64_t)s.st_ino;
        *(uint32_t *)(fh + 0) = need; // handle_bytes
        *(int32_t *)(fh + 4) = 1;     // handle_type (stable, arbitrary)
        memcpy(fh + 8, &dev, 8);
        memcpy(fh + 16, &ino, 8);
        if (a3) {
            if (!host_range_mapped((uintptr_t)a3, sizeof(int))) { G_RET(c) = (uint64_t)(int64_t)(-EFAULT); break; }
            *(int *)a3 = (int)s.st_dev; // mount_id
        }
        G_RET(c) = 0;
        break;
    }
    // faccessat2(dirfd,path,mode,flags) -- glibc access() uses it; same path/confinement, flags ignored
    case 439:
    case 48: {
        char pb[4200];
        // /proc/[self|pid]/exe magic symlink -> access the actual executable
        char ep[1024];
        if (proc_self_exe((const char *)a1, ep, sizeof ep)) {
            char hb[4200];
            const char *hp = xresolve_overlay(ep, hb, sizeof hb);
            int r = access(hp, (int)a2);
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        // pseudo /dev char devices (open() backs them with a host node) must also test as present: e.g.
        // libgcrypt probes access("/dev/urandom",R_OK) to pick its RNG module -- an ENOENT there aborts
        // gpgv and breaks `apt-get update`. Test the host device with the requested mode.
        {
            const char *hd = dev_node_hostpath((const char *)a1);
            if (hd) {
                int r = access(hd, (int)a2);
                G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
                break;
            }
        }
        // faccessat
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb, 0);
        // F_OK existence check: cacheable
        if (a2 == 0 && p) {
            int rc;
            if (!ac_lookup(p, &rc)) {
                int r = faccessat(ATFD(a0), p, 0, 0);
                rc = r < 0 ? -errno : 0;
                ac_store(p, rc);
            }
            G_RET(c) = (uint64_t)(int64_t)rc;
            break;
        }
        int r = faccessat(ATFD(a0), p, (int)a2, 0);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    default:
        return 0;
    }
    return svc_done(c); // boundary errno xlate (host macOS -> Linux); see helpers.c svc_done
}
