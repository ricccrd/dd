// dd/runtime/os/linux -- service(): the Linux syscall layer (the "kernel" the guest talks to).
// Dispatches the guest syscall number to the host, translating the ABI (errno, struct layouts, flags,
// fd semantics); every path argument is resolved through the container VFS jail. One sorted switch,
// grouped by category. See docs/SYSCALLS.md for the per-syscall table.

static void service(struct cpu *c) {
    uint64_t nr = c->x[8], a0 = c->x[0], a1 = c->x[1], a2 = c->x[2],
             a3 = c->x[3], a4 = c->x[4], a5 = c->x[5];
    if (g_trace) fprintf(stderr, "[sys] %llu (%llx,%llx,%llx)\n",(unsigned long long)nr,(unsigned long long)a0,(unsigned long long)a1,(unsigned long long)a2);
    switch (nr) {
    // ===================== I/O — read/write/seek (+ eventfd/timerfd/signalfd fd redirection) =====================
    case 62: { off_t r = lseek((int)a0, (off_t)a1, (int)a2); c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 63: { int rfd = (int)a0;
               if (rfd >= 0 && rfd == g_sigfd_read) {                                   // signalfd read -> struct signalfd_siginfo
                   char b; ssize_t pr = read(rfd, &b, 1);                               // drain one wake byte
                   if (pr <= 0) { c->x[0] = (uint64_t)(int64_t)(pr < 0 ? -errno : -EAGAIN); break; }
                   int sig = 0; uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST);
                   for (int s = 1; s < 64; s++) if ((p & (1ull << s)) && (g_sigfd_mask & (1ull << s))) { sig = s; break; }
                   if (sig) __atomic_and_fetch(&g_pending, ~(1ull << (unsigned)sig), __ATOMIC_SEQ_CST);
                   if (a1 && a2 >= 128) { memset((void *)a1, 0, 128); *(uint32_t *)a1 = (uint32_t)sig; }  // ssi_signo
                   c->x[0] = 128; break;
               }
               if (rfd >= 0 && rfd < 1024 && g_inotify[rfd]) {                          // inotify read -> struct inotify_event[]
                   struct kevent kv[32]; struct timespec zero = {0, 0}; int nb = fcntl(rfd, F_GETFL) & O_NONBLOCK;
                   int n = kevent(rfd, NULL, 0, kv, 32, nb ? &zero : NULL);
                   if (n <= 0) { c->x[0] = (uint64_t)(int64_t)(n < 0 ? -errno : -EAGAIN); break; }
                   uint8_t *out = (uint8_t *)a1; size_t off = 0;
                   for (int i = 0; i < n && off + 16 <= a2; i++) {
                       uint32_t f = kv[i].fflags, m = 0;
                       if (f & (NOTE_WRITE | NOTE_EXTEND)) m |= 0x2;                     // IN_MODIFY
                       if (f & NOTE_ATTRIB) m |= 0x4;                                    // IN_ATTRIB
                       if (f & NOTE_DELETE) m |= 0x400;                                  // IN_DELETE_SELF
                       if (f & NOTE_RENAME) m |= 0x800;                                  // IN_MOVE_SELF
                       *(int32_t *)(out + off) = (int32_t)kv[i].ident;                   // wd
                       *(uint32_t *)(out + off + 4) = m; *(uint32_t *)(out + off + 8) = 0; *(uint32_t *)(out + off + 12) = 0;  // mask,cookie,len
                       off += 16;
                   }
                   c->x[0] = (uint64_t)off; break;
               }
               if (rfd >= 0 && rfd < 1024 && g_timerfd[rfd]) {                          // timerfd read -> drain timer, return count
                   struct kevent kv; struct timespec zero = {0, 0}; int nb = fcntl(rfd, F_GETFL) & O_NONBLOCK;
                   int n = kevent(rfd, NULL, 0, &kv, 1, nb ? &zero : NULL);
                   if (n <= 0) { c->x[0] = (uint64_t)(int64_t)(n < 0 ? -errno : -EAGAIN); break; }   // EAGAIN
                   if (a1 && a2 >= 8) *(uint64_t *)a1 = (uint64_t)kv.data;
                   c->x[0] = 8; break;
               }
               ssize_t r = read(rfd, (void *)a1, (size_t)a2); c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 64: { int wfd = (int)a0; if (wfd >= 0 && wfd < 1024 && g_eventfd_peer[wfd]) wfd = g_eventfd_peer[wfd] - 1;  // eventfd -> pipe
               fd_evict(wfd); ssize_t r = write(wfd, (void *)a1, (size_t)a2); c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 65: { ssize_t r = readv((int)a0, (void *)a1, (int)a2); c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }   // readv
    case 66: { fd_evict((int)a0); ssize_t r = writev((int)a0, (void *)a1, (int)a2); c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }  // writev
    case 67: { ssize_t r = pread((int)a0, (void *)a1, (size_t)a2, (off_t)a3);          // pread64
               c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 68: { fd_evict((int)a0); ssize_t r = pwrite((int)a0, (void *)a1, (size_t)a2, (off_t)a3);  // pwrite64
               c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 71: {                                                                    // sendfile(out,in,off*,count)
        int outfd = (int)a0, infd = (int)a1; off_t *po = (off_t *)a2; size_t cnt = (size_t)a3;
        if (po) lseek(infd, *po, SEEK_SET);
        char bf[65536]; size_t tot = 0;
        while (tot < cnt) { size_t w = cnt - tot < sizeof bf ? cnt - tot : sizeof bf;
            ssize_t n = read(infd, bf, w); if (n <= 0) break;
            ssize_t wr = write(outfd, bf, n); if (wr < 0) break; tot += wr; if (wr < n) break; }
        if (po) *po += tot; c->x[0] = tot; break;
    }
    case 76: case 77: {                                          // splice(fd_in,off_in,fd_out,off_out,len,fl) / tee -> emulate
        int fin = (int)a0, fout = (int)a2; size_t len = (size_t)a4; if (len > 65536) len = 65536;
        static __thread char sb[65536]; ssize_t n;
        fd_evict(fout);
        if (nr == 76 && a1) n = pread(fin, sb, len, *(off_t *)a1); else n = read(fin, sb, len);   // splice off_in
        if (n <= 0) { c->x[0] = n < 0 ? (uint64_t)(-errno) : 0; break; }
        ssize_t w = (nr == 76 && a3) ? pwrite(fout, sb, n, *(off_t *)a3) : write(fout, sb, n);
        if (w < 0) { c->x[0] = (uint64_t)(-errno); break; }
        if (nr == 76 && a1) *(off_t *)a1 += w;  if (nr == 76 && a3) *(off_t *)a3 += w;
        c->x[0] = (uint64_t)w; break; }

    // ===================== Filesystem — open/stat/dir/link/perm/xattr/cwd, all path-confined to the rootfs jail =====================
    case 5: case 6: case 7: c->x[0] = 0; break;                  // setxattr/lsetxattr/fsetxattr -> ignore
    case 8: case 9: case 10: c->x[0] = (uint64_t)(-ENODATA); break;   // getxattr/... -> ENODATA (no such attr)
    case 11: case 12: case 13: c->x[0] = 0; break;               // listxattr/... -> empty list
    case 14: case 15: case 16: c->x[0] = 0; break;               // removexattr/... -> ok
    case 17: { if (g_rootfs) { size_t l = strlen(g_cwd);                               // getcwd -> the GUEST cwd (not the host path)
                   if (a0 && l + 1 <= a1) { memcpy((void *)a0, g_cwd, l + 1); c->x[0] = l + 1; } else c->x[0] = (uint64_t)(-ERANGE); break; }
               if (getcwd((char *)a0, (size_t)a1)) c->x[0] = strlen((char *)a0) + 1;
               else c->x[0] = (uint64_t)(-errno); break; }
    case 23: { int r = dup((int)a0);                                                   // dup
               if (r >= 0 && r < 1024 && (int)a0 >= 0 && (int)a0 < 1024) strcpy(g_fdpath[r], g_fdpath[(int)a0]);  // carry path
               c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 24: { int r = dup2((int)a0, (int)a1);                                         // dup3(old,new,flags)
               if (r >= 0 && (int)a1 >= 0 && (int)a1 < 1024 && (int)a0 >= 0 && (int)a0 < 1024) strcpy(g_fdpath[(int)a1], g_fdpath[(int)a0]);
               c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 25: { int lcmd = (int)a1;                                                    // fcntl -- Linux cmd# -> macOS (they diverge!)
        if (lcmd == 3) {                                                               // F_GETFL: macOS O_* -> Linux O_*
            int r = fcntl((int)a0, F_GETFL, 0); if (r < 0) { c->x[0] = (uint64_t)(-errno); break; }
            int lf = r & 0x3;                                                          // access mode identical
            if (r & 0x8) lf |= 0x400; if (r & 0x4) lf |= 0x800; if (r & 0x40) lf |= 0x2000;  // APPEND/NONBLOCK/ASYNC
            c->x[0] = (uint64_t)(unsigned)lf; break;
        }
        if (lcmd == 4) {                                                               // F_SETFL: Linux O_* -> macOS O_*
            int la = (int)a2, mf = 0;
            if (la & 0x400) mf |= 0x8; if (la & 0x800) mf |= 0x4; if (la & 0x2000) mf |= 0x40;  // APPEND/NONBLOCK/ASYNC
            int r = fcntl((int)a0, F_SETFL, mf); c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break;
        }
        if (lcmd == 5 || lcmd == 6 || lcmd == 7) {                                     // F_GETLK/SETLK/SETLKW: xlate struct flock + cmd
            int mc = lcmd == 5 ? F_GETLK : lcmd == 6 ? F_SETLK : F_SETLKW;             // macOS F_GETLK=7,SETLK=8,SETLKW=9
            uint8_t *lf = (uint8_t *)a2; struct flock fl; memset(&fl, 0, sizeof fl);   // Linux flock: type/whence/pad/start@8/len@16/pid@24
            short lt = *(short *)(lf + 0);
            fl.l_type = lt == 0 ? F_RDLCK : lt == 1 ? F_WRLCK : F_UNLCK;               // Linux RDLCK=0,WRLCK=1,UNLCK=2 -> macOS
            fl.l_whence = *(short *)(lf + 2); fl.l_start = *(int64_t *)(lf + 8);
            fl.l_len = *(int64_t *)(lf + 16); fl.l_pid = *(int32_t *)(lf + 24);
            int r = fcntl((int)a0, mc, &fl), e = errno;
            if (r >= 0 && lcmd == 5) {                                                 // F_GETLK writes the conflicting lock back
                *(short *)(lf + 0) = fl.l_type == F_RDLCK ? 0 : fl.l_type == F_WRLCK ? 1 : 2;
                *(short *)(lf + 2) = fl.l_whence; *(int64_t *)(lf + 8) = fl.l_start;
                *(int64_t *)(lf + 16) = fl.l_len; *(int32_t *)(lf + 24) = (int32_t)fl.l_pid;
            }
            c->x[0] = r < 0 ? (uint64_t)(-(int64_t)e) : (uint64_t)r; break;
        }
        int mcmd = lcmd;
        if (lcmd == 8) mcmd = F_SETOWN; else if (lcmd == 9) mcmd = F_GETOWN;           // owner cmds also swapped on macOS
        else if (lcmd == 1030) mcmd = F_DUPFD_CLOEXEC;
        else if (lcmd == 1024 || lcmd == 1025 || lcmd == 1026 || lcmd == 1033 || lcmd == 1034) { c->x[0] = 0; break; }  // lease/notify/seals: no-op
        int r = fcntl((int)a0, mcmd, a2);
        if (r >= 0 && (lcmd == 0 || lcmd == 1030) && r < 1024 && (int)a0 >= 0 && (int)a0 < 1024) strcpy(g_fdpath[r], g_fdpath[(int)a0]);  // F_DUPFD(_CLOEXEC)
        c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 29: {                                                                     // ioctl(fd, req, arg) -- Linux req# -> macOS
        int fd = (int)a0; unsigned long rq = (unsigned long)a1; void *arg = (void *)a2;
        switch (rq) {
        case 0x5401: { struct termios t; if (tcgetattr(fd, &t) < 0) { c->x[0] = (uint64_t)(-errno); break; }   // TCGETS
                       termios_m2l(&t, (uint8_t *)arg); c->x[0] = 0; break; }
        case 0x5402: case 0x5403: case 0x5404: { struct termios t; termios_l2m((const uint8_t *)arg, &t);       // TCSETS/W/F
                       int act = rq == 0x5402 ? TCSANOW : rq == 0x5403 ? TCSADRAIN : TCSAFLUSH;
                       c->x[0] = tcsetattr(fd, act, &t) < 0 ? (uint64_t)(-errno) : 0; break; }
        case 0x802c542a: { struct termios t; if (tcgetattr(fd, &t) < 0) { c->x[0] = (uint64_t)(-errno); break; } // TCGETS2 (glibc aarch64 uses this)
                       termios_m2l(&t, (uint8_t *)arg);
                       *(uint32_t *)((uint8_t *)arg + 36) = (uint32_t)cfgetispeed(&t); *(uint32_t *)((uint8_t *)arg + 40) = (uint32_t)cfgetospeed(&t);
                       c->x[0] = 0; break; }
        case 0x402c542b: case 0x402c542c: case 0x402c542d: { struct termios t; termios_l2m((const uint8_t *)arg, &t);  // TCSETS2/W2/F2
                       cfsetispeed(&t, *(uint32_t *)((const uint8_t *)arg + 36)); cfsetospeed(&t, *(uint32_t *)((const uint8_t *)arg + 40));
                       int act = rq == 0x402c542b ? TCSANOW : rq == 0x402c542c ? TCSADRAIN : TCSAFLUSH;
                       c->x[0] = tcsetattr(fd, act, &t) < 0 ? (uint64_t)(-errno) : 0; break; }
        case 0x5413: c->x[0] = ioctl(fd, TIOCGWINSZ, arg) < 0 ? (uint64_t)(-errno) : 0; break;                 // TIOCGWINSZ (struct same)
        case 0x5414: c->x[0] = ioctl(fd, TIOCSWINSZ, arg) < 0 ? (uint64_t)(-errno) : 0; break;                 // TIOCSWINSZ
        case 0x80045430: if (arg && fd >= 0 && fd < 1024) *(uint32_t *)arg = (uint32_t)fd; c->x[0] = 0; break;  // TIOCGPTN -> pts# = master fd
        case 0x40045431: c->x[0] = 0; break;                                                                   // TIOCSPTLCK (unlockpt done at open)
        case 0x5421: { int on = arg ? *(int *)arg : 0, fl = fcntl(fd, F_GETFL);                                // FIONBIO
                       fl = on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK); c->x[0] = fcntl(fd, F_SETFL, fl) < 0 ? (uint64_t)(-errno) : 0; break; }
        case 0x541b: c->x[0] = ioctl(fd, FIONREAD, arg) < 0 ? (uint64_t)(-errno) : 0; break;                   // FIONREAD
        case 0x5451: c->x[0] = fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 ? (uint64_t)(-errno) : 0; break;             // FIOCLEX
        case 0x5450: { int fl = fcntl(fd, F_GETFD); c->x[0] = fcntl(fd, F_SETFD, fl & ~FD_CLOEXEC) < 0 ? (uint64_t)(-errno) : 0; break; }  // FIONCLEX
        case 0x540f: if (arg) *(int *)arg = (int)getpgrp(); c->x[0] = 0; break;                                // TIOCGPGRP
        case 0x5410: c->x[0] = 0; break;                                                                       // TIOCSPGRP
        case 0x540e: c->x[0] = 0; break;                                                                       // TIOCSCTTY
        default: c->x[0] = (uint64_t)(-25); break;                                                            // ENOTTY
        } break; }
    case 33: {                                                                         // mknodat(dirfd, path, mode, dev)
        if (g_rootfs) { char fin[512]; int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, 1);
            if (pfd < 0) { c->x[0] = (uint64_t)(int64_t)pfd; break; }
            int r = mknodat(pfd, fin, (mode_t)a2, (dev_t)a3), e = errno;
            char dp[4200]; if (r >= 0 && fcntl(pfd, F_GETPATH, dp) == 0) { char hp[4400]; snprintf(hp, sizeof hp, "%s/%s", dp, fin); mc_evict(hp); ac_evict(hp); }
            close(pfd); c->x[0] = r < 0 ? (uint64_t)(-(int64_t)e) : 0; break; }
        char pb[4200]; const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        int r = mknodat(ATFD(a0), p, (mode_t)a2, (dev_t)a3); if (r >= 0) { mc_evict(p); ac_evict(p); }
        c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break; }
    case 34: {                                                   // mkdirat(dirfd, path, mode) -- confined
        if (g_rootfs) { char fin[512]; int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, 1);
            if (pfd < 0) { c->x[0] = (uint64_t)(int64_t)pfd; break; }
            int r = mkdirat(pfd, fin, (mode_t)a2), e = errno;
            char dp[4200]; if (r >= 0 && fcntl(pfd, F_GETPATH, dp) == 0) { char hp[4400]; snprintf(hp, sizeof hp, "%s/%s", dp, fin); mc_evict(hp); ac_evict(hp); }
            close(pfd); c->x[0] = r < 0 ? (uint64_t)(-(int64_t)e) : 0; break; }
        char pb[4200]; const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        int r = mkdirat(ATFD(a0), p, (mode_t)a2); mc_evict(p); ac_evict(p);   // namespace change -> evict
        c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break; }
    case 35: {                                                   // unlinkat(dirfd, path, flags) -- confined
        if (g_rootfs && g_nlower) {                              // OVERLAY: whiteout (hides lower) + drop the upper copy
            char gp[4200]; abs_guest((int)a0, (const char *)a1, gp, sizeof gp); char host[4300];
            if (!overlay_resolve(gp, host, sizeof host, 1)) { c->x[0] = (uint64_t)(-2); break; }   // ENOENT
            overlay_whiteout(gp); c->x[0] = 0; break;
        }
        if (g_rootfs) { char fin[512]; int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, 1);
            if (pfd < 0) { c->x[0] = (uint64_t)(int64_t)pfd; break; }
            int r = unlinkat(pfd, fin, (a2 & 0x200) ? AT_REMOVEDIR : 0), e = errno;   // AT_REMOVEDIR: linux 0x200
            char dp[4200]; if (r >= 0 && fcntl(pfd, F_GETPATH, dp) == 0) { char hp[4400]; snprintf(hp, sizeof hp, "%s/%s", dp, fin); mc_evict(hp); ac_evict(hp); rl_evict(hp); }
            close(pfd); c->x[0] = r < 0 ? (uint64_t)(-(int64_t)e) : 0; break; }
        char pb[4200]; const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        int r = unlinkat(ATFD(a0), p, (a2 & 0x200) ? AT_REMOVEDIR : 0); mc_evict(p); ac_evict(p); rl_evict(p);
        c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break; }
    case 36: {                                                                         // symlinkat(target, newdirfd, linkpath)
        const char *target = (const char *)a0;                                          // target is the link CONTENT (unresolved); follow-time confinement guards it
        if (g_rootfs) { char fin[512]; int pfd = jail_at((int)a1, (const char *)a2, fin, sizeof fin, 1);
            if (pfd < 0) { c->x[0] = (uint64_t)(int64_t)pfd; break; }
            int r = symlinkat(target, pfd, fin), e = errno;
            close(pfd); c->x[0] = r < 0 ? (uint64_t)(-(int64_t)e) : 0; break; }
        char pb[4200]; const char *p = atpath((int)a1, (const char *)a2, pb, sizeof pb);
        c->x[0] = symlinkat(target, ATFD(a1), p) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 37: {                                                                         // linkat(odir,opath,ndir,npath,flags)
        int fl = (a4 & 0x400) ? AT_SYMLINK_FOLLOW : 0;
        if (g_rootfs) { char ofin[512], nfin[512];                                      // both ends confined via TOCTOU-free resolver
            int opfd = jail_at((int)a0, (const char *)a1, ofin, sizeof ofin, 1);
            if (opfd < 0) { c->x[0] = (uint64_t)(int64_t)opfd; break; }
            int npfd = jail_at((int)a2, (const char *)a3, nfin, sizeof nfin, 1);
            if (npfd < 0) { close(opfd); c->x[0] = (uint64_t)(int64_t)npfd; break; }
            int r = linkat(opfd, ofin, npfd, nfin, fl), e = errno;
            close(opfd); close(npfd); c->x[0] = r < 0 ? (uint64_t)(-(int64_t)e) : 0; break; }
        char ob[4200], nb[4200];
        const char *op = atpath((int)a0, (const char *)a1, ob, sizeof ob);
        const char *np = atpath((int)a2, (const char *)a3, nb, sizeof nb);
        c->x[0] = linkat(ATFD(a0), op, ATFD(a2), np, fl) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 38: case 276: {                                                               // renameat / renameat2 (flags ignored)
        if (g_rootfs) { char ofin[512], nfin[512];                                      // both ends confined (TOCTOU-free)
            int opfd = jail_at((int)a0, (const char *)a1, ofin, sizeof ofin, 1);
            if (opfd < 0) { c->x[0] = (uint64_t)(int64_t)opfd; break; }
            int npfd = jail_at((int)a2, (const char *)a3, nfin, sizeof nfin, 1);
            if (npfd < 0) { close(opfd); c->x[0] = (uint64_t)(int64_t)npfd; break; }
            char dp[4200]; if (fcntl(opfd, F_GETPATH, dp) == 0) { char hp[4400]; snprintf(hp, sizeof hp, "%s/%s", dp, ofin); mc_evict(hp); ac_evict(hp); }
            int r = renameat(opfd, ofin, npfd, nfin), e = errno;
            close(opfd); close(npfd); c->x[0] = r < 0 ? (uint64_t)(-(int64_t)e) : 0; break; }
        char ob[4200], nb[4200];
        const char *op = atpath((int)a0, (const char *)a1, ob, sizeof ob);
        const char *np = atpath((int)a2, (const char *)a3, nb, sizeof nb);
        c->x[0] = renameat(ATFD(a0), op, ATFD(a2), np) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 40: case 39: case 41: c->x[0] = 0; break;                                     // mount / umount2 / pivot_root -> ok
    case 43: case 44: { uint8_t *b = (uint8_t *)(nr == 43 ? a1 : a1); memset(b, 0, 120); // statfs/fstatfs
        *(uint64_t *)(b + 0) = 0x01021994; *(uint64_t *)(b + 8) = 4096;
        *(uint64_t *)(b + 16) = 1u << 24; *(uint64_t *)(b + 24) = 1u << 23; c->x[0] = 0; break; }
    case 46: { int r = ftruncate((int)a0, (off_t)a1); fd_evict((int)a0); c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break; }  // ftruncate
    case 47: { struct stat s; off_t end = (off_t)(a2 + a3);                            // fallocate(fd,mode,offset,len): extend (no shrink)
               if (fstat((int)a0, &s) == 0 && s.st_size < end && ftruncate((int)a0, end) < 0) {}
               fd_evict((int)a0); c->x[0] = 0; break; }
    case 49: { char pb[4200]; const char *p = atpath(-100, (const char *)a0, pb, sizeof pb);  // chdir (confined; tracks guest cwd)
               if (chdir(p) < 0) { c->x[0] = (uint64_t)(-errno); break; }
               if (g_rootfs && !strncmp(p, g_rootfs_canon, g_rootfs_canon_len)) { const char *g = p + g_rootfs_canon_len; snprintf(g_cwd, sizeof g_cwd, "%s", g[0] ? g : "/"); }
               c->x[0] = 0; break; }
    case 50: { if (fchdir((int)a0) < 0) { c->x[0] = (uint64_t)(-errno); break; }       // fchdir (tracks guest cwd)
               if ((int)a0 >= 0 && (int)a0 < 1024 && g_fdpath[(int)a0][0]) { const char *g = g_fdpath[(int)a0];
                   if (g_rootfs && !strncmp(g, g_rootfs_canon, g_rootfs_canon_len)) g += g_rootfs_canon_len; snprintf(g_cwd, sizeof g_cwd, "%s", g[0] ? g : "/"); }
               c->x[0] = 0; break; }
    case 52: c->x[0] = fchmod((int)a0, (mode_t)a1) < 0 ? (uint64_t)(-errno) : 0; break;   // fchmod(fd, mode)
    case 53: case 452: {                                                               // fchmodat(dirfd,path,mode,flags) / fchmodat2
        if (g_rootfs) { char fin[512]; int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, 0);
            if (pfd < 0) { c->x[0] = (uint64_t)(int64_t)pfd; break; }
            int r = fchmodat(pfd, fin, (mode_t)a2, 0), e = errno; char dp[4200]; if (r >= 0 && fcntl(pfd, F_GETPATH, dp) == 0) { char hp[4400]; snprintf(hp, sizeof hp, "%s/%s", dp, fin); mc_evict(hp); }
            close(pfd); c->x[0] = r < 0 ? (uint64_t)(-(int64_t)e) : 0; break; }
        char pb[4200]; const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        int r = fchmodat(ATFD(a0), p, (mode_t)a2, 0); if (r >= 0) mc_evict(p);
        c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break; }
    case 54: {                                                                         // fchownat(dirfd,path,uid,gid,flags) -- best-effort (rootless)
        if (g_rootfs) { char fin[512]; int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, (a4 & 0x100) ? 1 : 0);
            if (pfd < 0) { c->x[0] = (uint64_t)(int64_t)pfd; break; }
            fchownat(pfd, fin, (uid_t)a2, (gid_t)a3, (a4 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0); close(pfd); c->x[0] = 0; break; }  // EPERM on the host -> faked OK
        char pb[4200]; const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        fchownat(ATFD(a0), p, (uid_t)a2, (gid_t)a3, 0); c->x[0] = 0; break; }
    case 55: { fchown((int)a0, (uid_t)a1, (gid_t)a2); c->x[0] = 0; break; }             // fchown(fd,uid,gid) -- best-effort
    case 56: { int lf = (int)a2, mf = lf & 0x3;          // openat -- Linux O_* -> macOS O_* (they differ!)
               { const char *rp = (const char *)a1;      // synthesize /proc/* (macOS has no /proc)
                 if (rp && !strncmp(rp, "/proc/", 6)) {
                     if (strstr(rp, "/auxv")) {            // /proc/[self|pid]/auxv (rustix/libc read it)
                         char tn[] = "/tmp/.ddauxvXXXXXX"; int afd = mkstemp(tn);
                         if (afd >= 0) { unlink(tn); if (write(afd, g_auxv_data, g_auxv_len) < 0) {} lseek(afd, 0, SEEK_SET); }
                         c->x[0] = afd < 0 ? (uint64_t)(-errno) : (uint64_t)afd; break;
                     }
                     int pf = proc_open(rp);               // cpuinfo/meminfo/stat/mounts/uptime/loadavg/version
                     if (pf != -2) { c->x[0] = pf < 0 ? (uint64_t)(-errno) : (uint64_t)pf; break; }
                 }
                 if (rp && !strncmp(rp, "/sys/fs/cgroup/", 15)) {  // cgroup v2 limit files (JVM/Go self-size on these)
                     int pf = proc_open(rp);
                     if (pf != -2) { c->x[0] = pf < 0 ? (uint64_t)(-errno) : (uint64_t)pf; break; }
                 }
                 if (rp && !strncmp(rp, "/dev/", 5)) {              // device nodes -> host devices (rootfs has no real /dev)
                     const char *hd = !strcmp(rp, "/dev/null") ? "/dev/null" : !strcmp(rp, "/dev/zero") ? "/dev/zero"
                         : !strcmp(rp, "/dev/full") ? "/dev/null" : !strcmp(rp, "/dev/random") ? "/dev/random"
                         : !strcmp(rp, "/dev/urandom") ? "/dev/urandom" : !strcmp(rp, "/dev/tty") ? "/dev/tty" : NULL;
                     if (hd) { int d = open(hd, mf); c->x[0] = d < 0 ? (uint64_t)(-errno) : (uint64_t)d; break; }
                 } }
               if (lf & 0x40) mf |= O_CREAT;   if (lf & 0x80) mf |= O_EXCL;
               if (lf & 0x200) mf |= O_TRUNC;  if (lf & 0x400) mf |= O_APPEND;
               if (lf & 0x800) mf |= O_NONBLOCK; if (lf & 0x10000) mf |= O_DIRECTORY;
               if (lf & 0x80000) mf |= O_CLOEXEC;
               { const char *rp = (const char *)a1;       // pty: /dev/ptmx -> posix_openpt; /dev/pts/N -> slave
                 if (rp && !strcmp(rp, "/dev/ptmx")) {
                     int m = posix_openpt(O_RDWR | O_NOCTTY); if (m >= 0) { grantpt(m); unlockpt(m); }
                     c->x[0] = m < 0 ? (uint64_t)(-errno) : (uint64_t)m; break;
                 }
                 if (rp && !strncmp(rp, "/dev/pts/", 9) && rp[9] >= '0' && rp[9] <= '9') {
                     char *sn = ptsname(atoi(rp + 9));
                     if (!sn) { c->x[0] = (uint64_t)(int64_t)(-2); break; }   // ENOENT
                     int s = open(sn, mf); c->x[0] = s < 0 ? (uint64_t)(-errno) : (uint64_t)s; break;
                 } }
               if (g_rootfs && g_nlower) {                // OVERLAY: resolve across layers (upper shadows lowers)
                   char gp[4200]; abs_guest((int)a0, (const char *)a1, gp, sizeof gp);
                   char host[4300]; int isw = (lf & 3) || (lf & 0x40);          // O_WRONLY/O_RDWR/O_CREAT -> write
                   if (isw) overlay_copyup(gp, host, sizeof host);              // copy-up the lower file (or upper path to create)
                   else overlay_resolve(gp, host, sizeof host, (lf & 0x20000) != 0);
                   int r = open(host, mf | ((lf & 0x20000) ? O_NOFOLLOW : 0), (mode_t)a3);
                   if (r >= 0) { char gpa[4200]; if (fcntl(r, F_GETPATH, gpa) == 0) { fd_setpath(r, gpa);
                                     if (isw) { mc_evict(gpa); rl_evict(gpa); ac_evict(gpa); } }
                                 if (r < 1024) snprintf(g_ovldir[r], sizeof g_ovldir[r], "%s", gp); }  // remember guest path for merged getdents
                   c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break;
               }
               if (g_rootfs) {                            // TOCTOU-free per-component resolve in the jail
                   char fin[512];
                   int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, (lf & 0x20000) != 0);  // O_NOFOLLOW
                   if (pfd < 0) { c->x[0] = (uint64_t)(int64_t)pfd; break; }
                   int r = openat(pfd, fin, mf | O_NOFOLLOW, (mode_t)a3);   // fin is resolved -> O_NOFOLLOW safe
                   int e = errno; close(pfd);
                   if (r >= 0) { char gp[4200]; if (fcntl(r, F_GETPATH, gp) == 0) {  // canonical host path for tracking
                                     fd_setpath(r, gp);
                                     if ((lf & 3) || (lf & 0x40) || (lf & 0x200)) { mc_evict(gp); rl_evict(gp); ac_evict(gp); } } }
                   c->x[0] = r < 0 ? (uint64_t)(-(int64_t)e) : (uint64_t)r; break;
               }
               char pb[4200]; const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);  // no jail
               int r = openat(ATFD(a0), p, mf, (mode_t)a3);
               if (r >= 0) { fd_setpath(r, p);
                             if ((lf & 3) || (lf & 0x40) || (lf & 0x200)) { mc_evict(p); rl_evict(p); ac_evict(p); } }
               c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 57: { int cf = (int)a0; if (cf >= 0 && cf < 1024) { if (g_eventfd_peer[cf]) { close(g_eventfd_peer[cf] - 1); g_eventfd_peer[cf] = 0; } g_timerfd[cf] = 0; g_ovldir[cf][0] = 0; g_lo_port[cf] = 0; g_sock_stream[cf] = 0; }  // reap eventfd peer / timerfd / overlay dir / loopback
               int r = close(cf); fd_clear(cf); c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break; }  // close: -errno on fail
    case 59: { int fds[2]; if (pipe(fds) < 0) { c->x[0] = (uint64_t)(-errno); break; }  // pipe2(fds, flags)
        int fl = (int)a1;
        if (fl & 0x80000) { fcntl(fds[0], F_SETFD, FD_CLOEXEC); fcntl(fds[1], F_SETFD, FD_CLOEXEC); }
        if (fl & 0x800)   { fcntl(fds[0], F_SETFL, O_NONBLOCK); fcntl(fds[1], F_SETFL, O_NONBLOCK); }
        ((int *)a0)[0] = fds[0]; ((int *)a0)[1] = fds[1]; c->x[0] = 0; break; }
    case 61: {                                                                         // getdents64
        int fd = (int)a0;
        if (g_nlower && fd >= 0 && fd < 1024 && g_ovldir[fd][0]) {                      // OVERLAY: merged listing across layers
            static struct { int fd; int n, pos; char nm[1024][256]; uint8_t ty[1024]; } oc[16];
            int slot = -1; for (int i = 0; i < 16; i++) if (oc[i].fd == fd + 1) { slot = i; break; }
            if (slot < 0) { for (int i = 0; i < 16; i++) if (oc[i].fd == 0) { slot = i; break; } if (slot < 0) slot = 0;
                oc[slot].fd = fd + 1; oc[slot].pos = 0;
                oc[slot].n = overlay_readdir(g_ovldir[fd], oc[slot].nm, oc[slot].ty, 1024); }
            uint8_t *out = (uint8_t *)a1; size_t o = 0;
            while (oc[slot].pos < oc[slot].n) {
                const char *nm = oc[slot].nm[oc[slot].pos]; size_t nl = strlen(nm), lr = (19 + nl + 1 + 7) & ~7ull;
                if (o + lr > (size_t)a2) break;
                uint8_t *ld = out + o;
                *(uint64_t *)(ld + 0) = oc[slot].pos + 1; *(uint64_t *)(ld + 8) = o + lr;
                *(uint16_t *)(ld + 16) = (uint16_t)lr; *(ld + 18) = oc[slot].ty[oc[slot].pos];
                memcpy(ld + 19, nm, nl); ld[19 + nl] = 0;
                o += lr; oc[slot].pos++;
            }
            if (o == 0) oc[slot].fd = 0;                                                // exhausted -> free the slot
            c->x[0] = (uint64_t)o; break;
        }
        static struct { int fd; DIR *d; } dirs[64]; static int ndirs;
        DIR *dir = NULL;
        for (int i = 0; i < ndirs; i++) if (dirs[i].fd == fd) { dir = dirs[i].d; break; }
        if (!dir) { dir = fdopendir(dup(fd)); if (!dir) { c->x[0] = (uint64_t)(-errno); break; }
                    if (ndirs < 64) { dirs[ndirs].fd = fd; dirs[ndirs].d = dir; ndirs++; } }
        uint8_t *out = (uint8_t *)a1; size_t o = 0; struct dirent *de; long pos = telldir(dir);
        while ((de = readdir(dir))) {
            size_t nl = strlen(de->d_name), lr = (19 + nl + 1 + 7) & ~7ull;
            if (o + lr > (size_t)a2) { seekdir(dir, pos); break; }
            uint8_t *ld = out + o;
            *(uint64_t *)(ld + 0) = de->d_ino; *(uint64_t *)(ld + 8) = o + lr;
            *(uint16_t *)(ld + 16) = (uint16_t)lr; *(ld + 18) = de->d_type;
            memcpy(ld + 19, de->d_name, nl); ld[19 + nl] = 0;
            o += lr; pos = telldir(dir);
        }
        c->x[0] = o; break;
    }
    case 78: {                                                                    // readlinkat
        const char *p = (const char *)a1; char *buf = (char *)a2; size_t bs = (size_t)a3;
        if (p && strstr(p, "/proc/self/exe")) {
            char rp[1024]; if (!realpath(g_exe_path, rp)) strncpy(rp, g_exe_path, sizeof rp - 1);
            size_t l = strlen(rp); if (l > bs) l = bs; memcpy(buf, rp, l); c->x[0] = l;
        } else { char pb[4200]; const char *rp = xlate(p, pb, sizeof pb);
                 int rc, len;
                 if (rl_lookup(rp, &rc, buf, bs, &len)) { c->x[0] = rc < 0 ? (uint64_t)(int64_t)rc : (uint64_t)len; break; }
                 ssize_t r = readlink(rp, buf, bs);
                 rl_store(rp, r < 0 ? -errno : (int)r, buf, r < 0 ? 0 : (int)r);
                 c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; }
        break;
    }
    case 79: { struct stat s; char pb[4200];                                      // newfstatat(dfd, path, buf, flags)
        const char *raw = (const char *)a1, *p = atpath((int)a0, raw, pb, sizeof pb);
        { const char *gp = (g_rootfs && !strncmp(p, g_rootfs_canon, g_rootfs_canon_len)) ? p + g_rootfs_canon_len : p;
          if (synth_stat(gp, (uint8_t *)a2)) { c->x[0] = 0; break; } }              // synthesized /proc or /sys file
        if (raw && raw[0] && !(a3 & 0x100)) {                                      // cacheable: named path, follow
            int rc;
            if (!mc_lookup(p, &rc, &s)) { int r = fstatat(ATFD(a0), p, &s, 0); rc = r < 0 ? -errno : 0; mc_store(p, rc, &s); }
            if (rc == 0) fill_linux_stat((uint8_t *)a2, &s);
            c->x[0] = (uint64_t)(int64_t)rc; break;
        }
        int r = (raw && !raw[0] && (a3 & 0x1000)) ? fstat((int)a0, &s)             // AT_EMPTY_PATH -> fstat(dfd)
                : fstatat(ATFD(a0), p, &s, AT_SYMLINK_NOFOLLOW);
        if (r < 0) { c->x[0] = (uint64_t)(-errno); break; }
        fill_linux_stat((uint8_t *)a2, &s); c->x[0] = 0; break; }
    case 80: { struct stat s;                                                     // fstat(fd, buf)
        if (fstat((int)a0, &s) < 0) { c->x[0] = (uint64_t)(-errno); break; }
        fill_linux_stat((uint8_t *)a1, &s); c->x[0] = 0; break; }
    case 81: sync(); c->x[0] = 0; break;                                               // sync
    case 82: c->x[0] = fsync((int)a0) < 0 ? (uint64_t)(-errno) : 0; break;             // fsync
    case 83: c->x[0] = fsync((int)a0) < 0 ? (uint64_t)(-errno) : 0; break;             // fdatasync -> fsync (no macOS fdatasync)
    case 88: {                                                                         // utimensat(dirfd, path, times, flags)
        struct timespec *ts = (struct timespec *)a2;
        if (!a1) { c->x[0] = futimens((int)a0, ts) < 0 ? (uint64_t)(-errno) : 0; break; }  // path NULL -> futimens(fd)
        if (g_rootfs) { char fin[512]; int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, (a3 & 0x100) ? 1 : 0);
            if (pfd < 0) { c->x[0] = (uint64_t)(int64_t)pfd; break; }
            int r = utimensat(pfd, fin, ts, (a3 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0), e = errno;
            char dp[4200]; if (r >= 0 && fcntl(pfd, F_GETPATH, dp) == 0) { char hp[4400]; snprintf(hp, sizeof hp, "%s/%s", dp, fin); mc_evict(hp); }  // mtime changed
            close(pfd); c->x[0] = r < 0 ? (uint64_t)(-(int64_t)e) : 0; break; }
        char pb[4200]; const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        int r = utimensat(ATFD(a0), p, ts, (a3 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0); if (r >= 0) mc_evict(p);
        c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break; }
    case 166: c->x[0] = (uint64_t)umask((mode_t)a0); break;                            // umask -> old mask
    case 223: c->x[0] = 0; break;                                                      // fadvise64 -- advisory no-op
    case 285: {                                                                        // copy_file_range(fdin,offin*,fdout,offout*,len,flags)
        int fdin = (int)a0, fdout = (int)a2; size_t len = (size_t)a4, done = 0; int err = 0;
        off_t *poi = (off_t *)a1, *poo = (off_t *)a3; off_t oi = poi ? *poi : -1, oo = poo ? *poo : -1;
        char cb[8192];
        while (done < len) {
            size_t chunk = (len - done > sizeof cb) ? sizeof cb : len - done;
            ssize_t r = (oi >= 0) ? pread(fdin, cb, chunk, oi) : read(fdin, cb, chunk);
            if (r < 0) { err = errno; break; } if (r == 0) break;
            ssize_t w = (oo >= 0) ? pwrite(fdout, cb, (size_t)r, oo) : write(fdout, cb, (size_t)r);
            if (w < 0) { err = errno; break; }
            done += (size_t)w; if (oi >= 0) oi += w; if (oo >= 0) oo += w; if (w < r) break;
        }
        if (poi) *poi = oi; if (poo) *poo = oo; fd_evict(fdout);
        c->x[0] = (done == 0 && err) ? (uint64_t)(-(int64_t)err) : (uint64_t)done; break; }
    case 291: { struct stat s; char pb[4200];                                     // statx(dfd, path, flags, mask, buf)
        const char *raw = (const char *)a1, *p = atpath((int)a0, raw, pb, sizeof pb);
        int rc, empty = (raw && !raw[0] && (a2 & 0x1000));
        const char *gp = (g_rootfs && !strncmp(p, g_rootfs_canon, g_rootfs_canon_len)) ? p + g_rootfs_canon_len : p;
        if (synth_stat_raw(gp, &s)) { rc = 0; }                                   // synth /proc or /sys -> fill from s below
        else if (raw && raw[0] && !empty) {                                       // cacheable
            if (!mc_lookup(p, &rc, &s)) { int rr = fstatat(ATFD(a0), p, &s, 0); rc = rr < 0 ? -errno : 0; mc_store(p, rc, &s); }
        } else { int rr = empty ? fstat((int)a0, &s) : fstatat(ATFD(a0), p, &s, 0); rc = rr < 0 ? -errno : 0; }
        if (rc < 0) { c->x[0] = (uint64_t)(int64_t)rc; break; }
        uint8_t *d = (uint8_t *)a4; memset(d, 0, 256);                             // struct statx (correct offsets)
        *(uint32_t *)(d + 0) = 0x17ff; *(uint32_t *)(d + 4) = 4096;                // stx_mask (BTIME|basic), stx_blksize
        *(uint32_t *)(d + 16) = s.st_nlink ? s.st_nlink : 1;                       // stx_nlink @16
        *(uint32_t *)(d + 20) = s.st_uid; *(uint32_t *)(d + 24) = s.st_gid;        // stx_uid@20 stx_gid@24
        *(uint16_t *)(d + 28) = (uint16_t)s.st_mode;                              // stx_mode @28  <-- was @36 (the bug)
        *(uint64_t *)(d + 32) = s.st_ino;                                         // stx_ino @32
        *(uint64_t *)(d + 40) = (uint64_t)s.st_size;                              // stx_size @40
        *(uint64_t *)(d + 48) = (uint64_t)s.st_blocks;                            // stx_blocks @48
        *(int64_t *)(d + 64) = s.st_atime; *(int64_t *)(d + 96) = s.st_ctime;     // stx_atime@64 stx_ctime@96
        *(int64_t *)(d + 112) = s.st_mtime;                                       // stx_mtime @112 (sec)
        c->x[0] = 0; break; }
    case 437: {                                          // openat2(dirfd, path, open_how*, size) -- glibc uses it; MUST confine
        uint64_t *how = (uint64_t *)a2;                  //   open_how { u64 flags; u64 mode; u64 resolve; }
        a2 = how ? how[0] : 0; a3 = how ? how[1] : 0;    // -> openat(dirfd, path, flags, mode); resolve flags ignored (jail confines)
    }   /* fall through to openat */
    case 439:  // faccessat2(dirfd,path,mode,flags) -- glibc access() uses it; same path/confinement, flags ignored
    case 48: { char pb[4200]; const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb); // faccessat
               if (a2 == 0 && p) {                                                     // F_OK existence check: cacheable
                   int rc;
                   if (!ac_lookup(p, &rc)) { int r = faccessat(ATFD(a0), p, 0, 0); rc = r < 0 ? -errno : 0; ac_store(p, rc); }
                   c->x[0] = (uint64_t)(int64_t)rc; break;
               }
               int r = faccessat(ATFD(a0), p, (int)a2, 0);
               c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break; }

    // ===================== Memory — mmap/brk/mprotect/madvise (anon charged against cgroup memory.max) =====================
    case 214: {                                                                   // brk
        if (a0 == 0) { c->x[0] = brk_cur; break; }
        if (a0 >= brk_lo && a0 <= brk_hi) {
            if (g_mem_max && a0 > brk_cur) {                                       // heap growth -> charge cgroup memory.max
                uint64_t delta = a0 - brk_cur;
                if (atomic_fetch_add(&g_mem_charged, delta) + delta > g_mem_max) {
                    atomic_fetch_sub(&g_mem_charged, delta); c->x[0] = brk_cur; break;   // over limit -> break unchanged (ENOMEM)
                }
            } else if (g_mem_max && a0 < brk_cur) {                               // shrink -> uncharge
                uint64_t delta = brk_cur - a0, cur = atomic_load(&g_mem_charged);
                atomic_fetch_sub(&g_mem_charged, delta > cur ? cur : delta);
            }
            brk_cur = a0;
        }
        c->x[0] = brk_cur; break;
    }
    case 215: { int r = munmap((void *)a0, (size_t)a1);                            // munmap
                if (r == 0 && g_mem_max) { uint64_t cur = atomic_load(&g_mem_charged), d = (uint64_t)a1;  // uncharge (clamp >=0)
                    atomic_fetch_sub(&g_mem_charged, d > cur ? cur : d); }
                c->x[0] = (uint64_t)r; break; }
    case 216: { void *r = mmap(0, (size_t)a2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0); // mremap (copy+grow)
                if (r == MAP_FAILED) { c->x[0] = (uint64_t)(-errno); break; }
                size_t old = (size_t)a1, n = old < (size_t)a2 ? old : (size_t)a2;
                memcpy(r, (void *)a0, n); c->x[0] = (uint64_t)r; break; }
    case 222: {                                                                   // mmap
        int charge = g_mem_max && (a3 & 0x20) && !(a3 & 0x4000);                  // charge anon, but NOT MAP_NORESERVE
        if (charge) {                                                            //   (libc reserves huge virtual arenas it never commits;
            if (atomic_fetch_add(&g_mem_charged, (uint64_t)a1) + (uint64_t)a1 > g_mem_max) {  // real memory.max counts RSS, not reservations)
                atomic_fetch_sub(&g_mem_charged, (uint64_t)a1); c->x[0] = (uint64_t)(-ENOMEM); break; }
        }
        void *r = mmap((void *)a0, (size_t)a1, (int)a2, mmap_flags((int)a3),
                       (a3 & 0x20) ? -1 : (int)a4, (off_t)a5);
        if (r == MAP_FAILED && charge) atomic_fetch_sub(&g_mem_charged, (uint64_t)a1);  // refund
        c->x[0] = (r == MAP_FAILED) ? (uint64_t)(-errno) : (uint64_t)r; break;
    }
    case 226: c->x[0] = (uint64_t)mprotect((void *)a0, (size_t)a1, (int)a2); break;// mprotect
    case 228: case 229: c->x[0] = 0; break;                                            // mlock/munlock (no-op)
    // Container-init compat: in the single-process model these are no-ops that return success so
    // entrypoints (mount /proc, unshare, drop caps, set hostname) proceed; the path-jail is the
    // real boundary, and a faked namespace grants no actual privilege (program still runs as our uid).
    case 232: c->x[0] = (uint64_t)(-ENOSYS); break;                                    // mincore -> unsupported (callers fall back)
    case 233: c->x[0] = 0; break;                                                      // madvise

    // ===================== Process & scheduling — clone/exec/wait/ids/prctl/futex/caps/sched =====================
    case 90: { if (a1) memset((void *)a1, 0xff, 12); c->x[0] = 0; break; }             // capget -> all caps present
    case 91: c->x[0] = 0; break;                                                       // capset -> ok
    case 93: c->exited = 1; c->exit_code = (int)a0; break;        // exit: end THIS thread
    case 94:                                                      // exit_group: end the whole process
        if (getenv("PROF")) fprintf(stderr, "[prof] crossings=%llu syscalls=%llu ibtc_miss=%llu branch_cross=%llu translations=%llu lse=%llu\n",
            (unsigned long long)g_prof_cross, (unsigned long long)g_prof_sys, (unsigned long long)g_prof_miss,
            (unsigned long long)(g_prof_cross - g_prof_sys - g_prof_miss), (unsigned long long)g_prof_xlate, (unsigned long long)g_lse_n);
        _exit((int)a0);
    case 96:  c->x[0] = (uint64_t)getpid(); break;   // set_tid_address -> returns caller's TID (musl stores it; 0 -> a_crash())
    case 97: case 268: c->x[0] = 0; break;                                             // unshare / setns -> ok (no real ns)
    case 98:  c->x[0] = (uint64_t)futex_op((int *)a0, (int)a1 & 0x7f, (int)a2, (struct timespec *)a3); break;  // futex
    case 99:  c->x[0] = 0; break;   // set_robust_list
    case 116: c->x[0] = 0; break;                                // syslog
    case 122: c->x[0] = 0; break;                                                  // sched_setaffinity
    case 123: { size_t n = (size_t)a1; if (n > 128) n = 128;                           // sched_getaffinity(pid,size,MASK=a2!)
                if (a2 && n) { memset((void *)a2, 0, n); *(uint8_t *)a2 = 1; }          // cpu 0 set; mask is a2 not a1
                c->x[0] = n < 8 ? (uint64_t)n : 8; break; }
    case 124: c->x[0] = 0; break;                                                      // sched_yield
    case 140: setpriority((int)a0, (int)a1, (int)a2); c->x[0] = 0; break;              // setpriority (best-effort)
    case 141: { errno = 0; int r = getpriority((int)a0, (int)a1);                      // getpriority -> Linux raw (20-nice)
                c->x[0] = (r == -1 && errno) ? (uint64_t)(-errno) : (uint64_t)(20 - r); break; }
    case 144: case 146: case 147: case 149: c->x[0] = 0; break;  // setgid/setfsuid/setresuid/setresgid -> ok
    case 145: c->x[0] = (uint64_t)getpgrp(); break;              // getpgid
    case 148: { if (a0) *(uint32_t *)a0 = cuid();                                  // getresuid(r,e,s)
                if (a1) *(uint32_t *)a1 = cuid(); if (a2) *(uint32_t *)a2 = cuid(); c->x[0] = 0; break; }
    case 150: { if (a0) *(uint32_t *)a0 = cgid();                                  // getresgid(r,e,s)
                if (a1) *(uint32_t *)a1 = cgid(); if (a2) *(uint32_t *)a2 = cgid(); c->x[0] = 0; break; }
    case 154: c->x[0] = setpgid((pid_t)a0, (pid_t)a1) < 0 ? (uint64_t)(-errno) : 0; break; // setpgid
    case 155: c->x[0] = (uint64_t)getpgid((pid_t)a0); break;                       // getpgid (bash job control)
    case 156: c->x[0] = (uint64_t)getsid((pid_t)a0); break;                        // getsid
    case 158: { if (g_gid >= 0) { if ((int)a0 >= 1 && a1) *(gid_t *)a1 = (gid_t)cgid(); c->x[0] = 1; break; }  // getgroups -> [container gid]
                int r = getgroups((int)a0, (gid_t *)a1); c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 159: c->x[0] = 0; break;                                                      // setgroups (privileged; ignore)
    case 165: {                                                                        // getrusage(who, *usage) -- a1 is the buffer, not a0!
        struct rusage ru; int who = ((int)a0 == -1) ? RUSAGE_CHILDREN : RUSAGE_SELF;    // Linux RUSAGE_THREAD(1) -> SELF
        if (a1) { uint8_t *d = (uint8_t *)a1; memset(d, 0, 144);                        // Linux struct rusage layout (18 longs)
            if (getrusage(who, &ru) == 0) {
                *(int64_t *)(d + 0) = ru.ru_utime.tv_sec;  *(int64_t *)(d + 8)  = ru.ru_utime.tv_usec;
                *(int64_t *)(d + 16) = ru.ru_stime.tv_sec; *(int64_t *)(d + 24) = ru.ru_stime.tv_usec;
                *(int64_t *)(d + 32) = ru.ru_maxrss / 1024;  // macOS bytes -> Linux KB
                *(int64_t *)(d + 64) = ru.ru_minflt; *(int64_t *)(d + 72) = ru.ru_majflt;
                *(int64_t *)(d + 88) = ru.ru_inblock; *(int64_t *)(d + 96) = ru.ru_oublock;
                *(int64_t *)(d + 120) = ru.ru_nsignals; *(int64_t *)(d + 128) = ru.ru_nvcsw; *(int64_t *)(d + 136) = ru.ru_nivcsw;
            } }
        c->x[0] = 0; break; }
    case 167: {                                                                        // prctl(option,...)
        switch ((int)a0) {                                                             // 0 for known no-ops; EINVAL for unknown (kernel does)
            case 1: case 3: case 4: case 8: case 15: case 35: case 36: case 38:
            case 53: case 55: case 59: c->x[0] = 0; break;  // PDEATHSIG/DUMPABLE/NAME/SECCOMP/TIMERSLACK/THP/SPECCTRL...
            default: c->x[0] = (uint64_t)(-22); break;       // EINVAL -- so feature probes (e.g. magic "AUXV") fail as on Linux
        }
        break; }
    case 172: c->x[0] = (uint64_t)container_pid(); break;                              // getpid (PID ns: init -> 1)
    case 173: c->x[0] = (container_pid() == 1) ? 0 : (uint64_t)getppid(); break;       // getppid (init's parent is 0 in the ns)
    case 174: case 175: c->x[0] = (uint64_t)cuid(); break;   // getuid/geteuid -> container uid (0=root by default)
    case 176: case 177: c->x[0] = (uint64_t)cgid(); break;   // getgid/getegid
    case 178: c->x[0] = (uint64_t)container_pid(); break;  // gettid
    case 220: {                                                                   // clone(flags,stack,ptid,tls,ctid)
        if (a0 & 0x10000) {                                       // CLONE_THREAD: stack arg IS the top
            c->x[0] = (uint64_t)spawn_thread(c, a0, a1, a3, a2, a4); break;
        }
        pid_t pid = fork();                                       // fork/vfork: COW copy; child continues
        if (pid == 0) c->ssp = 0;                                 // §B: child's pre-fork host_rets crossed run_block -> drop, use IBTC
        c->x[0] = pid < 0 ? (uint64_t)(-errno) : (uint64_t)pid;   // parent: pid, child: 0
        break;
    }
    case 221: {                                                                   // execve(path, argv, envp)
        char pb[4200]; const char *p = xresolve_exec((const char *)a0, pb, sizeof pb);  // follow symlink rootfs-relative (busybox applets)
        if (access(p, F_OK) != 0) { c->x[0] = (uint64_t)(-2); break; }            // ENOENT
        char *argv[256]; int ac = 0; uint64_t *gv = (uint64_t *)a1;
        while (gv && gv[ac] && ac < 255) { argv[ac] = (char *)gv[ac]; ac++; }
        argv[ac] = NULL;
        char sh_interp[256], sh_arg[256], shpb[4200];                             // shebang: exec the #! interpreter instead
        { int sfd = open(p, O_RDONLY); char hdr[258]; ssize_t k = sfd >= 0 ? read(sfd, hdr, sizeof hdr - 1) : -1;
          if (sfd >= 0) close(sfd);
          if (k > 3 && hdr[0] == '#' && hdr[1] == '!') {
              hdr[k] = 0; char *nl = strchr(hdr, '\n'); if (nl) *nl = 0;
              char *s = hdr + 2; while (*s == ' ' || *s == '\t') s++;             // interpreter path
              char *e = s; while (*e && *e != ' ' && *e != '\t') e++;
              char *arg = NULL; if (*e) { *e = 0; arg = e + 1; while (*arg == ' ' || *arg == '\t') arg++; if (!*arg) arg = NULL; }
              snprintf(sh_interp, sizeof sh_interp, "%s", s);
              char *na[258]; int ni = 0; na[ni++] = sh_interp;                    // [interp, (optarg), scriptpath, args...]
              if (arg) { snprintf(sh_arg, sizeof sh_arg, "%s", arg); na[ni++] = sh_arg; }
              na[ni++] = (char *)a0;                                              // the guest script path (interp re-opens it)
              for (int i = 1; i < ac && ni < 256; i++) na[ni++] = argv[i];
              na[ni] = NULL;
              p = xresolve_exec(sh_interp, shpb, sizeof shpb);                    // load the interpreter, not the script
              if (access(p, F_OK) != 0) { c->x[0] = (uint64_t)(-2); break; }
              for (int i = 0; i <= ni; i++) argv[i] = na[i]; ac = ni;
          } }
        struct loaded lm; load_elf(p, &lm);
        uint64_t jump = lm.entry, at_base = 0; char interp[256];
        if (elf_interp(p, interp, sizeof interp) == 0) {
            char ib[4200]; const char *ih = xresolve_exec(interp, ib, sizeof ib);   // follow+confine ld.so symlink
            struct loaded li; load_elf(ih, &li); jump = li.entry; at_base = li.base;
        }
        g_cp = g_cache; memset(g_map, 0, sizeof g_map); g_npend = 0;              // flush old translations
        memset(g_ibtc, 0, sizeof g_ibtc); c->ssp = 0;                            // execve: drop IBTC + §B shadow (old image)
        uint8_t *heap = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        brk_lo = brk_cur = (uint64_t)heap; brk_hi = brk_lo + (256u << 20);
        uint64_t sp = build_stack(ac, argv, &lm, at_base);
        memset(c->x, 0, sizeof c->x); c->nzcv = 0; c->tls = 0;
        c->sp = sp; c->pc = jump; c->redirect = 1;                                // jump to new program; don't advance pc
        break;
    }
    case 260: {                                                                   // wait4(pid, *status, opts, *rusage)
        int st = 0; pid_t r = wait4((pid_t)(int)a0, &st, (int)a2, (struct rusage *)a3);
        if (r < 0) { c->x[0] = (uint64_t)(-errno); break; }
        if ((st & 0x7f) != 0 && (st & 0x7f) != 0x7f)                              // WIFSIGNALED: macOS termsig -> Linux
            st = (st & ~0x7f) | (sig_m2l(st & 0x7f) & 0x7f);
        else if ((st & 0xff) == 0x7f)                                             // WIFSTOPPED: macOS stopsig -> Linux
            st = (st & ~0xff00) | ((sig_m2l((st >> 8) & 0xff) & 0xff) << 8);
        if (a1) *(int *)a1 = st; c->x[0] = (uint64_t)r; break;
    }
    case 261: { if (a3) { uint64_t *o = (uint64_t *)a3;                            // prlimit64(pid,res,new,OLD): old=a3!
                  o[0] = ((int)a1 == 3) ? (8ull << 20) : ~0ull;                    // RLIMIT_STACK=8MB, else unlimited
                  o[1] = ~0ull; }
                c->x[0] = 0; break; }
    case 435: {                                                                   // clone3(clone_args*, size)
        uint64_t *ca = (uint64_t *)a0; uint64_t flags = ca[0];
        if (flags & 0x10000) {                                    // CLONE_THREAD: sp = stack + stack_size
            c->x[0] = (uint64_t)spawn_thread(c, flags, ca[5] + ca[6], ca[7], ca[3], ca[2]); break;
        }
        pid_t pid = fork();
        if (pid == 0) c->ssp = 0;                                 // §B: same -- child drops the inherited shadow
        c->x[0] = pid < 0 ? (uint64_t)(-errno) : (uint64_t)pid;
        break;
    }

    // ===================== Signals — Linux signal numbers -> macOS; kill/sigaction/sigreturn =====================
    case 129:                                                            // kill(pid,sig)
        if ((int)a0 == container_pid() || (int)a0 <= 0) { raise_guest_signal(c, (int)a1); c->x[0] = 0; }  // self / pgrp (PID-ns aware)
        else c->x[0] = kill((pid_t)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0;
        break;
    case 130: raise_guest_signal(c, (int)a1); c->x[0] = 0; break;        // tkill(tid,sig)
    case 131: raise_guest_signal(c, (int)a2); c->x[0] = 0; break;        // tgkill(tgid,tid,sig)
    case 132: {                                                  // sigaltstack(new, old)
        if (a1) { *(uint64_t *)(a1 + 0) = c->alt_sp;             // report current (or SS_DISABLE=2 if none)
                  *(uint32_t *)(a1 + 8) = c->alt_sp ? c->alt_flags : 2;
                  *(uint64_t *)(a1 + 16) = c->alt_size; }
        if (a0) { c->alt_sp = *(uint64_t *)(a0 + 0); c->alt_flags = *(uint32_t *)(a0 + 8); c->alt_size = *(uint64_t *)(a0 + 16); }
        c->x[0] = 0; break;
    }
    case 134: {                                                  // rt_sigaction(sig, *act, *old)
        int sig = (int)a0;
        if (sig < 1 || sig > 64) { c->x[0] = (uint64_t)(-22); break; }
        if (a2) { *(uint64_t *)(a2 + 0) = g_sigact[sig].handler;
                  *(uint64_t *)(a2 + 8) = g_sigact[sig].flags;
                  *(uint64_t *)(a2 + 16) = g_sigact[sig].mask; }    // aarch64: handler,flags,mask
        if (a1) {
            uint64_t h = *(uint64_t *)(a1 + 0);
            g_sigact[sig].handler = h;
            g_sigact[sig].flags   = *(uint64_t *)(a1 + 8);
            g_sigact[sig].mask    = *(uint64_t *)(a1 + 16);
            if (sig != 9 && sig != 19) {                          // can't touch SIGKILL/SIGSTOP (Linux nums)
                int ms = sig_l2m(sig);                            // host(macOS) signo to install on
                if (h == 0) signal(ms, SIG_DFL);
                else if (h == 1) signal(ms, SIG_IGN);            // honor SIG_IGN (e.g. SIGPIPE)
                else if (!sig_is_sync(sig)) {                     // async: flag pending, deliver in dispatcher
                    struct sigaction sa; memset(&sa, 0, sizeof sa);
                    sa.sa_handler = host_sigh; sigfillset(&sa.sa_mask);
                    sigaction(ms, &sa, NULL);
                }
            }
        }
        c->x[0] = 0; break;
    }
    case 135: {                                                  // rt_sigprocmask(how, *set, *old)
        if (a2) *(uint64_t *)a2 = c->sigmask;
        if (a1) { uint64_t set = *(uint64_t *)a1;
            if (a0 == 0) c->sigmask |= set;                      // SIG_BLOCK
            else if (a0 == 1) c->sigmask &= ~set;                // SIG_UNBLOCK
            else c->sigmask = set; }                             // SIG_SETMASK
        c->x[0] = 0; break;
    }
    case 136: {                                                  // rt_sigpending(set, sigsetsize)
        uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST), out = 0;
        for (int s = 1; s <= 64; s++) if (p & (1ull << s)) out |= (1ull << (s - 1));   // 1<<N -> sigset_t bit N-1
        if (a0) *(uint64_t *)a0 = out;
        c->x[0] = 0; break;
    }
    case 139: do_sigreturn(c); c->redirect = 1; break;           // rt_sigreturn (restorer path)

    // ===================== Time — clock_gettime/nanosleep/gettimeofday (Linux clock-id translation) =====================
    case 101: nanosleep((const struct timespec *)a0, (struct timespec *)a1); c->x[0] = 0; break;  // nanosleep
    case 113: { clockid_t mc;                                                      // clock_gettime -- Linux clockid -> macOS
                switch ((int)a0) { case 0: case 5: mc = CLOCK_REALTIME; break;     // REALTIME(_COARSE)
                    case 1: case 6: case 7: mc = CLOCK_MONOTONIC; break;           // MONOTONIC(_COARSE)/BOOTTIME
                    case 2: mc = CLOCK_PROCESS_CPUTIME_ID; break; case 3: mc = CLOCK_THREAD_CPUTIME_ID; break;
                    case 4: mc = CLOCK_MONOTONIC_RAW; break; default: mc = CLOCK_MONOTONIC; break; }
                struct timespec ts; clock_gettime(mc, &ts);
                uint64_t *g = (uint64_t *)a1; if (g) { g[0] = ts.tv_sec; g[1] = ts.tv_nsec; }
                c->x[0] = 0; break; }
    case 114: { if (a1) { *(uint64_t *)a1 = 0; *(uint64_t *)(a1 + 8) = 1; } c->x[0] = 0; break; }  // clock_getres -> 1ns
    case 115: nanosleep((const struct timespec *)a2, (struct timespec *)a3); c->x[0] = 0; break;  // clock_nanosleep
    case 153: c->x[0] = 0; break;                                                      // times
    case 169: { struct timeval tv; gettimeofday(&tv, 0); uint64_t *g = (uint64_t *)a0;  // gettimeofday
                if (g) { g[0] = tv.tv_sec; g[1] = tv.tv_usec; } c->x[0] = 0; break; }

    // ===================== Network — sockets; port-map (-p) + NET-ns private loopback =====================
    case 198: { int ty = (int)a1; int r = socket((int)a0, ty & 0xf, (int)a2);      // socket
                if (r >= 0) { if (ty & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC); if (ty & 0x800) fcntl(r, F_SETFL, O_NONBLOCK);
                              if (r < 1024) { g_sock_stream[r] = ((ty & 0xf) == SOCK_STREAM && (int)a0 == AF_INET); g_lo_port[r] = 0; } }
                c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 199: { int sv[2]; int r = socketpair((int)a0, (int)a1 & 0xf, (int)a2, sv);  // socketpair
                if (r == 0) { ((int *)a3)[0] = sv[0]; ((int *)a3)[1] = sv[1]; } c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break; }
    case 200: {                                                                       // bind -- port-map: bind the published host port
        uint8_t *sa = (uint8_t *)a1;                                                   // GUEST Linux sockaddr_in: family@0(u16 LE), port@2(BE)
        if (lo_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] && lo_is(sa, (socklen_t)a2)) {  // private loopback
            uint16_t p = ntohs(*(uint16_t *)(sa + 2)); char up[200]; lo_path(p, up, sizeof up);
            if (lo_swap((int)a0) < 0) { c->x[0] = (uint64_t)(-errno); break; }
            unlink(up); struct sockaddr_un un; memset(&un, 0, sizeof un); un.sun_family = AF_UNIX; snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = bind((int)a0, (struct sockaddr *)&un, sizeof un);
            if (r == 0) g_lo_port[(int)a0] = p ? p : 1;
            c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break;
        }
        if (g_nportmap && sa && a2 >= 8 && *(uint16_t *)(sa + 0) == AF_INET) {
            uint16_t cp = ntohs(*(uint16_t *)(sa + 2)), hp = pm_host(cp);
            if ((int)a0 >= 0 && (int)a0 < 1024) g_fd_cport[(int)a0] = cp;              // remember for getsockname
            if (hp != cp) { uint8_t buf[128]; socklen_t L = a2 < 128 ? (socklen_t)a2 : 128;
                memcpy(buf, sa, L); *(uint16_t *)(buf + 2) = htons(hp);                // publish on :H instead of :C (port @2)
                c->x[0] = bind((int)a0, (struct sockaddr *)buf, L) < 0 ? (uint64_t)(-errno) : 0; break; }
        }
        c->x[0] = bind((int)a0, (void *)a1, (socklen_t)a2) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 201: c->x[0] = listen((int)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0; break;
    case 202: case 242: { int lfd = (int)a0; int pl = (lfd >= 0 && lfd < 1024) ? g_lo_port[lfd] : 0;  // accept / accept4
                int r = pl ? accept(lfd, NULL, NULL) : accept(lfd, (void *)a1, (socklen_t *)a2);       // private-lo: don't expose unix peer
                if (r >= 0) { if (nr == 242) { if ((int)a3 & 0x800) fcntl(r, F_SETFL, fcntl(r, F_GETFL) | O_NONBLOCK);
                                               if ((int)a3 & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC); }
                              if (pl) { if (r < 1024) { g_lo_port[r] = pl; g_sock_stream[r] = 1; } fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, pl); } }  // peer = 127.0.0.1:lport
                c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 203: {                                                                       // connect
        uint8_t *sa = (uint8_t *)a1;
        if (lo_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] && lo_is(sa, (socklen_t)a2)) {  // private loopback
            uint16_t p = ntohs(*(uint16_t *)(sa + 2)); char up[200]; lo_path(p, up, sizeof up);
            if (lo_swap((int)a0) < 0) { c->x[0] = (uint64_t)(-errno); break; }
            struct sockaddr_un un; memset(&un, 0, sizeof un); un.sun_family = AF_UNIX; snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = connect((int)a0, (struct sockaddr *)&un, sizeof un);
            if (r == 0 || errno == EINPROGRESS) g_lo_port[(int)a0] = p ? p : 1;
            c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break;
        }
        c->x[0] = connect((int)a0, (void *)a1, (socklen_t)a2) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 204: { int fd = (int)a0;                                                     // getsockname
        if (fd >= 0 && fd < 1024 && g_lo_port[fd]) { fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, g_lo_port[fd]); c->x[0] = 0; break; }
        int r = getsockname(fd, (void *)a1, (socklen_t *)a2);
        if (r == 0 && g_nportmap && a1 && fd >= 0 && fd < 1024 && g_fd_cport[fd])
            *(uint16_t *)((uint8_t *)a1 + 2) = htons(g_fd_cport[fd]);                  // app sees the port it asked for (port @2)
        c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break; }
    case 205: { int fd = (int)a0;                                                     // getpeername
        if (fd >= 0 && fd < 1024 && g_lo_port[fd]) { fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, g_lo_port[fd]); c->x[0] = 0; break; }
        c->x[0] = getpeername(fd, (void *)a1, (socklen_t *)a2) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 206: { ssize_t r = sendto((int)a0, (void *)a1, (size_t)a2, msgflags_l2m((int)a3), (void *)a4, (socklen_t)a5);
                c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 207: { ssize_t r = recvfrom((int)a0, (void *)a1, (size_t)a2, msgflags_l2m((int)a3), (void *)a4, (socklen_t *)a5);
                c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 208: {                                                                   // setsockopt(fd, level, optname, val, len)
        int lvl = (int)a1, opt = (int)a2;
        if (lvl == 1) { lvl = SOL_SOCKET; opt = so_opt_l2m((int)a2); if (opt < 0) { c->x[0] = 0; break; } }  // translate SOL_SOCKET; ignore unknown
        int r = setsockopt((int)a0, lvl, opt, (void *)a3, (socklen_t)a4);          // other levels (TCP/IP) pass through (TCP_NODELAY matches)
        c->x[0] = r < 0 ? 0 : 0; (void)r; break;                                   // never fail the guest on an unsupported option
    }
    case 209: {                                                                   // getsockopt(fd, level, optname, val, len)
        int lvl = (int)a1, opt = (int)a2;
        if (lvl == 1) { lvl = SOL_SOCKET; opt = so_opt_l2m((int)a2);
            if (opt < 0) { if (a4 && *(socklen_t *)a4 >= 4 && a3) *(int *)a3 = 0; c->x[0] = 0; break; } }  // unknown -> report 0
        int r = getsockopt((int)a0, lvl, opt, (void *)a3, (socklen_t *)a4);
        c->x[0] = r < 0 ? (uint64_t)(-errno) : 0; break;
    }
    case 210: c->x[0] = shutdown((int)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0; break;   // shutdown(fd, how) -- SHUT_RD/WR/RDWR match
    case 211: case 212: {                                                         // sendmsg/recvmsg -- translate Linux msghdr -> macOS
        uint8_t *g = (uint8_t *)a1; struct msghdr mh; memset(&mh, 0, sizeof mh);   // Linux: iovlen/controllen are 8-byte; macOS 4
        mh.msg_name = (void *)*(uint64_t *)(g + 0); mh.msg_namelen = *(uint32_t *)(g + 8);
        mh.msg_iov = (void *)*(uint64_t *)(g + 16); mh.msg_iovlen = (int)*(uint64_t *)(g + 24);
        mh.msg_control = (void *)*(uint64_t *)(g + 32); mh.msg_controllen = (socklen_t)*(uint64_t *)(g + 40);
        mh.msg_flags = *(uint32_t *)(g + 48);
        ssize_t r = (nr == 211) ? sendmsg((int)a0, &mh, msgflags_l2m((int)a2)) : recvmsg((int)a0, &mh, msgflags_l2m((int)a2));
        if (nr == 212 && r >= 0) { *(uint32_t *)(g + 8) = mh.msg_namelen;          // recvmsg writes back name/control len + flags
                                   *(uint64_t *)(g + 40) = mh.msg_controllen; *(uint32_t *)(g + 48) = (uint32_t)mh.msg_flags; }
        c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 269: case 243: {                                                         // sendmmsg/recvmmsg(fd, mmsghdr[], vlen, flags, [timeout])
        uint8_t *vec = (uint8_t *)a1; unsigned vlen = (unsigned)a2; int done = 0, err = 0;  // mmsghdr = msghdr(56) + msg_len(4) + pad
        for (unsigned i = 0; i < vlen; i++) {
            uint8_t *g = vec + (size_t)i * 64; struct msghdr mh; memset(&mh, 0, sizeof mh);
            mh.msg_name = (void *)*(uint64_t *)(g + 0); mh.msg_namelen = *(uint32_t *)(g + 8);
            mh.msg_iov = (void *)*(uint64_t *)(g + 16); mh.msg_iovlen = (int)*(uint64_t *)(g + 24);
            mh.msg_control = (void *)*(uint64_t *)(g + 32); mh.msg_controllen = (socklen_t)*(uint64_t *)(g + 40);
            mh.msg_flags = *(uint32_t *)(g + 48);
            int rf = (int)a3; if (nr == 243 && i > 0) rf |= 0x40;                  // after the first, don't block (MSG_WAITFORONE-ish)
            ssize_t r = (nr == 269) ? sendmsg((int)a0, &mh, msgflags_l2m(rf)) : recvmsg((int)a0, &mh, msgflags_l2m(rf));
            if (r < 0) { err = errno; break; }
            *(uint32_t *)(g + 56) = (uint32_t)r;                                   // msg_len
            if (nr == 243) { *(uint32_t *)(g + 8) = mh.msg_namelen; *(uint64_t *)(g + 40) = mh.msg_controllen; *(uint32_t *)(g + 48) = (uint32_t)mh.msg_flags; }
            done++;
        }
        c->x[0] = (done == 0 && err) ? (uint64_t)(-(int64_t)err) : (uint64_t)done; break; }

    // ===================== Event loop — epoll/eventfd/timerfd/signalfd/inotify (macOS kqueue) =====================
    case 19: {                                                                         // eventfd2(initval, flags) -> pipe
        int fds[2]; if (pipe(fds) < 0) { c->x[0] = (uint64_t)(-errno); break; }
        if (a1 & 0x80000) { fcntl(fds[0], F_SETFD, FD_CLOEXEC); fcntl(fds[1], F_SETFD, FD_CLOEXEC); }   // EFD_CLOEXEC
        if (a1 & 0x800) { fcntl(fds[0], F_SETFL, O_NONBLOCK); fcntl(fds[1], F_SETFL, O_NONBLOCK); }       // EFD_NONBLOCK
        if (fds[0] < 1024 && fds[1] < 1024) g_eventfd_peer[fds[0]] = fds[1] + 1;       // writes to the eventfd go to fds[1]
        if (a0 > 0) { uint64_t v = a0; if (write(fds[1], &v, 8) < 0) {} }              // initval: read() returns it (else blocks)
        c->x[0] = (uint64_t)fds[0]; break; }
    case 20: { int r = kqueue();                                                       // epoll_create1(flags) -> kqueue
               if (r >= 0 && (a0 & 0x80000)) fcntl(r, F_SETFD, FD_CLOEXEC);            // EPOLL_CLOEXEC
               c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 21: {                                                                         // epoll_ctl(epfd, op, fd, event) -> kevent
        int op = (int)a1, fd = (int)a2; uint32_t ev = 0; uint64_t data = (uint64_t)(unsigned)fd;
        if (a3) { ev = *(uint32_t *)a3; memcpy(&data, (void *)(a3 + 4), 8); }          // struct epoll_event {u32 events; u64 data} packed
        struct kevent kv[2]; int n = 0;
        uint16_t base = (op == 2) ? EV_DELETE : EV_ADD;                                // op: 1=ADD 2=DEL 3=MOD
        uint16_t xf = (uint16_t)((ev & 0x80000000u ? EV_CLEAR : 0) | (ev & 0x40000000u ? EV_ONESHOT : 0));  // EPOLLET/ONESHOT
        if (op == 2 || (ev & 0x1)) { EV_SET(&kv[n], fd, EVFILT_READ,  base | xf, 0, 0, (void *)data); n++; }  // EPOLLIN
        if (op == 2 || (ev & 0x4)) { EV_SET(&kv[n], fd, EVFILT_WRITE, base | xf, 0, 0, (void *)data); n++; }  // EPOLLOUT
        for (int i = 0; i < n; i++) kevent((int)a0, &kv[i], 1, NULL, 0, NULL);          // per-filter so DEL of an absent one is ignored
        c->x[0] = 0; break; }
    case 22: {                                                                         // epoll_pwait(epfd, events, max, timeout_ms, sigmask)
        int maxev = (int)a2; if (maxev > 256) maxev = 256; if (maxev < 0) maxev = 0;
        struct kevent kv[256]; struct timespec ts, *tp = NULL;
        if ((int)a3 >= 0) { ts.tv_sec = (int)a3 / 1000; ts.tv_nsec = (long)((int)a3 % 1000) * 1000000L; tp = &ts; }
        int r = kevent((int)a0, NULL, 0, kv, maxev, tp);
        if (r < 0) { c->x[0] = (uint64_t)(-errno); break; }
        uint8_t *out = (uint8_t *)a1;
        for (int i = 0; i < r; i++) {
            uint32_t ev = (kv[i].filter == EVFILT_READ) ? 0x1u : (kv[i].filter == EVFILT_WRITE) ? 0x4u : 0u;
            if (kv[i].flags & EV_EOF) ev |= 0x10u;                                      // EPOLLHUP
            if (kv[i].flags & EV_ERROR) ev |= 0x8u;                                     // EPOLLERR
            *(uint32_t *)(out + i * 12) = ev; memcpy(out + i * 12 + 4, &kv[i].udata, 8);
        }
        c->x[0] = (uint64_t)r; break; }
    case 26: { int r = kqueue();                                                       // inotify_init1(flags) -> kqueue
               if (r >= 0) { if (r < 1024) g_inotify[r] = 1; if (a0 & 0x800) fcntl(r, F_SETFL, O_NONBLOCK); if (a0 & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC); }
               c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 27: {                                                                         // inotify_add_watch(fd, path, mask) -- kqueue EVFILT_VNODE
        char pb[4200]; const char *p = atpath(-100, (const char *)a1, pb, sizeof pb);   // confined (realpath gate)
        int wfd = open(p, O_EVTONLY); if (wfd < 0) { c->x[0] = (uint64_t)(-errno); break; }
        struct kevent kv; EV_SET(&kv, wfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
            NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB | NOTE_EXTEND, 0, (void *)(intptr_t)wfd);
        if (kevent((int)a0, &kv, 1, NULL, 0, NULL) < 0) { int e = errno; close(wfd); c->x[0] = (uint64_t)(-(int64_t)e); break; }
        c->x[0] = (uint64_t)wfd; break; }                                              // watch descriptor = the watched fd
    case 28: { struct kevent kv; EV_SET(&kv, (int)a1, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);  // inotify_rm_watch(fd, wd)
               kevent((int)a0, &kv, 1, NULL, 0, NULL); close((int)a1); c->x[0] = 0; break; }
    case 73: { struct pollfd *fds = (void *)a0; struct timespec *ts = (void *)a2;   // ppoll -> poll
               int tmo = ts ? (int)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000) : -1;
               int r = poll(fds, (nfds_t)a1, tmo); c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 74: {                                                                         // signalfd4(fd, mask, sizemask, flags)
        uint64_t lm = a1 ? *(uint64_t *)a1 : 0, pm = 0;                                 // sigset bit (signo-1) -> g_pending bit signo
        for (int s = 1; s < 64; s++) if (lm & (1ull << (s - 1))) pm |= (1ull << s);
        if (g_sigfd_pipe[0] < 0 && pipe(g_sigfd_pipe) < 0) { c->x[0] = (uint64_t)(-errno); break; }
        g_sigfd_mask |= pm; g_sigfd_read = g_sigfd_pipe[0];
        for (int s = 1; s < 64; s++) if ((pm & (1ull << s)) && !sig_is_sync(s)) {       // make sure the host delivers them
            struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = host_sigh; sigaction(sig_l2m(s), &sa, NULL); }
        if (a3 & 0x80000) fcntl(g_sigfd_pipe[0], F_SETFD, FD_CLOEXEC);                  // SFD_CLOEXEC
        if (a3 & 0x800) fcntl(g_sigfd_pipe[0], F_SETFL, O_NONBLOCK);                    // SFD_NONBLOCK
        c->x[0] = (uint64_t)g_sigfd_pipe[0]; break; }
    case 85: { int r = kqueue();                                                       // timerfd_create(clockid, flags) -> kqueue
               if (r >= 0) { if (r < 1024) g_timerfd[r] = 1; if (a1 & 1) fcntl(r, F_SETFL, O_NONBLOCK); }  // TFD_NONBLOCK=1
               c->x[0] = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 86: {                                                                         // timerfd_settime(fd, flags, new, old)
        struct kevent kv; uint64_t iv_s=0, iv_n=0, vl_s=0, vl_n=0;
        if (a2) { memcpy(&iv_s, (void *)a2, 8); memcpy(&iv_n, (void *)(a2 + 8), 8); memcpy(&vl_s, (void *)(a2 + 16), 8); memcpy(&vl_n, (void *)(a2 + 24), 8); }
        int64_t period_ns = (iv_s || iv_n) ? (int64_t)(iv_s * 1000000000ull + iv_n)    // periodic uses it_interval
                                           : (int64_t)(vl_s * 1000000000ull + vl_n);   // one-shot uses it_value
        if (period_ns <= 0) { EV_SET(&kv, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL); kevent((int)a0, &kv, 1, NULL, 0, NULL); c->x[0] = 0; break; }  // disarm
        uint16_t fl = EV_ADD | ((iv_s || iv_n) ? 0 : EV_ONESHOT);                       // no interval -> one-shot
        EV_SET(&kv, 1, EVFILT_TIMER, fl, NOTE_NSECONDS, period_ns, NULL);
        c->x[0] = kevent((int)a0, &kv, 1, NULL, 0, NULL) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 87: { if (a1) memset((void *)a1, 0, 32); c->x[0] = 0; break; }                // timerfd_gettime -> best-effort 0

    // ===================== Misc — uname/sysinfo/getrandom/hostname =====================
    case 160: { char *u = (char *)a0; memset(u, 0, 6 * 65);                        // uname
                strcpy(u, "Linux"); strcpy(u + 65, g_hostname[0] ? g_hostname : "jit"); strcpy(u + 130, "6.1.0");
                strcpy(u + 195, "#1 jit"); strcpy(u + 260, "aarch64"); c->x[0] = 0; break; }
    case 161: { int n = (int)a1; if (n > 64) n = 64; if (n > 0) { memcpy(g_hostname, (void *)a0, n); g_hostname[n] = 0; } // sethostname (UTS ns)
                c->x[0] = 0; break; }
    case 162: c->x[0] = 0; break;                                                       // setdomainname -> ignore
    case 179: memset((void *)a0, 0, 112); c->x[0] = 0; break;                          // sysinfo
    case 278: arc4random_buf((void *)a0, (size_t)a1); c->x[0] = a1; break;         // getrandom
    case 293: c->x[0] = (uint64_t)(-ENOSYS); break; // rseq -> ENOSYS (glibc falls back)

    // ===================== unhandled =====================
    default:
        fprintf(stderr, "[jit] unhandled syscall %llu (a0=%llx a1=%llx) at pc=%llx\n",
                (unsigned long long)nr, (unsigned long long)a0, (unsigned long long)a1,
                (unsigned long long)c->pc);
        c->x[0] = (uint64_t)(-ENOSYS); break;   // ENOSYS, keep going so we can see what's next
    }
    // Boundary errno translation: every case sets c->x[0] to a host(macOS) errno on error
    // (-errno, saved e, helper returns, or a macOS E* constant). Map to the Linux errno the guest
    // expects. Skip redirect (sigreturn restored an already-Linux x0 from the signal frame).
    if (!c->redirect) { int64_t rv = (int64_t)c->x[0];
        if (rv < 0 && rv >= -4095) c->x[0] = (uint64_t)(-(int64_t)m2l_errno((int)(-rv))); }
}

