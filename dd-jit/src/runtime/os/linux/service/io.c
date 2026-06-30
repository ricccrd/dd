// Extracted from service(): I/O — fd read/write/seek + plain fd ops (dup/dup3/fcntl/pipe2/sendfile/splice/tee/copy_file_range/fsync/etc).
// Returns 1 if nr was handled, 0 otherwise. Included by service.c after service/helpers.c, before service() — same TU scope (globals + helpers).

// Guest O_DIRECT differs per arch (aarch64/asm-generic = 0x10000, x86-64 = 0x4000); derive it from the
// arch's O_DIRECTORY (provided by abi.h) so pipe2(O_DIRECT) is recognised on both targets.
#if G_O_DIRECTORY == 0x10000
#define G_O_DIRECT 0x4000 // x86-64
#else
#define G_O_DIRECT 0x10000 // aarch64 / asm-generic
#endif

static int svc_io(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (nr) {
    // ===================== I/O — read/write/seek (+ eventfd/timerfd/signalfd fd redirection) =====================
    case 62: {
        // lseek -- SEEK_SET/CUR/END(0/1/2) match, but SEEK_DATA/SEEK_HOLE are SWAPPED between the ABIs:
        // Linux SEEK_DATA=3,SEEK_HOLE=4 ; macOS SEEK_HOLE=3,SEEK_DATA=4. Translate so sparse-file
        // probing finds holes/data correctly.
        int whence = (int)a2;
        struct memf *mm = memf_get((int)a0);
        if (mm) {
            off_t mr = memf_lseek(mm, (off_t)a1, whence);
            if (mr != -2) { G_RET(c) = mr < 0 ? (uint64_t)(-EINVAL) : (uint64_t)mr; break; }
            memf_materialize((int)a0); // SEEK_DATA/HOLE: fall through to the now-materialized host fd
        }
        if (whence == 3) whence = 4;      // Linux SEEK_DATA -> macOS SEEK_DATA
        else if (whence == 4) whence = 3; // Linux SEEK_HOLE -> macOS SEEK_HOLE
        off_t r = lseek((int)a0, (off_t)a1, whence);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 63: {
        int rfd = (int)a0;
        // RAM-backed scratch file: serve the read from memory
        if (memf_get(rfd)) {
            ssize_t r = memf_read_pos(g_memf[rfd], (void *)a1, (size_t)a2);
            G_RET(c) = r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
            break;
        }
        // signalfd read -> struct signalfd_siginfo
        if (rfd >= 0 && rfd == g_sigfd_read) {
            char b;
            // drain one wake byte
            ssize_t pr = read(rfd, &b, 1);
            if (pr <= 0) {
                G_RET(c) = (uint64_t)(int64_t)(pr < 0 ? -errno : -EAGAIN);
                break;
            }
            int sig = 0;
            uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST);
            for (int s = 1; s < 64; s++)
                if ((p & (1ull << s)) && (g_sigfd_mask & (1ull << s))) {
                    sig = s;
                    break;
                }
            if (sig) __atomic_and_fetch(&g_pending, ~(1ull << (unsigned)sig), __ATOMIC_SEQ_CST);
            if (a1 && a2 >= 128) {
                memset((void *)a1, 0, 128);
                *(uint32_t *)a1 = (uint32_t)sig;
            // ssi_signo
            }
            G_RET(c) = 128;
            break;
        }
        // inotify read -> struct inotify_event[]
        if (rfd >= 0 && rfd < 1024 && g_inotify[rfd]) {
            struct kevent kv[32];
            struct timespec zero = {0, 0};
            int nb = fcntl(rfd, F_GETFL) & O_NONBLOCK;
            int n = kevent(rfd, NULL, 0, kv, 32, nb ? &zero : NULL);
            if (n <= 0) {
                G_RET(c) = (uint64_t)(int64_t)(n < 0 ? -errno : -EAGAIN);
                break;
            }
            uint8_t *out = (uint8_t *)a1;
            size_t off = 0;
            for (int i = 0; i < n; i++) {
                int wd = (int)kv[i].ident;
                if (wd >= 0 && wd < 1024 && g_inotify_wpath[wd][0]) {
                    // directory watch: diff current entries against the snapshot -> IN_CREATE/IN_DELETE+name
                    char *cur = dir_snapshot(g_inotify_wpath[wd]);
                    char *old = g_inotify_snap[wd];
                    for (int pass = 0; pass < 2; pass++) {            // pass 0 = created, pass 1 = deleted
                        const char *src = pass == 0 ? cur : old, *other = pass == 0 ? old : cur;
                        uint32_t mask = pass == 0 ? 0x100u : 0x200u;  // IN_CREATE / IN_DELETE
                        for (const char *p = src ? src : ""; *p;) {
                            const char *e = strchr(p, '\n');
                            size_t l = e ? (size_t)(e - p) : strlen(p);
                            if (l && !snap_has(other, p, l)) {
                                size_t nlen = (l + 1 + 15) & ~(size_t)15; // padded name field
                                if (off + 16 + nlen > a2) break;
                                *(int32_t *)(out + off) = wd;
                                *(uint32_t *)(out + off + 4) = mask;
                                *(uint32_t *)(out + off + 8) = 0;               // cookie
                                *(uint32_t *)(out + off + 12) = (uint32_t)nlen; // len
                                memcpy(out + off + 16, p, l);
                                memset(out + off + 16 + l, 0, nlen - l);
                                off += 16 + nlen;
                            }
                            p = e ? e + 1 : p + l;
                        }
                    }
                    free(old);
                    g_inotify_snap[wd] = cur;
                } else {
                    if (off + 16 > a2) break;
                    uint32_t f = kv[i].fflags, m = 0;
                    if (f & (NOTE_WRITE | NOTE_EXTEND)) m |= 0x2;  // IN_MODIFY
                    if (f & NOTE_ATTRIB) m |= 0x4;                 // IN_ATTRIB
                    if (f & NOTE_DELETE) m |= 0x400;               // IN_DELETE_SELF
                    if (f & NOTE_RENAME) m |= 0x800;               // IN_MOVE_SELF
                    *(int32_t *)(out + off) = wd;
                    *(uint32_t *)(out + off + 4) = m;
                    *(uint32_t *)(out + off + 8) = 0;
                    *(uint32_t *)(out + off + 12) = 0;
                    off += 16;
                }
            }
            G_RET(c) = (uint64_t)off;
            break;
        }
        // timerfd read -> drain timer, return count
        if (rfd >= 0 && rfd < 1024 && g_timerfd[rfd]) {
            struct kevent kv;
            struct timespec zero = {0, 0};
            int nb = fcntl(rfd, F_GETFL) & O_NONBLOCK;
            int n = kevent(rfd, NULL, 0, &kv, 1, nb ? &zero : NULL);
            if (n <= 0) {
                G_RET(c) = (uint64_t)(int64_t)(n < 0 ? -errno : -EAGAIN);
                break;
            // EAGAIN
            }
            if (a1 && a2 >= 8) *(uint64_t *)a1 = (uint64_t)kv.data;
            G_RET(c) = 8;
            break;
        }
        // eventfd read: return the accumulated counter, reset it, drain the readiness pipe
        if (rfd >= 0 && rfd < 1024 && g_eventfd_peer[rfd]) {
            if (a2 < 8) { G_RET(c) = (uint64_t)(-EINVAL); break; }
            if (g_eventfd_count[rfd] == 0) {
                if (fcntl(rfd, F_GETFL) & O_NONBLOCK) { G_RET(c) = (uint64_t)(-EAGAIN); break; }
                char b; if (read(rfd, &b, 1) < 0) {} // block until a writer signals 0->positive
            }
            uint64_t v;
            if (g_eventfd_sema[rfd]) { v = 1; g_eventfd_count[rfd] -= 1; } // EFD_SEMAPHORE: one at a time
            else { v = g_eventfd_count[rfd]; g_eventfd_count[rfd] = 0; }
            // re-sync the pipe to "counter > 0": drain it, then re-signal one byte if still positive
            int fl = fcntl(rfd, F_GETFL);
            fcntl(rfd, F_SETFL, fl | O_NONBLOCK);
            char buf[64]; while (read(rfd, buf, sizeof buf) > 0) {}
            fcntl(rfd, F_SETFL, fl);
            if (g_eventfd_count[rfd] > 0) { char b = 1; if (write(g_eventfd_peer[rfd] - 1, &b, 1) < 0) {} }
            if (a1) *(uint64_t *)a1 = v;
            G_RET(c) = 8;
            break;
        }
        ssize_t r = read(rfd, (void *)a1, (size_t)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 64: {
        int wfd = (int)a0;
        // RAM-backed scratch file: serve the write from memory (spill to the host file past the cap)
        if (memf_get(wfd) && memf_room_or_spill(wfd, (off_t)g_memf[wfd]->pos + (off_t)a2)) {
            ssize_t r = memf_write_pos(g_memf[wfd], (void *)a1, (size_t)a2);
            G_RET(c) = r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
            break;
        }
        // eventfd write: ADD to the counter (not a raw pipe write); signal the pipe readable on 0->positive.
        if (wfd >= 0 && wfd < 1024 && g_eventfd_peer[wfd]) {
            if (a2 < 8) { G_RET(c) = (uint64_t)(-EINVAL); break; }
            uint64_t add = *(uint64_t *)a1;
            if (add == 0xffffffffffffffffULL) { G_RET(c) = (uint64_t)(-EINVAL); break; }
            uint64_t old = g_eventfd_count[wfd];
            g_eventfd_count[wfd] = old + add;
            if (old == 0 && add > 0) { char b = 1; if (write(g_eventfd_peer[wfd] - 1, &b, 1) < 0) {} }
            G_RET(c) = 8;
            break;
        }
        fd_evict(wfd);
        ssize_t r = write(wfd, (void *)a1, (size_t)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 65: {
        if (memf_get((int)a0)) {
            ssize_t r = memf_preadv(g_memf[(int)a0], (const struct iovec *)a1, (int)a2, -1, 1);
            G_RET(c) = r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
            break;
        }
        ssize_t r = readv((int)a0, (void *)a1, (int)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    // readv
    }
    case 66: {
        if (memf_get((int)a0)) {
            const struct iovec *iv = (const struct iovec *)a1;
            off_t end = g_memf[(int)a0]->pos;
            for (int i = 0; i < (int)a2; i++) end += iv[i].iov_len;
            if (memf_room_or_spill((int)a0, end)) {
                ssize_t r = memf_pwritev(g_memf[(int)a0], iv, (int)a2, -1, 1);
                G_RET(c) = r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
                break;
            }
        }
        fd_evict((int)a0);
        ssize_t r = writev((int)a0, (void *)a1, (int)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    // writev
    }
    case 67: {
        // pread64
        if (memf_get((int)a0)) {
            ssize_t r = memf_pread(g_memf[(int)a0], (void *)a1, (size_t)a2, (off_t)a3);
            G_RET(c) = r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
            break;
        }
        ssize_t r = pread((int)a0, (void *)a1, (size_t)a2, (off_t)a3);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 68: {
        // pwrite64
        if (memf_get((int)a0) && memf_room_or_spill((int)a0, (off_t)a3 + (off_t)a2)) {
            ssize_t r = memf_pwrite(g_memf[(int)a0], (void *)a1, (size_t)a2, (off_t)a3);
            G_RET(c) = r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
            break;
        }
        fd_evict((int)a0);
        ssize_t r = pwrite((int)a0, (void *)a1, (size_t)a2, (off_t)a3);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // sendfile(out,in,off*,count)
    case 71: {
        int outfd = (int)a0, infd = (int)a1;
        memf_materialize(outfd); // sendfile reads/writes via the real fds -> flush RAM cache first
        memf_materialize(infd);
        off_t *po = (off_t *)a2;
        size_t cnt = (size_t)a3;
        if (po) lseek(infd, *po, SEEK_SET);
        char bf[65536];
        size_t tot = 0;
        while (tot < cnt) {
            size_t w = cnt - tot < sizeof bf ? cnt - tot : sizeof bf;
            ssize_t n = read(infd, bf, w);
            if (n <= 0) break;
            ssize_t wr = write(outfd, bf, n);
            if (wr < 0) break;
            tot += wr;
            if (wr < n) break;
        }
        if (po) *po += tot;
        G_RET(c) = tot;
        break;
    }
    case 76:
    // splice(fd_in,off_in,fd_out,off_out,len,fl) / tee -> emulate
    case 77: {
        int fin = (int)a0, fout = (int)a2;
        memf_materialize(fin); // splice/tee move bytes via the real fds -> flush RAM cache first
        memf_materialize(fout);
        size_t len = (size_t)a4;
        if (len > 65536) len = 65536;
        static __thread char sb[65536];
        ssize_t n;
        fd_evict(fout);
        if (nr == 76 && a1)
            n = pread(fin, sb, len, *(off_t *)a1);
        else
            // splice off_in
            n = read(fin, sb, len);
        if (n <= 0) {
            G_RET(c) = n < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        ssize_t w = (nr == 76 && a3) ? pwrite(fout, sb, n, *(off_t *)a3) : write(fout, sb, n);
        if (w < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        if (nr == 76 && a1) *(off_t *)a1 += w;
        if (nr == 76 && a3) *(off_t *)a3 += w;
        G_RET(c) = (uint64_t)w;
        break;
    }
    case 23: {
        // dup -- a 2nd fd would share the description; flush the RAM cache so both see the real file
        memf_materialize((int)a0);
        int r = dup((int)a0);
        // carry path + socket-emulation metadata to the new fd
        if (r >= 0 && r < 1024 && (int)a0 >= 0 && (int)a0 < 1024) { strcpy(g_fdpath[r], g_fdpath[(int)a0]); fd_carry_sock(r, (int)a0); }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 24: {
        // dup3(old,new,flags) -- unlike dup2, equal oldfd==newfd is an error (EINVAL) on Linux
        if ((int)a0 == (int)a1) {
            G_RET(c) = (uint64_t)(-EINVAL);
            break;
        }
        memf_materialize((int)a0); // source: a 2nd fd shares the description -> flush RAM cache
        memf_close((int)a1);       // target fd is about to be reused; drop any cache it held
        int r = dup2((int)a0, (int)a1);
        if (r >= 0) {
            if ((int)a2 & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC); // O_CLOEXEC
            if ((int)a1 >= 0 && (int)a1 < 1024 && (int)a0 >= 0 && (int)a0 < 1024) {
                strcpy(g_fdpath[(int)a1], g_fdpath[(int)a0]);
                fd_carry_sock((int)a1, (int)a0);
            }
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 25: {
        // fcntl -- Linux cmd# -> macOS (they diverge!)
        int lcmd = (int)a1;
        // F_DUPFD(_CLOEXEC) makes a 2nd fd sharing the description; F_SETFL O_APPEND changes write-offset
        // semantics. Either way, flush a RAM-backed fd so the real host fd takes over with correct bytes.
        if (lcmd == 0 || lcmd == 1030 || (lcmd == 4 && ((int)a2 & 0x400))) memf_materialize((int)a0);
        // F_GETFL: macOS O_* -> Linux O_*
        if (lcmd == 3) {
            int r = fcntl((int)a0, F_GETFL, 0);
            if (r < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
            }
            // access mode identical
            int lf = r & 0x3;
            if (r & 0x8) lf |= 0x400;
            if (r & 0x4) lf |= 0x800;
            // APPEND/NONBLOCK/ASYNC
            if (r & 0x40) lf |= 0x2000;
            G_RET(c) = (uint64_t)(unsigned)lf;
            break;
        }
        // F_SETFL: Linux O_* -> macOS O_*
        if (lcmd == 4) {
            int la = (int)a2, mf = 0;
            if (la & 0x400) mf |= 0x8;
            if (la & 0x800) mf |= 0x4;
            // APPEND/NONBLOCK/ASYNC
            if (la & 0x2000) mf |= 0x40;
            int r = fcntl((int)a0, F_SETFL, mf);
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        // F_GETLK/SETLK/SETLKW: xlate struct flock + cmd
        if (lcmd == 5 || lcmd == 6 || lcmd == 7) {
            // macOS F_GETLK=7,SETLK=8,SETLKW=9
            int mc = lcmd == 5 ? F_GETLK : lcmd == 6 ? F_SETLK : F_SETLKW;
            uint8_t *lf = (uint8_t *)a2;
            struct flock fl;
            // Linux flock: type/whence/pad/start@8/len@16/pid@24
            memset(&fl, 0, sizeof fl);
            short lt = *(short *)(lf + 0);
            // Linux RDLCK=0,WRLCK=1,UNLCK=2 -> macOS
            fl.l_type = lt == 0 ? F_RDLCK : lt == 1 ? F_WRLCK : F_UNLCK;
            fl.l_whence = *(short *)(lf + 2);
            fl.l_start = *(int64_t *)(lf + 8);
            fl.l_len = *(int64_t *)(lf + 16);
            fl.l_pid = *(int32_t *)(lf + 24);
            int r = fcntl((int)a0, mc, &fl), e = errno;
            // F_GETLK writes the conflicting lock back
            if (r >= 0 && lcmd == 5) {
                *(short *)(lf + 0) = fl.l_type == F_RDLCK ? 0 : fl.l_type == F_WRLCK ? 1 : 2;
                *(short *)(lf + 2) = fl.l_whence;
                *(int64_t *)(lf + 8) = fl.l_start;
                *(int64_t *)(lf + 16) = fl.l_len;
                *(int32_t *)(lf + 24) = (int32_t)fl.l_pid;
            }
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : (uint64_t)r;
            break;
        }
        // F_SETPIPE_SZ(1031)/F_GETPIPE_SZ(1032): macOS can't resize a pipe, so emulate -- record the
        // requested size (rounded up to a page, >= requested) and report it back on GET.
        if (lcmd == 1031) {
            int want = (int)a2;
            long pg = sysconf(_SC_PAGESIZE);
            if (pg <= 0) pg = 4096;
            int rounded = (int)(((want + pg - 1) / pg) * pg);
            if (rounded < (int)pg) rounded = (int)pg;
            if ((int)a0 >= 0 && (int)a0 < 1024) g_pipesz[(int)a0] = rounded;
            G_RET(c) = (uint64_t)(unsigned)rounded;
            break;
        }
        if (lcmd == 1032) {
            int sz = ((int)a0 >= 0 && (int)a0 < 1024 && g_pipesz[(int)a0]) ? g_pipesz[(int)a0] : 65536;
            G_RET(c) = (uint64_t)(unsigned)sz;
            break;
        }
        int mcmd = lcmd;
        if (lcmd == 8)
            mcmd = F_SETOWN;
        else if (lcmd == 9)
            // owner cmds also swapped on macOS
            mcmd = F_GETOWN;
        else if (lcmd == 1030)
            mcmd = F_DUPFD_CLOEXEC;
        else if (lcmd == 1024 || lcmd == 1025 || lcmd == 1026 || lcmd == 1033 || lcmd == 1034) {
            G_RET(c) = 0;
            break;
        // lease/notify/seals: no-op
        }
        int r = fcntl((int)a0, mcmd, a2);
        if (r >= 0 && (lcmd == 0 || lcmd == 1030) && r < 1024 && (int)a0 >= 0 && (int)a0 < 1024) {
            // F_DUPFD(_CLOEXEC)
            strcpy(g_fdpath[r], g_fdpath[(int)a0]);
            fd_carry_sock(r, (int)a0);
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 59: {
        // pipe2(fds, flags). O_DIRECT requests "packet mode": each write is a distinct record that reads
        // back whole, never coalesced. macOS pipes can't do this, but an AF_UNIX SOCK_DGRAM socketpair
        // preserves message boundaries exactly, so back an O_DIRECT pipe with one (SOCK_SEQPACKET would be
        // closer but macOS PF_LOCAL doesn't support it). A plain pipe is fine for the non-O_DIRECT case.
        int fds[2], fl = (int)a1;
        int mk = (fl & G_O_DIRECT) ? socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) : pipe(fds);
        if (mk < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        if (fl & 0x80000) {
            fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        }
        if (fl & 0x800) {
            fcntl(fds[0], F_SETFL, O_NONBLOCK);
            fcntl(fds[1], F_SETFL, O_NONBLOCK);
        }
        ((int *)a0)[0] = fds[0];
        ((int *)a0)[1] = fds[1];
        G_RET(c) = 0;
        break;
    }
    // fsync -- durability policy (S3DB_DURABILITY): default/fast == plain fsync() (legacy path)
    // A RAM-backed scratch file is anonymous/private: fsync has no observable effect -> 0.
    case 82: G_RET(c) = memf_get((int)a0) ? 0 : s3db_sync_fd((int)a0); break;
    // fdatasync -> fsync (no macOS fdatasync); same durability policy
    case 83: G_RET(c) = memf_get((int)a0) ? 0 : s3db_sync_fd((int)a0); break;
    // copy_file_range(fdin,offin*,fdout,offout*,len,flags)
    case 285: {
        int fdin = (int)a0, fdout = (int)a2;
        memf_materialize(fdin); // copy_file_range moves bytes via the real fds -> flush RAM caches first
        memf_materialize(fdout);
        size_t len = (size_t)a4, done = 0;
        int err = 0;
        off_t *poi = (off_t *)a1, *poo = (off_t *)a3;
        off_t oi = poi ? *poi : -1, oo = poo ? *poo : -1;
        char cb[8192];
        while (done < len) {
            size_t chunk = (len - done > sizeof cb) ? sizeof cb : len - done;
            ssize_t r = (oi >= 0) ? pread(fdin, cb, chunk, oi) : read(fdin, cb, chunk);
            if (r < 0) {
                err = errno;
                break;
            }
            if (r == 0) break;
            ssize_t w = (oo >= 0) ? pwrite(fdout, cb, (size_t)r, oo) : write(fdout, cb, (size_t)r);
            if (w < 0) {
                err = errno;
                break;
            }
            done += (size_t)w;
            if (oi >= 0) oi += w;
            if (oo >= 0) oo += w;
            if (w < r) break;
        }
        if (poi) *poi = oi;
        if (poo) *poo = oo;
        fd_evict(fdout);
        G_RET(c) = (done == 0 && err) ? (uint64_t)(-(int64_t)err) : (uint64_t)done;
        break;
    }
    // preadv/pwritev: struct iovec layout is identical Linux<->macOS
    case 69: {
        if (memf_get((int)a0)) {
            ssize_t r = memf_preadv(g_memf[(int)a0], (const struct iovec *)a1, (int)a2, (off_t)a3, 0);
            G_RET(c) = r < 0 ? (uint64_t)(int64_t)r : (uint64_t)r;
            break;
        }
        ssize_t r = preadv((int)a0, (const struct iovec *)a1, (int)a2, (off_t)a3);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 70: {
        if (memf_get((int)a0)) {
            const struct iovec *iv = (const struct iovec *)a1;
            off_t end = (off_t)a3;
            for (int i = 0; i < (int)a2; i++) end += iv[i].iov_len;
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
    case 84: G_RET(c) = memf_get((int)a0) ? 0 : s3db_sync_fd((int)a0); break; // sync_file_range -> fsync (no-op for RAM scratch)
    default: return 0;
    }
    return svc_done(c); // boundary errno xlate (host macOS -> Linux); see helpers.c svc_done
}
