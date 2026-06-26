// dd/runtime/frontend/x86_64 -- service(): the Linux syscall layer keyed by x86-64 syscall numbers
// (read=0, openat=257, ...). Reuses the macOS ABI translations; merges into os/linux/service.c at dedup.

// ---------------- syscalls (x86-64 numbers -> reused macOS translations) ----------------
static uint64_t brk_lo, brk_cur, brk_hi;
// Track our anon-RW mappings so guest MAP_FIXED (the dynamic loader placing library
// segments) is emulated in-place (pread/memset) instead of a real macOS MAP_FIXED,
// which would SPLIT the mapping and SIGBUS the rest (the macOS/OrbStack VM quirk).
static struct {
    uint64_t lo, hi;
} g_regions[256];
static int g_nregions;
static int region_has(uint64_t a, uint64_t end) {
    if (a >= brk_lo && end <= brk_hi) return 1;
    for (int i = 0; i < g_nregions; i++)
        if (a >= g_regions[i].lo && end <= g_regions[i].hi) return 1;
    return 0;
}
static void region_add(uint64_t lo, uint64_t hi) { // page-round: macOS maps whole pages, and the
    hi = (hi + 4095) & ~(uint64_t)4095;            // loader's FIXED segment mmaps are page-aligned and
    lo = lo & ~(uint64_t)4095;                     // may extend past the reservation's unrounded end.
    if (g_nregions < 256) {
        g_regions[g_nregions].lo = lo;
        g_regions[g_nregions].hi = hi;
        g_nregions++;
    }
}

static void service(struct cpu *c) {
    // x86-64 ABI: nr=rax, args rdi,rsi,rdx,r10,r8,r9 ; return -> rax
    uint64_t nr = c->r[RAX];
    uint64_t a0 = c->r[RDI], a1 = c->r[RSI], a2 = c->r[RDX], a3 = c->r[10], a4 = c->r[8], a5 = c->r[9];
    uint64_t ret;
    if (g_trace)
        fprintf(stderr, "[sys] %llu (%llx,%llx,%llx)", (unsigned long long)nr, (unsigned long long)a0,
                (unsigned long long)a1, (unsigned long long)a2);
    switch (nr) {
    case 501:
        fprintf(stderr, "[w8] --- mallocs done, frees begin ---\n");
        ret = 0;
        break;
    case 500:
        g_w8 = (uint8_t *)a0;
        g_w8v = g_w8 ? *g_w8 : 0; // debug: arm byte-watchpoint
        fprintf(stderr, "[w8] armed @%p = %02x\n", (void *)g_w8, g_w8v);
        ret = 0;
        break;
    case 0: {
        int rfd = (int)a0;                     // read
        if (rfd >= 0 && rfd == g_sigfd_read) { // signalfd read -> struct signalfd_siginfo
            char b;
            ssize_t pr = read(rfd, &b, 1); // drain one wake byte
            if (pr <= 0) {
                ret = (uint64_t)(int64_t)(pr < 0 ? -derr(errno) : -11);
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
            } // ssi_signo @0
            ret = 128;
            break;
        }
        if (rfd >= 0 && rfd < 1024 && (g_inotify[rfd] || g_timerfd[rfd])) { // inotify/timerfd -> drain kqueue
            struct kevent kv[32];
            struct timespec zero = {0, 0};
            int nb = fcntl(rfd, F_GETFL) & O_NONBLOCK;
            int n = kevent(rfd, NULL, 0, kv, g_inotify[rfd] ? 32 : 1, nb ? &zero : NULL);
            if (n <= 0) {
                ret = (uint64_t)(-(n < 0 ? derr(errno) : 11));
                break;
            } // EAGAIN=11
            if (g_timerfd[rfd]) {
                if (a1 && a2 >= 8) *(uint64_t *)a1 = (uint64_t)kv[0].data;
                ret = 8;
                break;
            }
            uint8_t *out = (uint8_t *)a1;
            size_t off = 0;
            for (int i = 0; i < n && off + 16 <= a2; i++) {
                uint32_t f = kv[i].fflags, m = 0;
                if (f & (NOTE_WRITE | NOTE_EXTEND)) m |= 0x2;
                if (f & NOTE_ATTRIB) m |= 0x4;
                if (f & NOTE_DELETE) m |= 0x400;
                if (f & NOTE_RENAME) m |= 0x800;
                *(int32_t *)(out + off) = (int32_t)kv[i].ident;
                *(uint32_t *)(out + off + 4) = m;
                *(uint32_t *)(out + off + 8) = 0;
                *(uint32_t *)(out + off + 12) = 0;
                off += 16;
            }
            ret = (uint64_t)off;
            break;
        }
        ssize_t r = read(rfd, (void *)a1, (size_t)a2);
        ret = r < 0 ? (uint64_t)(-derr(errno)) : (uint64_t)r;
        break;
    }
    case 1: {
        int wfd = (int)a0; // write (eventfd -> its pipe write-end)
        if (wfd >= 0 && wfd < 1024 && g_eventfd_peer[wfd]) wfd = g_eventfd_peer[wfd] - 1;
        ssize_t r = write(wfd, (void *)a1, (size_t)a2);
        ret = r < 0 ? (uint64_t)(-derr(errno)) : (uint64_t)r;
        break;
    }
    case 19: ret = (uint64_t)readv((int)a0, (void *)a1, (int)a2); break;  // readv
    case 20: ret = (uint64_t)writev((int)a0, (void *)a1, (int)a2); break; // writev
    case 17: {
        ssize_t r = pread((int)a0, (void *)a1, (size_t)a2, (off_t)a3);
        ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    } // pread64
    case 18: {
        ssize_t r = pwrite((int)a0, (void *)a1, (size_t)a2, (off_t)a3);
        ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    } // pwrite64
    case 257: {
        const char *raw = (const char *)a1;
        char pb[4200];
        const char *p; // openat
        int lf = (int)a2, mf = lf & 0x3;
        if (lf & 0x40) mf |= O_CREAT;
        if (lf & 0x80) mf |= O_EXCL;
        if (lf & 0x200) mf |= O_TRUNC;
        if (lf & 0x400) mf |= O_APPEND;
        if (lf & 0x800) mf |= O_NONBLOCK;
        if (lf & 0x10000) mf |= O_DIRECTORY;
        if (lf & 0x80000) mf |= O_CLOEXEC;
        if (g_nlower && raw && raw[0] == '/') { // OVERLAY: write copies-up, read resolves across layers
            char host[4300];
            if ((mf & 3) != O_RDONLY || (lf & 0x40))
                overlay_copyup(raw, host, sizeof host);
            else
                overlay_resolve(raw, host, sizeof host, 0);
            snprintf(pb, sizeof pb, "%s", host);
            p = pb;
        } else
            p = atpath(raw, pb, sizeof pb);
        int r = openat(ATFD(a0), p, mf, (mode_t)a3);
        if (r >= 0 && r < 1024 && g_nlower && raw && raw[0] == '/') {
            snprintf(g_ovldir[r], sizeof g_ovldir[r], "%s", raw);
            g_ovlcur[r] = 0;
        } // guest path + cursor for merged getdents
        ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 2: {
        const char *raw = (const char *)a0;
        char pb[4200];
        const char *p; // open
        int lf = (int)a1, mf = lf & 0x3;
        if (lf & 0x40) mf |= O_CREAT;
        if (lf & 0x200) mf |= O_TRUNC;
        if (g_nlower && raw && raw[0] == '/') { // OVERLAY
            char host[4300];
            if ((mf & 3) != O_RDONLY || (lf & 0x40))
                overlay_copyup(raw, host, sizeof host);
            else
                overlay_resolve(raw, host, sizeof host, 0);
            snprintf(pb, sizeof pb, "%s", host);
            p = pb;
        } else
            p = atpath(raw, pb, sizeof pb);
        int r = open(p, mf, (mode_t)a2);
        if (r >= 0 && r < 1024 && g_nlower && raw && raw[0] == '/') {
            snprintf(g_ovldir[r], sizeof g_ovldir[r], "%s", raw);
            g_ovlcur[r] = 0;
        }
        ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 3: {
        int cf = (int)a0; // close (reap eventfd peer / timerfd / inotify / loopback state)
        if (cf >= 0 && cf < 1024) {
            if (g_eventfd_peer[cf]) {
                close(g_eventfd_peer[cf] - 1);
                g_eventfd_peer[cf] = 0;
            }
            g_timerfd[cf] = 0;
            g_inotify[cf] = 0;
            g_lo_port[cf] = 0;
            g_sock_stream[cf] = 0;
            g_fd_cport[cf] = 0;
            g_ovldir[cf][0] = 0;
        }
        ret = (uint64_t)close(cf);
        break;
    }
    case 8: ret = (uint64_t)lseek((int)a0, (off_t)a1, (int)a2); break; // lseek
    case 9: {                                                          // mmap
        // All guest memory = anon RW (we translate code, never run guest pages, and
        // treat mprotect as a no-op). File content is pread in (no file mmap). MAP_FIXED
        // within a region we own is emulated in-place (no macOS split -> no SIGBUS).
        int anon = (int)a3 & 0x20, fixed = (int)a3 & 0x10;
        if (fixed && region_has(a0, a0 + a1)) {
            if (!anon)
                pread((int)a4, (void *)a0, (size_t)a1, (off_t)a5); // file segment -> copy into region
            else if (a2 & 0x2)
                memset((void *)a0, 0, (size_t)a1); // anon writable -> zero
            if (g_trace || g_diag)
                fprintf(stderr, "[mmap] FIXED-in-region %llx len=%llx (emulated, %s)\n", (unsigned long long)a0,
                        (unsigned long long)a1, anon ? "anon" : "file");
            ret = a0;
            break;
        }
        // Non-fixed mmaps get a 64KB guard tail mapped RW so glibc's vectorized
        // string ops (16-byte SSE over-reads past the logical end) land in mapped
        // zero memory instead of an unmapped page (Darwin SIGBUS).
        size_t guard = fixed ? 0 : 0x10000;
        void *r = mmap((void *)(fixed ? a0 : 0), (size_t)a1 + guard, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | (fixed ? MAP_FIXED : 0), -1, 0);
        if (r == MAP_FAILED) {
            if (g_trace || g_diag)
                fprintf(stderr, "[mmap] FAILED addr=%llx len=%llx flags=%llx fixed=%d errno=%d\n",
                        (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a3, fixed, errno);
            ret = (uint64_t)(-errno);
            break;
        }
        if (!anon) {
            ssize_t k = pread((int)a4, r, (size_t)a1, (off_t)a5);
            (void)k;
        } // file-backed: read content
        region_add((uint64_t)r, (uint64_t)r + a1 + guard);
        if (g_nochain && anon && a1 == 0x2000) { // DEBUG (WATCH): auto-arm byte-watchpoint on 2nd small group's slot-12
                                                 // IB (no guest perturbation)
            static int n2k;
            if (++n2k == 2) {
                g_w8 = (uint8_t *)((uint64_t)r + 0x1ad);
                g_w8v = *g_w8;
                fprintf(stderr, "[w8] auto-armed @%p = %02x (group %p)\n", (void *)g_w8, g_w8v, r);
            }
        }
        if (g_trace || g_diag)
            fprintf(stderr, "[mmap] addr=%llx len=%llx flags=%llx fd=%lld off=%llx -> %p (%s)\n",
                    (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a3, (long long)(int)a4,
                    (unsigned long long)a5, r, anon ? "anon" : "file");
        ret = (uint64_t)r;
        break;
    }
    case 10: ret = 0; break; // mprotect: NO-OP (can't split anon maps here; we don't enforce guest prot)
    case 11:
        if (a0 >= brk_lo && a0 < brk_hi) {
            ret = 0;
            break;
        } // munmap: skip within arena
        ret = (uint64_t)munmap((void *)a0, (size_t)a1);
        break;
    case 25: {
        void *r = mmap(0, (size_t)a2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1,
                       0); // mremap(old,oldsz,newsz,..) -> copy+grow
        if (r == MAP_FAILED) {
            ret = (uint64_t)(-errno);
            break;
        }
        size_t old = (size_t)a1, n = old < (size_t)a2 ? old : (size_t)a2;
        memcpy(r, (void *)a0, n);
        ret = (uint64_t)r;
        break;
    }
    case 12: { // brk: report a fixed, non-growable break so musl/glibc use their mmap-based
        // allocator path (avoids building a brk heap inside our arena, which the
        // guest then mmap/mprotects -- impossible to split on this macOS VM).
        ret = brk_lo;
        if (g_trace)
            fprintf(stderr, "[brk] %llx -> %llx (non-growable)\n", (unsigned long long)a0, (unsigned long long)brk_lo);
        break;
    }
    case 16: ret = (uint64_t)(-25); break; // ioctl -> ENOTTY
    case 5: {
        struct stat s;
        if (fstat((int)a0, &s) < 0) {
            ret = (uint64_t)(-errno);
            break;
        } // fstat
        fill_linux_stat((uint8_t *)a1, &s);
        ret = 0;
        break;
    }
    case 262: {
        struct stat s;
        char pb[4200]; // newfstatat
        const char *raw = (const char *)a1, *p = atpath(raw, pb, sizeof pb);
        int r = fstatat(ATFD(a0), p, &s, (a3 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0);
        if (r < 0) {
            ret = (uint64_t)(-errno);
            break;
        }
        fill_linux_stat((uint8_t *)a2, &s);
        ret = 0;
        break;
    }
    case 4: {
        struct stat s;
        char pb[4200];
        const char *p = atpath((const char *)a0, pb, sizeof pb); // stat
        int r = stat(p, &s);
        if (r < 0) {
            ret = (uint64_t)(-errno);
            break;
        }
        fill_linux_stat((uint8_t *)a1, &s);
        ret = 0;
        break;
    }
    case 158: {
        int code = (int)a0; // arch_prctl
        if (code == 0x1002) {
            c->fs_base = a1;
            ret = 0;
        } // ARCH_SET_FS
        else if (code == 0x1001) {
            c->gs_base = a1;
            ret = 0;
        } // ARCH_SET_GS
        else if (code == 0x1003) {
            *(uint64_t *)a1 = c->fs_base;
            ret = 0;
        } // ARCH_GET_FS
        else if (code == 0x1004) {
            *(uint64_t *)a1 = c->gs_base;
            ret = 0;
        } // ARCH_GET_GS
        else
            ret = (uint64_t)(-22);
        break;
    }
    case 218: ret = (uint64_t)getpid(); break; // set_tid_address
    case 273: ret = 0; break;                  // set_robust_list
    case 228: {
        struct timespec ts;
        clock_gettime((clockid_t)a0, &ts); // clock_gettime
        uint64_t *g = (uint64_t *)a1;
        if (g) {
            g[0] = ts.tv_sec;
            g[1] = ts.tv_nsec;
        }
        ret = 0;
        break;
    }
    case 96: {
        struct timeval tv;
        gettimeofday(&tv, 0);
        uint64_t *g = (uint64_t *)a0; // gettimeofday
        if (g) {
            g[0] = tv.tv_sec;
            g[1] = tv.tv_usec;
        }
        ret = 0;
        break;
    }
    case 318:
        arc4random_buf((void *)a0, (size_t)a1);
        ret = a1;
        break; // getrandom
    case 63: {
        char *u = (char *)a0;
        memset(u, 0, 6 * 65); // uname
        strcpy(u, "Linux");
        strcpy(u + 65, "jit86");
        strcpy(u + 130, "6.1.0");
        strcpy(u + 195, "#1 jit86");
        strcpy(u + 260, "x86_64");
        ret = 0;
        break;
    }
    case 79: {
        size_t need = strlen(g_cwd) + 1; // getcwd -> guest cwd
        if ((size_t)a1 < need) {
            ret = (uint64_t)(-ERANGE);
            break;
        }
        memcpy((char *)a0, g_cwd, need);
        ret = need;
        break;
    }
    case 80: {
        char pb[4200];
        const char *p = xlate((const char *)a0, pb, sizeof pb); // chdir
        if (chdir(p) != 0) {
            ret = (uint64_t)(-errno);
            break;
        }
        char real[4200]; // re-derive canonical guest cwd
        if (getcwd(real, sizeof real)) {
            size_t rl = g_rootfs ? strlen(g_rootfs) : 0;
            if (rl && strncmp(real, g_rootfs, rl) == 0) {
                const char *g = real + rl;
                snprintf(g_cwd, sizeof g_cwd, "%s", g[0] ? g : "/");
            } else
                snprintf(g_cwd, sizeof g_cwd, "%s", real);
        }
        ret = 0;
        break;
    }
    case 81: {
        if (fchdir((int)a0) != 0) {
            ret = (uint64_t)(-errno);
            break;
        } // fchdir
        char real[4200];
        if (getcwd(real, sizeof real)) {
            size_t rl = g_rootfs ? strlen(g_rootfs) : 0;
            if (rl && strncmp(real, g_rootfs, rl) == 0) {
                const char *g = real + rl;
                snprintf(g_cwd, sizeof g_cwd, "%s", g[0] ? g : "/");
            } else
                snprintf(g_cwd, sizeof g_cwd, "%s", real);
        }
        ret = 0;
        break;
    }
    case 89: {
        char pb[4200];
        const char *p = xlate((const char *)a0, pb, sizeof pb); // readlink
        ssize_t r = readlink(p, (char *)a1, (size_t)a2);
        ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 267: {
        char pb[4200];
        const char *raw = (const char *)a1, *p = atpath(raw, pb, sizeof pb); // readlinkat
        if (raw && strstr(raw, "/proc/self/exe")) {
            char rp[1024];
            if (!realpath(g_exe_path, rp)) strncpy(rp, g_exe_path, sizeof rp - 1);
            size_t l = strlen(rp);
            if (l > a3) l = (size_t)a3;
            memcpy((void *)a2, rp, l);
            ret = l;
            break;
        }
        ssize_t r = readlink(p, (char *)a2, (size_t)a3);
        ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // ---- filesystem mutation (all path args go through the rootfs jail) ----
    case 87: {
        const char *raw = (const char *)a0; // unlink
        if (g_nlower && raw && raw[0] == '/') {
            overlay_whiteout(raw);
            ret = 0;
            break;
        } // OVERLAY: drop upper + .wh marker
        char pb[4200];
        const char *p = xlate(raw, pb, sizeof pb);
        ret = unlink(p) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 84: {
        char pb[4200];
        const char *p = xlate((const char *)a0, pb, sizeof pb);
        ret = rmdir(p) < 0 ? (uint64_t)(-errno) : 0;
        break;
    } // rmdir
    case 83: {
        char pb[4200];
        const char *p = xlate((const char *)a0, pb, sizeof pb);
        ret = mkdir(p, (mode_t)a1) < 0 ? (uint64_t)(-errno) : 0;
        break;
    } // mkdir
    case 82: {
        char pb[4200], pb2[4200];
        const char *o = xlate((const char *)a0, pb, sizeof pb);
        char b2[4200];
        const char *n = xlate((const char *)a1, b2, sizeof b2);
        (void)pb2;
        ret = rename(o, n) < 0 ? (uint64_t)(-errno) : 0;
        break;
    } // rename
    case 86: {
        char pb[4200], b2[4200];
        const char *o = xlate((const char *)a0, pb, sizeof pb), *n = xlate((const char *)a1, b2, sizeof b2);
        ret = link(o, n) < 0 ? (uint64_t)(-errno) : 0;
        break;
    } // link
    case 88: {
        char b2[4200];
        const char *t = (const char *)a0, *l = xlate((const char *)a1, b2, sizeof b2);
        ret = symlink(t, l) < 0 ? (uint64_t)(-errno) : 0;
        break;
    } // symlink (target kept verbatim)
    case 90: {
        char pb[4200];
        const char *p = xlate((const char *)a0, pb, sizeof pb);
        ret = chmod(p, (mode_t)a1) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }                                                                               // chmod
    case 91: ret = fchmod((int)a0, (mode_t)a1) < 0 ? (uint64_t)(-errno) : 0; break; // fchmod
    case 92: {
        char pb[4200];
        const char *p = xlate((const char *)a0, pb, sizeof pb);
        ret = chown(p, (uid_t)a1, (gid_t)a2) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }                                                                                         // chown
    case 93: ret = fchown((int)a0, (uid_t)a1, (gid_t)a2) < 0 ? (uint64_t)(-errno) : 0; break; // fchown
    case 94: {
        char pb[4200];
        const char *p = xlate((const char *)a0, pb, sizeof pb);
        ret = lchown(p, (uid_t)a1, (gid_t)a2) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }                                                  // lchown
    case 95: ret = (uint64_t)umask((mode_t)a0); break; // umask
    case 76: {
        char pb[4200];
        const char *p = xlate((const char *)a0, pb, sizeof pb);
        ret = truncate(p, (off_t)a1) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }                                                                                 // truncate
    case 77: ret = ftruncate((int)a0, (off_t)a1) < 0 ? (uint64_t)(-errno) : 0; break; // ftruncate
    case 161: {
        char pb[4200];
        const char *p = xlate((const char *)a0, pb, sizeof pb);
        (void)p;
        ret = 0;
        break;
    } // chroot: jail already confines; no-op
    case 263: {
        const char *raw = (const char *)a1; // unlinkat (AT_REMOVEDIR=0x200)
        if (g_nlower && raw && raw[0] == '/') {
            overlay_whiteout(raw);
            ret = 0;
            break;
        } // OVERLAY whiteout
        char pb[4200];
        const char *p = atpath(raw, pb, sizeof pb);
        ret = ((a2 & 0x200) ? rmdir(p) : unlink(p)) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 258: {
        char pb[4200];
        const char *p = atpath((const char *)a1, pb, sizeof pb);
        ret = mkdir(p, (mode_t)a2) < 0 ? (uint64_t)(-errno) : 0;
        break;
    } // mkdirat
    case 264:
    case 316: {
        char pb[4200], b2[4200]; // renameat / renameat2
        int od = (nr == 316) ? (int)a0 : (int)a0, nd = (nr == 316) ? (int)a2 : (int)a2;
        const char *o = atpath((const char *)a1, pb, sizeof pb), *n = atpath((const char *)a3, b2, sizeof b2);
        (void)od;
        (void)nd;
        ret = rename(o, n) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 265: {
        char pb[4200], b2[4200];
        const char *o = atpath((const char *)a1, pb, sizeof pb), *n = atpath((const char *)a3, b2, sizeof b2);
        ret = link(o, n) < 0 ? (uint64_t)(-errno) : 0;
        break;
    } // linkat
    case 266: {
        char b2[4200];
        const char *t = (const char *)a0, *l = atpath((const char *)a2, b2, sizeof b2);
        ret = symlink(t, l) < 0 ? (uint64_t)(-errno) : 0;
        break;
    } // symlinkat
    case 268: {
        char pb[4200];
        const char *p = atpath((const char *)a1, pb, sizeof pb);
        ret = fchmodat(AT_FDCWD, p, (mode_t)a2, 0) < 0 ? (uint64_t)(-errno) : 0;
        break;
    } // fchmodat
    case 260: {
        char pb[4200];
        const char *p = atpath((const char *)a1, pb, sizeof pb);
        ret = fchownat(AT_FDCWD, p, (uid_t)a2, (gid_t)a3, (a4 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0) < 0
                  ? (uint64_t)(-errno)
                  : 0;
        break;
    } // fchownat
    case 285: {
        struct stat st;
        if (fstat((int)a0, &st) == 0 && st.st_size < (off_t)(a2 + a3))
            ret = ftruncate((int)a0, (off_t)(a2 + a3)) < 0 ? (uint64_t)(-errno) : 0;
        else
            ret = 0;
        break;
    } // fallocate (extend only)
    case 40: {
        off_t off = a2 ? *(off_t *)a2 : 0;
        ssize_t r = 0;
        char buf[65536];
        size_t left = a3; // sendfile
        if (a2) lseek((int)a1, off, SEEK_SET);
        while (left) {
            ssize_t k = read((int)a1, buf, left < sizeof buf ? left : sizeof buf);
            if (k <= 0) break;
            ssize_t w = write((int)a0, buf, k);
            if (w < 0) {
                r = -1;
                break;
            }
            r += w;
            left -= w;
            if (w < k) break;
        }
        if (a2 && r > 0) *(off_t *)a2 = off + r;
        ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 326: {
        int fin = (int)a0, fout = (int)a2;
        off_t *oin = (off_t *)a1, *oout = (off_t *)a3;
        size_t len = (size_t)a4; // copy_file_range
        char buf[65536];
        size_t done = 0;
        ssize_t err = 0;
        if (oin) lseek(fin, *oin, SEEK_SET);
        if (oout) lseek(fout, *oout, SEEK_SET);
        while (done < len) {
            size_t want = len - done;
            if (want > sizeof buf) want = sizeof buf;
            ssize_t k = read(fin, buf, want);
            if (k < 0) {
                err = done ? 0 : -1;
                break;
            }
            if (k == 0) break;
            ssize_t w = write(fout, buf, k);
            if (w < 0) {
                err = done ? 0 : -1;
                break;
            }
            done += w;
            if (w < k) break;
        }
        if (oin) *oin += done;
        if (oout) *oout += done;
        ret = err < 0 ? (uint64_t)(-errno) : (uint64_t)done;
        break;
    }
    case 280: {
        if (a1) {
            char pb[4200];
            const char *p = atpath((const char *)a1, pb, sizeof pb); // utimensat
            ret = utimensat(AT_FDCWD, p, (struct timespec *)a2, (a3 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0) < 0
                      ? (uint64_t)(-errno)
                      : 0;
        } else
            ret = futimens((int)a0, (struct timespec *)a2) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 235: {
        char pb[4200];
        const char *p = xlate((const char *)a0, pb, sizeof pb); // utimes(path, tv[2])
        ret = utimes(p, (struct timeval *)a1) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 261: {
        char pb[4200];
        const char *p = atpath((const char *)a1, pb, sizeof pb); // futimesat(dfd, path, tv)
        ret = utimes(p, (struct timeval *)a2) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 59: { // execve -> re-exec this jit86 binary on the new guest program (process replace)
        const char *gp = (const char *)a0;
        char **gargv = (char **)a1, **genvp = (char **)a2;
        char chk[4200];
        const char *rp = xlate(gp, chk, sizeof chk);
        if (access(rp, F_OK) != 0) {
            ret = (uint64_t)(-errno);
            break;
        }
        int gc = 0;
        while (gargv && gargv[gc])
            gc++;
        char **hv = (char **)malloc((gc + 6 + g_nvols * 2 + g_nportmap * 2 + g_nlower * 2) * sizeof *hv);
        int n = 0;
        hv[n++] = (char *)g_self_path;
        if (g_rootfs) {
            hv[n++] = (char *)"--rootfs";
            hv[n++] = (char *)g_rootfs;
        }
        for (int vi = 0; vi < g_nvols; vi++) {
            static char vspec[32][1300];
            snprintf(vspec[vi], sizeof vspec[vi], "%s:%s", g_vols[vi].guest, g_vols[vi].host);
            hv[n++] = (char *)"--vol";
            hv[n++] = vspec[vi];
        }
        for (int pi = 0; pi < g_nportmap; pi++) {
            static char pspec[32][16];
            snprintf(pspec[pi], sizeof pspec[pi], "%u:%u", g_portmap[pi].hport, g_portmap[pi].cport);
            hv[n++] = (char *)"-p";
            hv[n++] = pspec[pi];
        }
        for (int li = 0; li < g_nlower; li++) {
            hv[n++] = (char *)"--lower";
            hv[n++] = g_lower[li].root;
        }
        hv[n++] = (char *)gp;
        for (int i = 1; i < gc; i++)
            hv[n++] = gargv[i];
        hv[n] = NULL;
        execve(g_self_path, hv, genvp); // replaces the process; returns only on error
        free(hv);
        ret = (uint64_t)(-errno);
        break;
    }
    case 284:
    case 290: {
        int fds[2];
        if (pipe(fds) < 0) {
            ret = (uint64_t)(-errno);
            break;
        } // eventfd/eventfd2(initval,flags)->pipe
        int flags = (nr == 290) ? (int)a1 : 0;
        if (flags & 0x800) {
            fcntl(fds[0], F_SETFL, O_NONBLOCK);
            fcntl(fds[1], F_SETFL, O_NONBLOCK);
        }
        if (flags & 0x80000) {
            fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        }
        if (a0) {
            uint64_t v = a0;
            if (write(fds[1], &v, 8) < 0) {}
        }
        if (fds[0] < 1024 && fds[1] < 1024) g_eventfd_peer[fds[0]] = fds[1] + 1;
        ret = (uint64_t)fds[0];
        break;
    }
    case 282:
    case 289: { // signalfd(fd,mask,sizemask) / signalfd4(fd,mask,sizemask,flags)
        uint64_t lm = a1 ? *(uint64_t *)a1 : 0, pm = 0; // sigset bit (signo-1) -> g_pending bit signo
        for (int s = 1; s < 64; s++)
            if (lm & (1ull << (s - 1))) pm |= (1ull << s);
        if (g_sigfd_pipe[0] < 0 && pipe(g_sigfd_pipe) < 0) {
            ret = (uint64_t)(-errno);
            break;
        }
        g_sigfd_mask |= pm;
        g_sigfd_read = g_sigfd_pipe[0];
        for (int s = 1; s < 64; s++)
            if ((pm & (1ull << s)) && !sig_is_sync(s)) { // ensure the host delivers them
                struct sigaction sa;
                memset(&sa, 0, sizeof sa);
                sa.sa_handler = host_sigh;
                sigaction(sig_l2m(s), &sa, NULL);
            }
        if (nr == 289) {
            if (a3 & 0x80000) fcntl(g_sigfd_pipe[0], F_SETFD, FD_CLOEXEC);
            if (a3 & 0x800) fcntl(g_sigfd_pipe[0], F_SETFL, O_NONBLOCK);
        }
        ret = (uint64_t)g_sigfd_pipe[0];
        break;
    }
    case 253:
    case 294: {
        int r = kqueue();
        if (r < 0) {
            ret = (uint64_t)(-errno);
            break;
        } // inotify_init/init1 -> kqueue
        if (r < 1024) g_inotify[r] = 1;
        if (nr == 294) {
            if ((int)a0 & 0x800) fcntl(r, F_SETFL, O_NONBLOCK);
            if ((int)a0 & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC);
        }
        ret = (uint64_t)r;
        break;
    }
    case 254: {
        char pb[4200];
        const char *p = atpath((const char *)a1, pb, sizeof pb); // inotify_add_watch(fd,path,mask)
        int wfd = open(p, O_EVTONLY);
        if (wfd < 0) {
            ret = (uint64_t)(-derr(errno));
            break;
        }
        struct kevent kv;
        EV_SET(&kv, wfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
               NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB | NOTE_EXTEND, 0, (void *)(intptr_t)wfd);
        if (kevent((int)a0, &kv, 1, NULL, 0, NULL) < 0) {
            int e = errno;
            close(wfd);
            ret = (uint64_t)(-derr(e));
            break;
        }
        ret = (uint64_t)wfd;
        break;
    }
    case 255: {
        struct kevent kv;
        EV_SET(&kv, (int)a1, EVFILT_VNODE, EV_DELETE, 0, 0, NULL); // inotify_rm_watch(fd,wd)
        kevent((int)a0, &kv, 1, NULL, 0, NULL);
        close((int)a1);
        ret = 0;
        break;
    }
    case 283: {
        int r = kqueue();
        if (r < 0) {
            ret = (uint64_t)(-errno);
            break;
        } // timerfd_create(clockid,flags)
        if (r < 1024) g_timerfd[r] = 1;
        if ((int)a1 & 1) fcntl(r, F_SETFL, O_NONBLOCK);
        ret = (uint64_t)r;
        break;
    }
    case 286: {
        struct kevent kv;
        uint64_t iv_s = 0, iv_n = 0, vl_s = 0, vl_n = 0; // timerfd_settime(fd,flags,new,old)
        if (a2) {
            memcpy(&iv_s, (void *)a2, 8);
            memcpy(&iv_n, (void *)(a2 + 8), 8);
            memcpy(&vl_s, (void *)(a2 + 16), 8);
            memcpy(&vl_n, (void *)(a2 + 24), 8);
        }
        int64_t period =
            (iv_s || iv_n) ? (int64_t)(iv_s * 1000000000ull + iv_n) : (int64_t)(vl_s * 1000000000ull + vl_n);
        if (period <= 0) {
            EV_SET(&kv, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
            kevent((int)a0, &kv, 1, NULL, 0, NULL);
            ret = 0;
            break;
        }
        uint16_t fl = EV_ADD | ((iv_s || iv_n) ? 0 : EV_ONESHOT);
        EV_SET(&kv, 1, EVFILT_TIMER, fl, NOTE_NSECONDS, period, NULL);
        ret = kevent((int)a0, &kv, 1, NULL, 0, NULL) < 0 ? (uint64_t)(-derr(errno)) : 0;
        break;
    }
    case 287:
        if (a1) memset((void *)a1, 0, 32);
        ret = 0;
        break; // timerfd_gettime -> best-effort 0
    case 213:
    case 291: {
        int r = kqueue();
        if (r < 0) {
            ret = (uint64_t)(-errno);
            break;
        } // epoll_create/create1 -> kqueue
        if (nr == 291 && ((int)a0 & 0x80000)) fcntl(r, F_SETFD, FD_CLOEXEC);
        ret = (uint64_t)r;
        break;
    }
    case 233: {
        int op = (int)a1, fd = (int)a2;
        uint32_t ev = 0;
        uint64_t data = (uint64_t)(unsigned)fd; // epoll_ctl -> kevent
        if (a3) {
            ev = *(uint32_t *)a3;
            memcpy(&data, (void *)(a3 + 4), 8);
        }
        struct kevent kv[2];
        int n = 0;
        uint16_t base = (op == 2) ? EV_DELETE : EV_ADD;
        uint16_t xf = (uint16_t)((ev & 0x80000000u ? EV_CLEAR : 0) | (ev & 0x40000000u ? EV_ONESHOT : 0));
        if (op == 2 || (ev & 0x1)) {
            EV_SET(&kv[n], fd, EVFILT_READ, base | xf, 0, 0, (void *)data);
            n++;
        }
        if (op == 2 || (ev & 0x4)) {
            EV_SET(&kv[n], fd, EVFILT_WRITE, base | xf, 0, 0, (void *)data);
            n++;
        }
        for (int i = 0; i < n; i++)
            kevent((int)a0, &kv[i], 1, NULL, 0, NULL);
        ret = 0;
        break;
    }
    case 232:
    case 281: {
        int maxev = (int)a2;
        if (maxev > 256) maxev = 256;
        if (maxev < 0) maxev = 0; // epoll_wait/pwait -> kevent
        struct kevent kv[256];
        struct timespec ts, *tp = NULL;
        if ((int)a3 >= 0) {
            ts.tv_sec = (int)a3 / 1000;
            ts.tv_nsec = (long)((int)a3 % 1000) * 1000000L;
            tp = &ts;
        }
        int r = kevent((int)a0, NULL, 0, kv, maxev, tp);
        if (r < 0) {
            ret = (uint64_t)(-derr(errno));
            break;
        }
        uint8_t *out = (uint8_t *)a1;
        for (int i = 0; i < r; i++) {
            uint32_t ev = (kv[i].filter == EVFILT_READ) ? 0x1u : (kv[i].filter == EVFILT_WRITE) ? 0x4u : 0u;
            if (kv[i].flags & EV_EOF) ev |= 0x10u;
            if (kv[i].flags & EV_ERROR) ev |= 0x8u;
            *(uint32_t *)(out + i * 12) = ev;
            memcpy(out + i * 12 + 4, &kv[i].udata, 8);
        }
        ret = (uint64_t)r;
        break;
    }
    case 74: ret = fsync((int)a0) < 0 ? (uint64_t)(-errno) : 0; break; // fsync
    case 75: ret = fsync((int)a0) < 0 ? (uint64_t)(-errno) : 0; break; // fdatasync (no macOS fdatasync -> fsync)
    case 162:
        sync();
        ret = 0;
        break;                                          // sync
    case 24: ret = (uint64_t)sched_yield(); break;      // sched_yield
    case 124: ret = (uint64_t)getsid((pid_t)a0); break; // getsid
    case 100: {
        struct tms t;
        clock_t r = times(&t);
        if (a0) memcpy((void *)a0, &t, sizeof t);
        ret = (uint64_t)r;
        break;
    } // times
    case 62:
        if ((int)a0 == getpid() || (int)a0 <= 0) {
            raise_guest_signal(c, (int)a1);
            ret = 0;
        } // kill: self/pgrp -> guest delivery
        else
            ret = kill((pid_t)a0, sig_l2m((int)a1)) < 0 ? (uint64_t)(-errno) : 0;
        break;
    case 200:
        raise_guest_signal(c, (int)a1);
        ret = 0;
        break; // tkill(tid,sig) -> self (single-threaded model)
    case 293: {
        int fds[2];
        if (pipe(fds) < 0) {
            ret = (uint64_t)(-errno);
            break;
        } // pipe2
        if ((int)a1 & 0x800) {
            fcntl(fds[0], F_SETFL, O_NONBLOCK);
            fcntl(fds[1], F_SETFL, O_NONBLOCK);
        }
        if ((int)a1 & 0x80000) {
            fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        }
        ((int *)a0)[0] = fds[0];
        ((int *)a0)[1] = fds[1];
        ret = 0;
        break;
    }
    case 230: {
        struct timespec *req = (struct timespec *)a2, rel; // clock_nanosleep
        if ((int)a1 & 1) {
            struct timespec now;
            clock_gettime((int)a0 == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &now);
            rel.tv_sec = req->tv_sec - now.tv_sec;
            rel.tv_nsec = req->tv_nsec - now.tv_nsec;
            if (rel.tv_nsec < 0) {
                rel.tv_sec--;
                rel.tv_nsec += 1000000000;
            }
            if (rel.tv_sec < 0) {
                rel.tv_sec = 0;
                rel.tv_nsec = 0;
            }
            req = &rel;
        }
        ret = nanosleep(req, NULL) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 271: {
        int to = -1;
        if (a2) {
            struct timespec *ts = (struct timespec *)a2;
            to = (int)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000);
        } // ppoll
        int r = poll((struct pollfd *)a0, (nfds_t)a1, to);
        ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 274: ret = (uint64_t)(-38); break; // get_robust_list -> ENOSYS
    case 118:
        if (a0) *(uint32_t *)a0 = 0;
        if (a1) *(uint32_t *)a1 = 0;
        if (a2) *(uint32_t *)a2 = 0;
        ret = 0;
        break; // getresuid -> root
    case 120:
        if (a0) *(uint32_t *)a0 = 0;
        if (a1) *(uint32_t *)a1 = 0;
        if (a2) *(uint32_t *)a2 = 0;
        ret = 0;
        break;                 // getresgid -> root
    case 141: ret = 0; break;  // setpriority -> ok (ignore)
    case 140: ret = 20; break; // getpriority -> nice 0 (kernel encoding 20-nice)
    case 157: ret = 0; break;  // prctl -> ok (PR_SET_NAME etc. ignored)
    case 131: ret = 0; break;  // sigaltstack -> ok (layout differs; stub)
    case 72: {
        int r = fcntl((int)a0, (int)a1, a2);
        ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }                                                       // fcntl
    case 32: ret = (uint64_t)dup((int)a0); break;           // dup
    case 33: ret = (uint64_t)dup2((int)a0, (int)a1); break; // dup2
    case 234:
        raise_guest_signal(c, (int)a2);
        ret = 0;
        break; // tgkill(tgid,tid,sig) -> self (single-threaded model)
    case 292: {
        int r = dup2((int)a0, (int)a1);
        if (r >= 0 && (a2 & 0x80000)) fcntl((int)a1, F_SETFD, FD_CLOEXEC);
        ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    } // dup3 (O_CLOEXEC=0x80000)
    case 21: {
        char pb[4200];
        const char *p = atpath((const char *)a0, pb, sizeof pb); // access
        int r = access(p, (int)a1);
        ret = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 269: {
        char pb[4200];
        const char *p = atpath((const char *)a1, pb, sizeof pb); // faccessat
        int r = faccessat(ATFD(a0), p, (int)a2, 0);
        ret = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 22: {
        int fds[2];
        if (pipe(fds) < 0) {
            ret = (uint64_t)(-errno);
            break;
        } // pipe
        ((int *)a0)[0] = fds[0];
        ((int *)a0)[1] = fds[1];
        ret = 0;
        break;
    }
    case 13: { // rt_sigaction(sig, *act, *old) -- x86-64 sigaction: handler@0,flags@8,restorer@16,mask@24
        int sig = (int)a0;
        if (sig < 1 || sig > 64) {
            ret = (uint64_t)(-22);
            break;
        }
        if (a2) {
            *(uint64_t *)(a2 + 0) = g_sigact[sig].handler;
            *(uint64_t *)(a2 + 8) = g_sigact[sig].flags;
            *(uint64_t *)(a2 + 24) = g_sigact[sig].mask;
        }
        if (a1) {
            uint64_t h = *(uint64_t *)(a1 + 0);
            g_sigact[sig].handler = h;
            g_sigact[sig].flags = *(uint64_t *)(a1 + 8);
            g_sigact[sig].mask = *(uint64_t *)(a1 + 24);
            if (sig != 9 && sig != 19) {
                int ms = sig_l2m(sig); // SIGKILL/SIGSTOP can't be caught
                if (h == 0)
                    signal(ms, SIG_DFL);
                else if (h == 1)
                    signal(ms, SIG_IGN);
                else if (!sig_is_sync(sig)) {
                    struct sigaction sa;
                    memset(&sa, 0, sizeof sa); // async: host flags pending, dispatcher delivers
                    sa.sa_handler = host_sigh;
                    sigfillset(&sa.sa_mask);
                    sigaction(ms, &sa, NULL);
                }
            }
        }
        ret = 0;
        break;
    }
    case 14: { // rt_sigprocmask(how, *set, *old)
        if (a2) *(uint64_t *)a2 = c->sigmask;
        if (a1) {
            uint64_t set = *(uint64_t *)a1;
            if (a0 == 0)
                c->sigmask |= set;
            else if (a0 == 1)
                c->sigmask &= ~set;
            else
                c->sigmask = set;
        } // BLOCK/UNBLOCK/SETMASK
        ret = 0;
        break;
    }
    case 15:
        do_sigreturn(c);
        c->redirect = 1;
        break; // rt_sigreturn (restorer path)
    case 127: {
        uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST), out = 0; // rt_sigpending
        for (int s = 1; s <= 64; s++)
            if (p & (1ull << s)) out |= (1ull << (s - 1));
        if (a0) *(uint64_t *)a0 = out;
        ret = 0;
        break;
    }
    case 105:
    case 106:
    case 113:
    case 114:
    case 117:
    case 119: ret = 0; break;                 // setuid/setgid/setreuid/setregid/setresuid/setresgid -> ok
    case 39: ret = (uint64_t)getpid(); break; // getpid
    case 102:
    case 104:
    case 107:
    case 108: ret = 0; break; // getuid/getgid/geteuid/getegid -> container root (docker model)
    case 115: {               // getgroups -> root's supplementary groups (from rootfs /etc/group)
        extern int g_ngroups, g_groups[];
        extern void build_root_groups(void);
        build_root_groups();
        if (a0 == 0) {
            ret = g_ngroups;
            break;
        } // size query
        if ((int)a0 < g_ngroups) {
            ret = (uint64_t)(-EINVAL);
            break;
        }
        for (int i = 0; i < g_ngroups; i++)
            ((uint32_t *)a1)[i] = g_groups[i];
        ret = g_ngroups;
        break;
    }
    case 116: ret = 0; break; // setgroups -> ok (no-op, container root)
    case 110: ret = (uint64_t)getppid(); break;
    case 186: ret = (uint64_t)getpid(); break; // gettid
    case 202: ret = 0; break;                  // futex (stub: single-thread)
    case 217: {                                // getdents64
        int gfd = (int)a0;
        if (g_nlower && gfd >= 0 && gfd < 1024 && g_ovldir[gfd][0]) { // OVERLAY: merged listing across layers
            static char onames[2048][256];
            static uint8_t otypes[2048];
            int ocnt = overlay_readdir(g_ovldir[gfd], onames, otypes, 2048); // stable order; persistent per-fd cursor
            int cur = g_ovlcur[gfd];
            uint8_t *out = (uint8_t *)a1;
            size_t o = 0;
            while (cur < ocnt) {
                size_t nl = strlen(onames[cur]), lr = (19 + nl + 1 + 7) & ~7ull;
                if (o + lr > (size_t)a2) break;
                uint8_t *ld = out + o;
                *(uint64_t *)(ld + 0) = (uint64_t)(cur + 1);
                *(uint64_t *)(ld + 8) = o + lr;
                *(uint16_t *)(ld + 16) = (uint16_t)lr;
                *(ld + 18) = otypes[cur];
                memcpy(ld + 19, onames[cur], nl);
                ld[19 + nl] = 0;
                o += lr;
                cur++;
            }
            g_ovlcur[gfd] = cur; // remember progress; returns 0 when exhausted -> EOF
            ret = o;
            break;
        }
        static struct {
            int fd;
            DIR *d;
        } dirs[64];
        static int ndirs;
        int fd = (int)a0;
        DIR *dir = NULL;
        for (int i = 0; i < ndirs; i++)
            if (dirs[i].fd == fd) {
                dir = dirs[i].d;
                break;
            }
        if (!dir) {
            dir = fdopendir(dup(fd));
            if (!dir) {
                ret = (uint64_t)(-errno);
                break;
            }
            if (ndirs < 64) {
                dirs[ndirs].fd = fd;
                dirs[ndirs].d = dir;
                ndirs++;
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
        ret = o;
        break;
    }
    case 28: ret = 0; break;                                        // madvise
    case 302: ret = 0; break;                                       // prlimit64 (stub)
    case 334: ret = (uint64_t)(-38); break;                         // rseq -> ENOSYS (glibc falls back)
    case 111: ret = (uint64_t)getpgrp(); break;                     // getpgrp
    case 121: ret = (uint64_t)getpgid((pid_t)a0); break;            // getpgid
    case 109: ret = (uint64_t)setpgid((pid_t)a0, (pid_t)a1); break; // setpgid
    case 112: ret = (uint64_t)setsid(); break;                      // setsid
    case 57:
    case 58:   // fork / vfork
    case 56: { // clone (process fork; threads unsupported)
        if (nr == 56 && (a0 & 0x100)) {
            ret = (uint64_t)(-38);
            break;
        }                 // CLONE_VM (thread) -> ENOSYS
        pid_t p = fork(); // host fork: COW-dup guest mem + JIT cache
        if (p < 0) {
            ret = (uint64_t)(-errno);
            break;
        }
        if (nr == 56) {                                                   // clone tid hand-back
            if (p > 0 && (a0 & 0x100000) && a2) *(int *)a2 = p;           // CLONE_PARENT_SETTID
            if (p == 0 && (a0 & 0x01000000) && a3) *(int *)a3 = getpid(); // CLONE_CHILD_SETTID
        }
        ret = (uint64_t)p;
        break; // parent: child pid; child: 0
    }
    case 61: {
        int st;
        pid_t p = wait4((pid_t)a0, a1 ? &st : NULL, (int)a2, NULL); // wait4
        if (a1) *(int *)a1 = st;
        ret = p < 0 ? (uint64_t)(-errno) : (uint64_t)p;
        break;
    }
    case 221: ret = 0; break; // fadvise64 -> ok (ignore)
    case 41: {
        int dom = (int)a0 == 10 ? AF_INET6 : (int)a0; // socket(domain,type,protocol)
        int r = socket(dom, socktype_l2d((int)a1), (int)a2);
        if (r < 0) {
            ret = (uint64_t)(-errno);
            break;
        }
        if ((int)a1 & 0x800) fcntl(r, F_SETFL, fcntl(r, F_GETFL, 0) | O_NONBLOCK); // SOCK_NONBLOCK
        if ((int)a1 & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC);                      // SOCK_CLOEXEC
        if (r < 1024) {
            g_sock_stream[r] = ((int)a0 == AF_INET && ((int)a1 & 0xff) == SOCK_STREAM);
            g_lo_port[r] = 0;
        }
        ret = (uint64_t)r;
        break;
    }
    case 49: {
        uint8_t *sa = (uint8_t *)a1; // bind
        if (lo_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] &&
            lo_is(sa, (socklen_t)a2)) { // private loopback
            uint16_t p = ntohs(*(uint16_t *)(sa + 2));
            char up[200];
            lo_path(p, up, sizeof up);
            if (lo_swap((int)a0) < 0) {
                ret = (uint64_t)(-errno);
                break;
            }
            unlink(up);
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            un.sun_len = sizeof un;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = bind((int)a0, (struct sockaddr *)&un, sizeof un);
            if (r == 0) g_lo_port[(int)a0] = p ? p : 1;
            ret = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        struct sockaddr_storage ss;
        socklen_t l = l2d_sa((void *)a1, (socklen_t)a2, &ss);
        if (g_nportmap && sa && a2 >= 8 && *(uint16_t *)sa == AF_INET) { // docker -p: bind the published host port :H
            uint16_t cp = ntohs(*(uint16_t *)(sa + 2));
            if ((int)a0 >= 0 && (int)a0 < 1024) g_fd_cport[(int)a0] = cp; // remember :C for getsockname
            if (l) ((struct sockaddr_in *)&ss)->sin_port = htons(pm_host(cp));
        }
        ret =
            bind((int)a0, l ? (struct sockaddr *)&ss : (void *)a1, l ? l : (socklen_t)a2) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 42: {
        uint8_t *sa = (uint8_t *)a1; // connect
        if (lo_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] &&
            lo_is(sa, (socklen_t)a2)) { // private loopback
            uint16_t p = ntohs(*(uint16_t *)(sa + 2));
            char up[200];
            lo_path(p, up, sizeof up);
            if (lo_swap((int)a0) < 0) {
                ret = (uint64_t)(-errno);
                break;
            }
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            un.sun_len = sizeof un;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = connect((int)a0, (struct sockaddr *)&un, sizeof un);
            if (r == 0 || errno == EINPROGRESS) g_lo_port[(int)a0] = p ? p : 1;
            ret = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        struct sockaddr_storage ss;
        socklen_t l = l2d_sa((void *)a1, (socklen_t)a2, &ss);
        ret = connect((int)a0, l ? (struct sockaddr *)&ss : (void *)a1, l ? l : (socklen_t)a2) < 0 ? (uint64_t)(-errno)
                                                                                                   : 0;
        break;
    }
    case 50: ret = listen((int)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0; break; // listen
    case 43:
    case 288: {
        int lfd = (int)a0;
        int pl = (lfd >= 0 && lfd < 1024) ? g_lo_port[lfd] : 0; // accept / accept4
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int r = pl ? accept(lfd, NULL, NULL) : accept(lfd, (struct sockaddr *)&ss, &sl);
        if (r < 0) {
            ret = (uint64_t)(-errno);
            break;
        }
        if (nr == 288) {
            if ((int)a3 & 0x800) fcntl(r, F_SETFL, fcntl(r, F_GETFL, 0) | O_NONBLOCK);
            if ((int)a3 & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC);
        }
        if (pl) {
            if (r < 1024) {
                g_lo_port[r] = pl;
                g_sock_stream[r] = 1;
            }
            fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, pl);
        } // peer = 127.0.0.1:lport
        else if (a1 && a2) {
            socklen_t gl = *(socklen_t *)a2;
            d2l_sa((struct sockaddr *)&ss, (void *)a1, gl, (socklen_t *)a2);
        }
        ret = (uint64_t)r;
        break;
    }
    case 51:
    case 52: {
        int fd = (int)a0; // getsockname / getpeername
        if (fd >= 0 && fd < 1024 && g_lo_port[fd]) {
            fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, g_lo_port[fd]);
            ret = 0;
            break;
        }
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss;
        int r = (nr == 51 ? getsockname : getpeername)(fd, (struct sockaddr *)&ss, &sl);
        if (r < 0) {
            ret = (uint64_t)(-errno);
            break;
        }
        if (a1 && a2) {
            socklen_t gl = *(socklen_t *)a2;
            d2l_sa((struct sockaddr *)&ss, (void *)a1, gl, (socklen_t *)a2);
        }
        if (nr == 51 && g_nportmap && a1 && fd >= 0 && fd < 1024 && g_fd_cport[fd])
            *(uint16_t *)((uint8_t *)a1 + 2) = htons(g_fd_cport[fd]); // report the :C the app asked for (port @2)
        ret = 0;
        break;
    }
    case 44: {
        struct sockaddr_storage ss;
        socklen_t l = a4 ? l2d_sa((void *)a4, (socklen_t)a5, &ss) : 0; // sendto
        ssize_t r = sendto((int)a0, (void *)a1, (size_t)a2, (int)a3, l ? (struct sockaddr *)&ss : (void *)a4, l);
        ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 45: {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof ss; // recvfrom
        ssize_t r =
            recvfrom((int)a0, (void *)a1, (size_t)a2, (int)a3, a4 ? (struct sockaddr *)&ss : NULL, a4 ? &sl : NULL);
        if (r < 0) {
            ret = (uint64_t)(-errno);
            break;
        }
        if (a4 && a5) {
            socklen_t gl = *(socklen_t *)a5;
            d2l_sa((struct sockaddr *)&ss, (void *)a4, gl, (socklen_t *)a5);
        }
        ret = (uint64_t)r;
        break;
    }
    case 48: ret = shutdown((int)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0; break; // shutdown
    case 53: {
        int sv[2];
        int r = socketpair((int)a0 == 10 ? AF_INET6 : (int)a0, socktype_l2d((int)a1), (int)a2, sv); // socketpair
        if (r < 0) {
            ret = (uint64_t)(-errno);
            break;
        }
        ((int *)a3)[0] = sv[0];
        ((int *)a3)[1] = sv[1];
        ret = 0;
        break;
    }
    case 54: {
        int lvl = (int)a1 == 1 ? 0xffff : (int)a1, opt = (int)a1 == 1 ? so_l2d((int)a2) : (int)a2; // setsockopt
        ret = setsockopt((int)a0, lvl, opt, (void *)a3, (socklen_t)a4) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 55: {
        int lvl = (int)a1 == 1 ? 0xffff : (int)a1, opt = (int)a1 == 1 ? so_l2d((int)a2) : (int)a2; // getsockopt
        ret = getsockopt((int)a0, lvl, opt, (void *)a3, (socklen_t *)a4) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 27: {
        uint8_t *v = (uint8_t *)a2;
        size_t n = ((size_t)a1 + 4095) / 4096; // mincore: report all resident
        if (v)
            for (size_t i = 0; i < n; i++)
                v[i] = 1;
        ret = 0;
        break;
    }
    case 204: {
        if (a2) *(uint64_t *)a2 = 1;
        ret = (a1 < 8) ? a1 : 8;
        break;
    } // sched_getaffinity (1 cpu)
    case 99:
        memset((void *)a0, 0, 112);
        {
            uint64_t *si = (uint64_t *)a0;
            if (si) {
                si[0] = 100;
                si[1] = 1u << 30;
                si[2] = 1u << 29;
            }
        }
        ret = 0;
        break; // sysinfo
    case 332: {
        struct stat s;
        char pb[4200]; // statx(dfd,path,flags,mask,buf)
        const char *raw = (const char *)a1, *p = atpath(raw, pb, sizeof pb);
        int empty = (raw && !raw[0] && (a2 & 0x1000)), rr;
        rr = empty ? fstat((int)a0, &s) : fstatat(ATFD(a0), p, &s, (a2 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0);
        if (rr < 0) {
            ret = (uint64_t)(-errno);
            break;
        }
        uint8_t *d = (uint8_t *)a4;
        memset(d, 0, 256); // correct statx layout
        *(uint32_t *)(d + 0) = 0x7ff;
        *(uint32_t *)(d + 4) = 4096;                         // stx_mask, stx_blksize
        *(uint32_t *)(d + 16) = s.st_nlink ? s.st_nlink : 1; // stx_nlink
        *(uint32_t *)(d + 20) = map_uid(s.st_uid);
        *(uint32_t *)(d + 24) = map_gid(s.st_gid);
        *(uint16_t *)(d + 28) = s.st_mode; // stx_mode @28 (not 26 -- would clobber gid)
        *(uint64_t *)(d + 32) = s.st_ino;
        *(uint64_t *)(d + 40) = s.st_size;
        *(uint64_t *)(d + 48) = s.st_blocks;
        *(uint64_t *)(d + 64) = s.st_atime;
        *(uint64_t *)(d + 96) = s.st_ctime;
        *(uint64_t *)(d + 112) = s.st_mtime;
        ret = 0;
        break;
    }
    case 137:
    case 138: {
        uint8_t *b = (uint8_t *)(nr == 137 ? a1 : a1);
        memset(b, 0, 120); // statfs/fstatfs
        *(uint64_t *)(b + 0) = 0x01021994;
        *(uint64_t *)(b + 8) = 4096;
        *(uint64_t *)(b + 16) = 1u << 24;
        *(uint64_t *)(b + 24) = 1u << 23;
        *(uint64_t *)(b + 32) = 1u << 23;
        *(uint64_t *)(b + 40) = 1u << 20;
        *(uint64_t *)(b + 48) = 1u << 19;
        *(uint64_t *)(b + 72) = 255;
        ret = 0;
        break;
    }
    case 60:
        c->exited = 1;
        c->exit_code = (int)a0;
        return; // exit
    case 231:   // exit_group
        if (g_prof)
            fprintf(stderr, "[prof] dispatcher round-trips=%llu  IBTC fills=%llu  (IBTC %s)\n",
                    (unsigned long long)g_disp_n, (unsigned long long)g_ibtc_fill, g_noibtc ? "OFF" : "ON");
        _exit((int)a0);
    default:
        fprintf(stderr, "[jit86] unhandled syscall %llu (a0=%llx a1=%llx) at rip=%llx\n", (unsigned long long)nr,
                (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)c->rip);
        ret = (uint64_t)(-38);
        break; // ENOSYS
    }
    // network/socket syscalls report raw host (Darwin) errno -> map to Linux for the guest
    if (((nr >= 41 && nr <= 55) || nr == 288) && (int64_t)ret < 0 && (int64_t)ret >= -134)
        ret = (uint64_t)(-derr((int)(-(int64_t)ret)));
    c->r[RAX] = ret;
    if (g_trace)
        fprintf(stderr, " = %llx%s\n", (unsigned long long)ret,
                ((int64_t)ret < 0 && (int64_t)ret > -4096) ? " [ERR]" : "");
}

// CPUID: report an x86-64 baseline CPU with SSE2 ONLY (no SSE3/AVX/BMI), so glibc/musl
// IFUNC resolvers select the SSE2/generic implementations we actually support.
static void do_cpuid(struct cpu *c) {
    uint32_t leaf = (uint32_t)c->r[RAX], a = 0, b = 0, cc = 0, d = 0;
    switch (leaf) {
    case 0:
        a = 7;
        b = 0x756e6547;
        d = 0x49656e69;
        cc = 0x6c65746e;
        break; // max-leaf=7, "GenuineIntel"
    case 1:
        a = 0x000306c3; // family/model (Haswell-ish id, harmless)
        d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 11) | (1u << 13) | (1u << 15) | (1u << 19) | (1u << 23) |
            (1u << 24) | (1u << 25) | (1u << 26); // FPU,TSC,CX8,SEP,PGE,CMOV,CLFSH,MMX,FXSR,SSE,SSE2
        cc = 0;
        break; // ecx=0: no SSE3/SSSE3/SSE4/AVX/CX16
    case 7:
        a = 0;
        b = 0;
        cc = 0;
        d = 0;
        break; // no AVX2/BMI/etc
    case 0x80000000: a = 0x80000001; break;
    case 0x80000001:
        d = (1u << 11) | (1u << 29);
        cc = (1u << 0);
        break;                          // SYSCALL, LM(64-bit), LAHF
    case 0x80000008: a = 0x3027; break; // 48-bit phys, 39-bit virt
    default: break;
    }
    c->r[RAX] = a;
    c->r[RBX] = b;
    c->r[RCX] = cc;
    c->r[RDX] = d;
}

// x87 80-bit extended <-> double conversion (done in C for reliability; libm-free).
// We emulate the ST stack at double precision, so this loses the 80-bit mantissa tail.
static void x87_fld_m80(struct cpu *c) {
    uint8_t *ea = (uint8_t *)c->x87_ea;
    uint64_t sig;
    uint16_t se;
    memcpy(&sig, ea, 8);
    memcpy(&se, ea + 8, 2);
    int s = se >> 15, e = se & 0x7fff;
    double d;
    if (sig == 0 && e == 0)
        d = 0.0;
    else {
        d = (double)sig;        // ~2^63 (ucvtf)
        int k = e - 16383 - 63; // scale exponent
        uint64_t db;
        memcpy(&db, &d, 8);
        int de = (int)((db >> 52) & 0x7ff) + k;
        if (de <= 0)
            d = 0.0;
        else if (de >= 0x7ff) {
            db = (db & (1ull << 63)) | (0x7ffull << 52);
            memcpy(&d, &db, 8);
        } else {
            db = (db & ~(0x7ffull << 52)) | ((uint64_t)de << 52);
            memcpy(&d, &db, 8);
        }
        if (s) d = -d;
    }
    c->fptop = (c->fptop - 1) & 7;
    c->st[c->fptop & 7] = d;
}
static void x87_fstp_m80(struct cpu *c) {
    uint8_t *ea = (uint8_t *)c->x87_ea;
    double d = c->st[c->fptop & 7];
    c->fptop = (c->fptop + 1) & 7;
    uint64_t db;
    memcpy(&db, &d, 8);
    int s = (int)(db >> 63), de = (int)((db >> 52) & 0x7ff);
    uint64_t dm = db & ((1ull << 52) - 1);
    uint64_t sig;
    uint16_t se;
    if (de == 0) {
        sig = 0;
        se = (uint16_t)(s ? 0x8000 : 0);
    } else {
        sig = (1ull << 63) | (dm << 11);
        int e80 = de - 1023 + 16383;
        se = (uint16_t)((s << 15) | (e80 & 0x7fff));
    }
    memcpy(ea, &sig, 8);
    memcpy(ea + 8, &se, 2);
}
