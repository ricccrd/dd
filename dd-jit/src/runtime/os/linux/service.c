// brk arena
static uint64_t brk_lo, brk_cur, brk_hi;
// dd/runtime/os/linux -- service(): the Linux syscall layer (the "kernel" the guest talks to).
// Dispatches the guest syscall number to the host, translating the ABI (errno, struct layouts, flags,
// fd semantics); every path argument is resolved through the container VFS jail. One sorted switch,
// grouped by category. See docs/SYSCALLS.md for the per-syscall table.

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/times.h> // times(2): CPU accounting (struct tms is layout-compatible with Linux)
#include <sys/mount.h> // host struct statfs -> translated to the Linux statfs layout
// macOS renamex_np/renameatx_np flags (Linux renameat2 flags map onto these)
#ifndef RENAME_SWAP
#define RENAME_SWAP 0x00000002 // atomic swap  <- Linux RENAME_EXCHANGE(2)
#endif
#ifndef RENAME_EXCL
#define RENAME_EXCL 0x00000004 // fail if dst exists <- Linux RENAME_NOREPLACE(1)
#endif
// Emulated pipe-buffer sizes for F_SETPIPE_SZ/F_GETPIPE_SZ (macOS has no pipe-size fcntl): we record
// the requested (page-rounded) size per-fd and report it back, so size-probing programs see it stick.
static int g_pipesz[1024];
// Configurable fsync durability policy (S3DB_DURABILITY=none|fast|strict), read once and cached.
//   0 = fast   (DEFAULT, and when env unset): plain fsync() -- the macOS fast path, unchanged legacy
//               behavior. NOTE: a plain fsync() only reaches the drive's write cache on macOS; this is
//               survives-process-crash, NOT survives-host-power-loss. We deliberately do NOT map
//               fsync->F_FULLFSYNC by default: a single F_FULLFSYNC is ~3 ms (160x a plain fsync) and
//               collapses sqlite commit throughput ~35-100x. Default must stay `fast`.
//   1 = none   no-op barrier (return success without syncing) -- for ephemeral/CI containers, which are
//               page-cache-coherent for any reader but NOT host-crash-durable. ~2.9x sqlite insert tps.
//   2 = strict fcntl(fd, F_FULLFSYNC) for real on-platter durability (falls back to fsync on non-reg fds).
static int s3db_durability(void) {
    static int mode = -1;
    if (__builtin_expect(mode < 0, 0)) {
        const char *e = getenv("S3DB_DURABILITY");
        int m = 0; // default == fast == legacy
        if (e) {
            if (!strcmp(e, "none")) m = 1;
            else if (!strcmp(e, "strict")) m = 2;
            else m = 0; // "fast" or anything unrecognized -> fast (safe default)
        }
        mode = m;
    }
    return mode;
}
// Route fsync/fdatasync/sync_file_range (82/83/84) through the durability policy. Returns 0 on success
// or -errno (Linux ABI convention used by the caller's G_RET). `fast`/default is byte-identical to the
// legacy `fsync((int)fd) < 0 ? -errno : 0` path.
static uint64_t s3db_sync_fd(int fd) {
    switch (s3db_durability()) {
        case 1: return 0; // none: no-op barrier
        case 2: // strict: real host-crash durability; fall back to fsync if F_FULLFSYNC unsupported
            if (fcntl(fd, F_FULLFSYNC, 0) == 0) return 0;
            // EINVAL/ENOTSUP on non-regular fds (pipes, sockets, etc.) -> plain fsync
            return fsync(fd) < 0 ? (uint64_t)(-errno) : 0;
        default: // 0 fast (== legacy default)
            return fsync(fd) < 0 ? (uint64_t)(-errno) : 0;
    }
}
// Map a Linux `semctl` cmd to the macOS one: the GET*/SET* values differ (Linux GETVAL=12/SETVAL=16,
// macOS GETVAL=5/SETVAL=8); IPC_RMID/SET/STAT (0/1/2) are the same.
static int sem_cmd_l2m(int c) {
    switch (c) {
        case 11: return 4;  case 12: return 5;  case 13: return 6;  case 14: return 3;
        case 15: return 7;  case 16: return 8;  case 17: return 9;  default: return c;
    }
}
// SysV IPC: namespace a key by the container (DD_NETNS) so two containers don't collide on the same key
// -- the per-IPC-ns isolation. IPC_PRIVATE stays private; --network host shares the host IPC.
static key_t ipc_ns_key(key_t k) {
    if (k == IPC_PRIVATE) return k;
    const char *ns = getenv("DD_NETNS");
    if (!ns || !ns[0]) return k;
    uint32_t salt = 2166136261u;
    for (const char *p = ns; *p; p++) { salt ^= (uint8_t)*p; salt = salt * 16777619u; }
    key_t hk = (key_t)((uint32_t)k ^ (salt & 0x7fffffffu));
    return hk == IPC_PRIVATE ? hk + 1 : hk;
}
// list a directory's entries (minus . / ..) as a newline-joined, NUL-terminated malloc'd string (for the
// inotify-on-a-directory diff). NULL on error.
static char *dir_snapshot(const char *path) {
    DIR *d = opendir(path);
    if (!d) return NULL;
    size_t cap = 256, len = 0;
    char *s = malloc(cap);
    if (!s) { closedir(d); return NULL; }
    s[0] = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t nl = strlen(e->d_name);
        if (len + nl + 2 > cap) { cap = (len + nl + 2) * 2; char *n = realloc(s, cap); if (!n) break; s = n; }
        memcpy(s + len, e->d_name, nl); len += nl; s[len++] = '\n'; s[len] = 0;
    }
    closedir(d);
    return s;
}
// is `name` present as a line in the newline-joined snapshot?
static int snap_has(const char *snap, const char *name, size_t nl) {
    if (!snap) return 0;
    for (const char *p = snap; *p;) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == nl && !memcmp(p, name, nl)) return 1;
        p = e ? e + 1 : p + l;
    }
    return 0;
}
static char g_procname[16]; // prctl PR_SET_NAME / PR_GET_NAME (the 15-char process/thread name)
// getdents directory-stream cache (guest fd -> host DIR*). MUST be invalidated on close(), else a reused
// fd gets a stale DIR* already at EOF (a second opendir of the same path then reads nothing -- broke glob).
static struct { int fd; DIR *d; } g_dirs[64];
static int g_ndirs;
static void dirs_drop(int fd) {
    for (int i = 0; i < g_ndirs; i++)
        if (g_dirs[i].fd == fd) { closedir(g_dirs[i].d); g_dirs[i] = g_dirs[--g_ndirs]; return; }
}
// Parse a "#!" shebang line. `host_path` is the RESOLVED HOST path of a candidate program. If the file
// begins with "#!", fills `interp` (size ni) with the interpreter path and `arg` (size na) with the
// optional single argument (arg[0]==0 when there is none) and returns 1. Returns 0 when it is not a
// shebang script, -1 when the file can't be opened/read. Shared by execve (case 221) and the initial
// program loader (jit_run): both then rewrite argv to [interp, (arg), scriptpath, args...] and load the
// INTERPRETER instead of the script. load_elf has no ELF-magic/#! check, so the script bytes would
// otherwise be parsed as a bogus ELF and fault.
static int parse_shebang(const char *host_path, char *interp, size_t ni, char *arg, size_t na) {
    int fd = open(host_path, O_RDONLY);
    char hdr[258];
    ssize_t k = fd >= 0 ? read(fd, hdr, sizeof hdr - 1) : -1;
    if (fd >= 0) close(fd);
    if (k < 0) return -1;
    if (k <= 3 || hdr[0] != '#' || hdr[1] != '!') return 0;
    hdr[k] = 0;
    char *nl = strchr(hdr, '\n');
    if (nl) *nl = 0;
    char *s = hdr + 2;
    while (*s == ' ' || *s == '\t')
        // interpreter path
        s++;
    char *e = s;
    while (*e && *e != ' ' && *e != '\t')
        e++;
    char *a = NULL;
    if (*e) {
        *e = 0;
        a = e + 1;
        while (*a == ' ' || *a == '\t')
            a++;
        if (!*a) a = NULL;
    }
    snprintf(interp, ni, "%s", s);
    if (a)
        snprintf(arg, na, "%s", a);
    else
        arg[0] = 0;
    return 1;
}
// Anonymous PRIVATE mmap ranges (MAP_ANON|MAP_PRIVATE) tracked so that madvise(MADV_DONTNEED) can
// give real Linux semantics -- re-mmap fresh zero pages over the range -- WITHOUT ever disturbing a
// file-backed or shared mapping (re-mmapping those with MAP_ANON would discard file data / break
// sharing). DONTNEED only acts when the advised range is fully contained in a tracked private-anon
// region; otherwise it falls back to the safe advisory passthrough. Lock-free like g_gmap; a race can
// only forget an entry (-> safe no-op), and the containment check gates every destructive remap.
static struct {
    uint64_t addr, len;
    int prot;
} g_anonmap[2048];
static int g_nanonmap;
static void anon_track(uint64_t addr, uint64_t len, int prot) {
    if (!addr || g_nanonmap >= (int)(sizeof g_anonmap / sizeof g_anonmap[0])) return;
    g_anonmap[g_nanonmap].addr = addr;
    g_anonmap[g_nanonmap].len = len;
    g_anonmap[g_nanonmap].prot = prot;
    g_nanonmap++;
}
// Forget any tracked anon coverage overlapping [addr,addr+len) -- on munmap, or when a non-anon
// mapping is laid over the range. Err toward forgetting (whole-entry drop) so a stale entry can never
// cause a wrong anon-remap of what is now a file mapping.
static void anon_untrack(uint64_t addr, uint64_t len) {
    uint64_t end = addr + len;
    for (int i = 0; i < g_nanonmap;) {
        uint64_t a = g_anonmap[i].addr, e = a + g_anonmap[i].len;
        if (a < end && addr < e)
            g_anonmap[i] = g_anonmap[--g_nanonmap];
        else
            i++;
    }
}
// prot of the tracked private-anon region fully containing [addr,addr+len), else -1 (unknown -> do
// not remap). Full containment guarantees the range is anon, so the remap cannot corrupt a file map.
static int anon_prot_if_contained(uint64_t addr, uint64_t len) {
    uint64_t end = addr + len;
    for (int i = 0; i < g_nanonmap; i++)
        if (g_anonmap[i].addr <= addr && end <= g_anonmap[i].addr + g_anonmap[i].len) return g_anonmap[i].prot;
    return -1;
}
// /proc/self/fd/N (and /proc/<pid>/fd/N for our own pid -- host pid, container pid, or init's "1")
// names an already-open fd. macOS has no /proc, so detect this form and recover the fd number; the
// caller then resolves it via F_GETPATH (readlinkat) or dup()/reopen (openat). Returns N>=0 on an
// EXACT /proc/.../fd/<N> match, else -1 (anything with a trailing component falls through to normal
// resolution). NOTE: <pid>/fd accepts only this process's pid; foreign pids are not introspectable.
static int procfd_num(const char *p) {
    if (!p) return -1;
    const char *rest = NULL;
    if (!strncmp(p, "/proc/self/fd/", 14))
        rest = p + 14;
    else if (!strncmp(p, "/proc/", 6)) {
        const char *q = p + 6;
        int i = 0;
        char num[16];
        while (q[i] >= '0' && q[i] <= '9' && i < 15) {
            num[i] = q[i];
            i++;
        }
        if (i == 0 || strncmp(q + i, "/fd/", 4)) return -1;
        num[i] = 0;
        int pid = atoi(num);
        if (pid != (int)getpid() && pid != container_pid()) return -1;
        rest = q + i + 4;
    }
    if (!rest || rest[0] < '0' || rest[0] > '9') return -1;
    for (const char *s = rest; *s; s++)
        if (*s < '0' || *s > '9') return -1; // trailing path component -> not a bare fd link
    return atoi(rest);
}
// ===== w3e EPOLL FAST PATH (gate: NOEPOLLOPT=1 reverts to the original per-ctl kevent path) =====
// The baseline emulates epoll over a per-epoll-fd kqueue, but issues a *separate* kevent() syscall
// for every epoll_ctl (and one per filter), then another for epoll_wait. For server event loops that
// rearm (EPOLLONESHOT) or toggle EPOLLOUT every request, that is one extra kevent round-trip per
// request. This path instead BUFFERS the epoll_ctl changes per epoll-fd and submits them as the
// *changelist* argument of the next epoll_wait kevent() — folding all pending registration changes
// into the single wait syscall (the classic libevent/libev kqueue batching). It also tracks the
// armed read/write filter per guest fd so EPOLL_CTL_MOD correctly removes a dropped filter (the
// baseline leaves a stale EVFILT_WRITE armed on a MOD from IN|OUT->IN), without emitting spurious
// EV_DELETEs that would error.  Tables are indexed by fd<1024 (matches every other fd table here);
// epfd/fd >= 1024 fall back to the immediate path.
static int g_epopt = -1;                       // -1 unknown, 0 off, 1 on
static struct kevent *g_ep_chg[1024];          // deferred changelist per epoll fd
static int g_ep_chgn[1024], g_ep_chgcap[1024];
static uint8_t g_ep_rd[1024], g_ep_wr[1024];   // per guest fd: read/write filter currently armed
static uint8_t g_ep_os[1024];                  // per guest fd: EPOLLONESHOT requested (kernel auto-removes on fire)
static unsigned long long g_ep_kevent_calls;   // PROF: kevent() syscalls issued by the epoll path
static int g_epprof = -1;
static int epopt_on(void) {
    if (g_epopt < 0) { const char *e = getenv("NOEPOLLOPT"); g_epopt = (e && e[0] == '1') ? 0 : 1; }
    return g_epopt;
}
static void ep_prof_dump(void) {
    if (g_epprof == 1) fprintf(stderr, "[ddepollprof] epoll_kevent_syscalls=%llu\n", g_ep_kevent_calls);
}
static void ep_count(void) {
    if (g_epprof < 0) { const char *e = getenv("DDEPOLLPROF"); g_epprof = (e && e[0] == '1') ? 1 : 0;
                        if (g_epprof) atexit(ep_prof_dump); }
    if (g_epprof == 1) g_ep_kevent_calls++;
}
// append a change to epfd's buffer, coalescing on (ident,filter) so repeated ctls collapse.
static void ep_push(int ep, uintptr_t ident, int16_t filt, uint16_t flags, void *udata) {
    if (ep < 0 || ep >= 1024) return;
    struct kevent *a = g_ep_chg[ep];
    for (int i = 0; i < g_ep_chgn[ep]; i++)
        if (a[i].ident == ident && a[i].filter == filt) { EV_SET(&a[i], ident, filt, flags, 0, 0, udata); return; }
    if (g_ep_chgn[ep] >= g_ep_chgcap[ep]) {
        int nc = g_ep_chgcap[ep] ? g_ep_chgcap[ep] * 2 : 16;
        struct kevent *na = realloc(a, (size_t)nc * sizeof *na);
        if (!na) return;
        g_ep_chg[ep] = na; g_ep_chgcap[ep] = nc; a = na;
    }
    EV_SET(&a[g_ep_chgn[ep]++], ident, filt, flags, 0, 0, udata);
}
// reset epoll armed-state for a guest fd (called from close(): kqueue auto-removes a closed fd, so the
// armed map must follow to avoid a later stale EV_DELETE on a reused fd number).
static void ep_fd_reset(int fd) {
    if (fd < 0 || fd >= 1024) return;
    g_ep_rd[fd] = g_ep_wr[fd] = g_ep_os[fd] = 0;
    if (g_ep_chg[fd]) { free(g_ep_chg[fd]); g_ep_chg[fd] = NULL; } // if fd was an epoll fd, drop its pending changelist
    g_ep_chgn[fd] = g_ep_chgcap[fd] = 0;
}
static void service(struct cpu *c) {
    // Frontends whose guest has legacy syscalls without a canonical (aarch64) equivalent rewrite them
    // into their *at form here (x86: open->openat, ...); a no-op where the guest is already canonical.
    if (G_NORMALIZE(c)) return;
    uint64_t nr = G_NR(c), a0 = G_A0(c), a1 = G_A1(c), a2 = G_A2(c), a3 = G_A3(c), a4 = G_A4(c), a5 = G_A5(c);
    if (g_trace)
        fprintf(stderr, "[sys] %llu (%llx,%llx,%llx)\n", (unsigned long long)nr, (unsigned long long)a0,
                (unsigned long long)a1, (unsigned long long)a2);
    // S2 path-resolution-cache invalidation: bump the epoch BEFORE dispatch on any syscall that mutates
    // the FS namespace, so no cached guest->host string mapping can survive a create/unlink/rename/mkdir/
    // symlink (over-invalidates, never under -- when in doubt, the next lookup MISSES and re-resolves).
    // Legacy x86 forms (open/mkdir/rename/...) were already normalized to these *at numbers by G_NORMALIZE.
    switch (nr) {
    case 33:  // mknodat
    case 34:  // mkdirat
    case 35:  // unlinkat (covers rmdir via AT_REMOVEDIR)
    case 36:  // symlinkat
    case 37:  // linkat
    case 38:  // renameat
    case 39:  // umount2
    case 40:  // mount
    case 276: // renameat2
        res_bump();
        break;
    case 56: // openat: a2 = Linux flags. O_CREAT (0x40) adds a name. In OVERLAY mode a write-open
             // (O_WRONLY/O_RDWR, a2&3) copies the file lower->upper, RELOCATING its resolved host path
             // -- so it must invalidate too, or a cached lower path goes stale (flat rootfs: no copy-up).
        if ((a2 & 0x40) || (g_nlower && (a2 & 3))) res_bump();
        break;
    case 437: { // openat2: flags live in open_how.flags (a2 -> struct open_how *), before its case rewrites a2
        uint64_t *how = (uint64_t *)a2;
        if (how && ((how[0] & 0x40) || (g_nlower && (how[0] & 3)))) res_bump();
        break;
    }
    default:
        break;
    }
    switch (nr) {
    // ===================== I/O — read/write/seek (+ eventfd/timerfd/signalfd fd redirection) =====================
    case 62: {
        // lseek -- SEEK_SET/CUR/END(0/1/2) match, but SEEK_DATA/SEEK_HOLE are SWAPPED between the ABIs:
        // Linux SEEK_DATA=3,SEEK_HOLE=4 ; macOS SEEK_HOLE=3,SEEK_DATA=4. Translate so sparse-file
        // probing finds holes/data correctly.
        int whence = (int)a2;
        if (whence == 3) whence = 4;      // Linux SEEK_DATA -> macOS SEEK_DATA
        else if (whence == 4) whence = 3; // Linux SEEK_HOLE -> macOS SEEK_HOLE
        off_t r = lseek((int)a0, (off_t)a1, whence);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 63: {
        int rfd = (int)a0;
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
        ssize_t r = readv((int)a0, (void *)a1, (int)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    // readv
    }
    case 66: {
        fd_evict((int)a0);
        ssize_t r = writev((int)a0, (void *)a1, (int)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    // writev
    }
    case 67: {
        // pread64
        ssize_t r = pread((int)a0, (void *)a1, (size_t)a2, (off_t)a3);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 68: {
        fd_evict((int)a0);
        // pwrite64
        ssize_t r = pwrite((int)a0, (void *)a1, (size_t)a2, (off_t)a3);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // sendfile(out,in,off*,count)
    case 71: {
        int outfd = (int)a0, infd = (int)a1;
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
                memcpy((void *)a0, g_cwd, l + 1);
                G_RET(c) = l + 1;
            } else
                G_RET(c) = (uint64_t)(-ERANGE);
            break;
        }
        if (getcwd((char *)a0, (size_t)a1))
            G_RET(c) = strlen((char *)a0) + 1;
        else
            G_RET(c) = (uint64_t)(-errno);
        break;
    }
    case 23: {
        // dup
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
    // ioctl(fd, req, arg) -- Linux req# -> macOS
    case 29: {
        int fd = (int)a0;
        unsigned long rq = (unsigned long)a1;
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
            G_RET(c) = tcsetattr(fd, act, &t) < 0 ? (uint64_t)(-errno) : 0;
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
            G_RET(c) = tcsetattr(fd, act, &t) < 0 ? (uint64_t)(-errno) : 0;
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
        case 0x540f:
            if (arg) *(int *)arg = (int)getpgrp();
            G_RET(c) = 0;
            // TIOCGPGRP
            break;
        // TIOCSPGRP
        case 0x5410: G_RET(c) = 0; break;
        // TIOCSCTTY
        case 0x540e: G_RET(c) = 0; break;
        // ENOTTY
        default: G_RET(c) = (uint64_t)(-25); break;
        }
        break;
    }
    // mknodat(dirfd, path, mode, dev)
    case 33: {
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
            }
            close(pfd);
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : 0;
            break;
        }
        char pb[4200];
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        int r = mknodat(ATFD(a0), p, (mode_t)a2, (dev_t)a3);
        if (r >= 0) {
            mc_evict(p);
            ac_evict(p);
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // mkdirat(dirfd, path, mode) -- confined
    case 34: {
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
            }
            close(pfd);
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : 0;
            break;
        }
        char pb[4200];
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        int r = mkdirat(ATFD(a0), p, (mode_t)a2);
        mc_evict(p);
        // namespace change -> evict
        ac_evict(p);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // unlinkat(dirfd, path, flags) -- confined
    case 35: {
        // OVERLAY: whiteout (hides lower) + drop the upper copy
        if (g_rootfs && g_nlower) {
            char gp[4200];
            abs_guest((int)a0, (const char *)a1, gp, sizeof gp);
            char host[4300];
            if (!overlay_resolve(gp, host, sizeof host, 1)) {
                G_RET(c) = (uint64_t)(-2);
                break;
            // ENOENT
            }
            overlay_whiteout(gp);
            G_RET(c) = 0;
            break;
        }
        if (g_rootfs) {
            char fin[512];
            int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, 1);
            if (pfd < 0) {
                G_RET(c) = (uint64_t)(int64_t)pfd;
                break;
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
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : 0;
            break;
        }
        char pb[4200];
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        int r = unlinkat(ATFD(a0), p, (a2 & 0x200) ? AT_REMOVEDIR : 0);
        mc_evict(p);
        ac_evict(p);
        rl_evict(p);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // symlinkat(target, newdirfd, linkpath)
    case 36: {
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
        const char *p = atpath((int)a1, (const char *)a2, pb, sizeof pb);
        G_RET(c) = symlinkat(target, ATFD(a1), p) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // linkat(odir,opath,ndir,npath,flags)
    case 37: {
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
        const char *op = atpath((int)a0, (const char *)a1, ob, sizeof ob);
        const char *np = atpath((int)a2, (const char *)a3, nb, sizeof nb);
        G_RET(c) = linkat(ATFD(a0), op, ATFD(a2), np, fl) < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 38:
    // renameat(38) / renameat2(276): translate the renameat2 flags onto macOS renameatx_np --
    // RENAME_NOREPLACE(1)->RENAME_EXCL (fail if dst exists), RENAME_EXCHANGE(2)->RENAME_SWAP (atomic swap).
    case 276: {
        unsigned int rxflags = 0;
        if (nr == 276) {
            int lf = (int)a4;
            if (lf & 1) rxflags |= RENAME_EXCL;
            if (lf & 2) rxflags |= RENAME_SWAP;
        }
        if (g_rootfs) {
            // both ends confined (TOCTOU-free)
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
            G_RET(c) = r < 0 ? (uint64_t)(-(int64_t)e) : 0;
            break;
        }
        char ob[4200], nb[4200];
        const char *op = atpath((int)a0, (const char *)a1, ob, sizeof ob);
        const char *np = atpath((int)a2, (const char *)a3, nb, sizeof nb);
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
            const char *p = atpath(-100, (const char *)a0, pb, sizeof pb);
            r = statfs(p, &hs);
        } else {
            r = fstatfs((int)a0, &hs);
        }
        if (r < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        uint8_t *b = (uint8_t *)a1;
        memset(b, 0, 120);
        *(int64_t *)(b + 0) = 0x01021994;                // f_type (TMPFS_MAGIC; geometry is what matters)
        *(int64_t *)(b + 8) = (int64_t)hs.f_bsize;       // f_bsize
        *(uint64_t *)(b + 16) = (uint64_t)hs.f_blocks;   // f_blocks
        *(uint64_t *)(b + 24) = (uint64_t)hs.f_bfree;    // f_bfree
        *(uint64_t *)(b + 32) = (uint64_t)hs.f_bavail;   // f_bavail
        *(uint64_t *)(b + 40) = (uint64_t)hs.f_files;    // f_files
        *(uint64_t *)(b + 48) = (uint64_t)hs.f_ffree;    // f_ffree
        *(int32_t *)(b + 56) = hs.f_fsid.val[0];         // f_fsid[0]
        *(int32_t *)(b + 60) = hs.f_fsid.val[1];         // f_fsid[1]
        *(int64_t *)(b + 64) = 255;                      // f_namelen (NAME_MAX)
        *(int64_t *)(b + 72) = (int64_t)hs.f_bsize;      // f_frsize
        *(int64_t *)(b + 80) = 0;                        // f_flags
        G_RET(c) = 0;
        break;
    }
    case 46: {
        int r = ftruncate((int)a0, (off_t)a1);
        fd_evict((int)a0);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    // ftruncate
    }
    case 47: {
        // fallocate(fd,mode,offset,len). FALLOC_FL_PUNCH_HOLE(2)|KEEP_SIZE(1): deallocate+zero a range
        // via macOS F_PUNCHHOLE (file stays the same size, the range reads as zeros).
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
        const char *p = atpath(-100, (const char *)a0, pb, sizeof pb);
        if (chdir(p) < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        if (g_rootfs && !strncmp(p, g_rootfs_canon, g_rootfs_canon_len)) {
            const char *g = p + g_rootfs_canon_len;
            snprintf(g_cwd, sizeof g_cwd, "%s", g[0] ? g : "/");
        }
        G_RET(c) = 0;
        break;
    }
    case 50: {
        if (fchdir((int)a0) < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        // fchdir (tracks guest cwd)
        }
        if ((int)a0 >= 0 && (int)a0 < 1024 && g_fdpath[(int)a0][0]) {
            const char *g = g_fdpath[(int)a0];
            if (g_rootfs && !strncmp(g, g_rootfs_canon, g_rootfs_canon_len)) g += g_rootfs_canon_len;
            snprintf(g_cwd, sizeof g_cwd, "%s", g[0] ? g : "/");
        }
        G_RET(c) = 0;
        break;
    }
    // fchmod(fd, mode)
    case 52: G_RET(c) = fchmod((int)a0, (mode_t)a1) < 0 ? (uint64_t)(-errno) : 0; break;
    case 53:
    // fchmodat(dirfd,path,mode,flags) / fchmodat2
    case 452: {
        if (g_rootfs) {
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
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        int r = fchmodat(ATFD(a0), p, (mode_t)a2, 0);
        if (r >= 0) mc_evict(p);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // fchownat(dirfd,path,uid,gid,flags) -- best-effort (rootless)
    case 54: {
        if (g_rootfs) {
            char fin[512];
            int pfd = jail_at((int)a0, (const char *)a1, fin, sizeof fin, (a4 & 0x100) ? 1 : 0);
            if (pfd < 0) {
                G_RET(c) = (uint64_t)(int64_t)pfd;
                break;
            }
            fchownat(pfd, fin, (uid_t)a2, (gid_t)a3, (a4 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0);
            close(pfd);
            G_RET(c) = 0;
            break;
        // EPERM on the host -> faked OK
        }
        char pb[4200];
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        fchownat(ATFD(a0), p, (uid_t)a2, (gid_t)a3, 0);
        G_RET(c) = 0;
        break;
    }
    case 55: {
        fchown((int)a0, (uid_t)a1, (gid_t)a2);
        G_RET(c) = 0;
        break;
    // fchown(fd,uid,gid) -- best-effort
    }
    case 56: {
        // openat -- Linux O_* -> macOS O_* (they differ!)
        int lf = (int)a2, mf = lf & 0x3;
        // O_TMPFILE (the __O_TMPFILE bit 0x400000 is arch-independent): create an unnamed, auto-cleaned
        // regular file inside the named directory by making one + immediately unlinking it (macOS has no
        // O_TMPFILE). The fd is a normal RW file with link count 0.
        if (lf & 0x400000) {
            char pb[4200];
            const char *dir = atpath((int)a0, (const char *)a1, pb, sizeof pb);
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
            if (fd >= 0 && fd < 1024) g_fdpath[fd][0] = 0; // anonymous: no tracked path
            G_RET(c) = fd < 0 ? (uint64_t)(-(int64_t)e) : (uint64_t)fd;
            break;
        }
        {
            // synthesize /proc/* (macOS has no /proc)
            const char *rp = (const char *)a1;
            if (rp && !strncmp(rp, "/proc/", 6)) {
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
            // device nodes -> host devices (rootfs has no real /dev)
            if (rp && !strncmp(rp, "/dev/", 5)) {
                const char *hd = !strcmp(rp, "/dev/null")      ? "/dev/null"
                                 : !strcmp(rp, "/dev/zero")    ? "/dev/zero"
                                 : !strcmp(rp, "/dev/full")    ? "/dev/null"
                                 : !strcmp(rp, "/dev/random")  ? "/dev/random"
                                 : !strcmp(rp, "/dev/urandom") ? "/dev/urandom"
                                 : !strcmp(rp, "/dev/tty")     ? "/dev/tty"
                                                               : NULL;
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
        {
            // /proc/self/fd/N -> reopen what host fd N points at. Linux reopen gives a FRESH file
            // description (offset 0, access narrowed to the requested mode), so prefer reopening by the
            // F_GETPATH path with the guest's flags; for fds with no path (pipe/socket/anon) fall back to
            // dup(N), which at least hands back a working, equivalent fd.
            int pfn = procfd_num((const char *)a1);
            if (pfn >= 0) {
                char gp[4200];
                int r = -1;
                if (fcntl(pfn, F_GETPATH, gp) == 0 && gp[0])
                    r = open(gp, mf & ~(O_EXCL | O_CREAT), (mode_t)a3);
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
            const char *rp = (const char *)a1;
            if (rp && !strncmp(rp, "/dev/shm/", 9)) {
                char hp[300];
                int n = snprintf(hp, sizeof hp, "/tmp/.ddshm-%s", rp + 9);
                for (int i = 12; i < n; i++) if (hp[i] == '/') hp[i] = '_';
                int d = open(hp, mf, (mode_t)a3);
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
            int r = open(host, mf | ((lf & G_O_NOFOLLOW) ? O_NOFOLLOW : 0), (mode_t)a3);
            if (r >= 0) {
                char gpa[4200];
                if (fcntl(r, F_GETPATH, gpa) == 0) {
                    fd_setpath(r, gpa);
                    if (isw) {
                        mc_evict(gpa);
                        rl_evict(gpa);
                        ac_evict(gpa);
                    }
                }
                if (r < 1024) snprintf(g_ovldir[r], sizeof g_ovldir[r], "%s", gp);
            // remember guest path for merged getdents
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
            int r = openat(pfd, fin, mf | O_NOFOLLOW, (mode_t)a3);
            int e = errno;
            close(pfd);
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
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        int r = openat(ATFD(a0), p, mf, (mode_t)a3);
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
        if (cf >= 0 && cf < 1024) {
            if (g_eventfd_peer[cf]) {
                close(g_eventfd_peer[cf] - 1);
                g_eventfd_peer[cf] = 0;
            }
            g_timerfd[cf] = 0;
            g_ovldir[cf][0] = 0;
            g_lo_port[cf] = 0;
            g_sock_stream[cf] = 0;
            g_br_port[cf] = 0;
            g_br_ip[cf] = 0;
            g_eventfd_count[cf] = 0;
            g_eventfd_sema[cf] = 0;
            ep_fd_reset(cf); // w3e: drop epoll armed-state (kqueue auto-removes a closed fd)
        // reap eventfd peer / timerfd / overlay dir / loopback
        }
        dirs_drop(cf); // invalidate the getdents DIR* cache so a reused fd re-opendir's
        int r = close(cf);
        fd_clear(cf);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    // close: -errno on fail
    }
    case 59: {
        int fds[2];
        if (pipe(fds) < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        // pipe2(fds, flags)
        }
        int fl = (int)a1;
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
    // getdents64
    case 61: {
        int fd = (int)a0;
        // OVERLAY: merged listing across layers
        if (g_nlower && fd >= 0 && fd < 1024 && g_ovldir[fd][0]) {
            static struct {
                int fd;
                int n, pos;
                char nm[1024][256];
                uint8_t ty[1024];
            } oc[16];
            int slot = -1;
            for (int i = 0; i < 16; i++)
                if (oc[i].fd == fd + 1) {
                    slot = i;
                    break;
                }
            if (slot < 0) {
                for (int i = 0; i < 16; i++)
                    if (oc[i].fd == 0) {
                        slot = i;
                        break;
                    }
                if (slot < 0) slot = 0;
                oc[slot].fd = fd + 1;
                oc[slot].pos = 0;
                oc[slot].n = overlay_readdir(g_ovldir[fd], oc[slot].nm, oc[slot].ty, 1024);
            }
            uint8_t *out = (uint8_t *)a1;
            size_t o = 0;
            while (oc[slot].pos < oc[slot].n) {
                const char *nm = oc[slot].nm[oc[slot].pos];
                size_t nl = strlen(nm), lr = (19 + nl + 1 + 7) & ~7ull;
                if (o + lr > (size_t)a2) break;
                uint8_t *ld = out + o;
                *(uint64_t *)(ld + 0) = oc[slot].pos + 1;
                *(uint64_t *)(ld + 8) = o + lr;
                *(uint16_t *)(ld + 16) = (uint16_t)lr;
                *(ld + 18) = oc[slot].ty[oc[slot].pos];
                memcpy(ld + 19, nm, nl);
                ld[19 + nl] = 0;
                o += lr;
                oc[slot].pos++;
            }
            // exhausted -> free the slot
            if (o == 0) oc[slot].fd = 0;
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
        // /proc/self/fd/N -> the path host fd N currently points at (recovered via F_GETPATH on macOS).
        int pfn = procfd_num(p);
        if (pfn >= 0) {
            char gp[4200];
            if (fcntl(pfn, F_GETPATH, gp) != 0) {
                G_RET(c) = (uint64_t)(-errno); // bad fd -> EBADF
                break;
            }
            // map the host path back into the guest's view (strip the rootfs prefix if jailed)
            const char *gpath = (g_rootfs && !strncmp(gp, g_rootfs_canon, g_rootfs_canon_len)) ? gp + g_rootfs_canon_len
                                                                                                : gp;
            if (!gpath[0]) gpath = "/";
            size_t l = strlen(gpath);
            if (l > bs) l = bs;
            memcpy(buf, gpath, l);
            G_RET(c) = l;
            break;
        }
        if (p && strstr(p, "/proc/self/exe")) {
            char rp[1024];
            if (!realpath(g_exe_path, rp)) strncpy(rp, g_exe_path, sizeof rp - 1);
            size_t l = strlen(rp);
            if (l > bs) l = bs;
            memcpy(buf, rp, l);
            G_RET(c) = l;
        } else {
            char pb[4200];
            const char *rp = xlate(p, pb, sizeof pb);
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
        const char *raw = (const char *)a1, *p = atpath((int)a0, raw, pb, sizeof pb);
        {
            const char *gp = (g_rootfs && !strncmp(p, g_rootfs_canon, g_rootfs_canon_len)) ? p + g_rootfs_canon_len : p;
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
            if (rc == 0) fill_linux_stat((uint8_t *)a2, &s);
            G_RET(c) = (uint64_t)(int64_t)rc;
            break;
        }
        // AT_EMPTY_PATH -> fstat(dfd)
        int r = (raw && !raw[0] && (a3 & 0x1000)) ? fstat((int)a0, &s)
                                                  : fstatat(ATFD(a0), p, &s, AT_SYMLINK_NOFOLLOW);
        if (r < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        fill_linux_stat((uint8_t *)a2, &s);
        G_RET(c) = 0;
        break;
    }
    case 80: {
        // fstat(fd, buf)
        struct stat s;
        if (fstat((int)a0, &s) < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        fill_linux_stat((uint8_t *)a1, &s);
        G_RET(c) = 0;
        break;
    }
    case 81:
        sync();
        G_RET(c) = 0;
        // sync
        break;
    // fsync -- durability policy (S3DB_DURABILITY): default/fast == plain fsync() (legacy path)
    case 82: G_RET(c) = s3db_sync_fd((int)a0); break;
    // fdatasync -> fsync (no macOS fdatasync); same durability policy
    case 83: G_RET(c) = s3db_sync_fd((int)a0); break;
    // utimensat(dirfd, path, times, flags)
    case 88: {
        struct timespec *ts = (struct timespec *)a2;
        if (!a1) {
            G_RET(c) = futimens((int)a0, ts) < 0 ? (uint64_t)(-errno) : 0;
            break;
        // path NULL -> futimens(fd)
        }
        if (g_rootfs) {
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
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
        int r = utimensat(ATFD(a0), p, ts, (a3 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0);
        if (r >= 0) mc_evict(p);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // umask -> old mask
    case 166: G_RET(c) = (uint64_t)umask((mode_t)a0); break;
    // fadvise64 -- advisory no-op
    case 223: G_RET(c) = 0; break;
    // copy_file_range(fdin,offin*,fdout,offout*,len,flags)
    case 285: {
        int fdin = (int)a0, fdout = (int)a2;
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
    case 291: {
        struct stat s;
        // statx(dfd, path, flags, mask, buf)
        char pb[4200];
        const char *raw = (const char *)a1, *p = atpath((int)a0, raw, pb, sizeof pb);
        int rc, empty = (raw && !raw[0] && (a2 & 0x1000));
        const char *gp = (g_rootfs && !strncmp(p, g_rootfs_canon, g_rootfs_canon_len)) ? p + g_rootfs_canon_len : p;
        if (synth_stat_raw(gp, &s)) {
            rc = 0;
        // synth /proc or /sys -> fill from s below
        }
        // cacheable
        else if (raw && raw[0] && !empty) {
            if (!mc_lookup(p, &rc, &s)) {
                int rr = fstatat(ATFD(a0), p, &s, 0);
                rc = rr < 0 ? -errno : 0;
                mc_store(p, rc, &s);
            }
        } else {
            int rr = empty ? fstat((int)a0, &s) : fstatat(ATFD(a0), p, &s, 0);
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
    // openat2(dirfd, path, open_how*, size) -- glibc uses it; MUST confine
    case 437: {
        //   open_how { u64 flags; u64 mode; u64 resolve; }
        uint64_t *how = (uint64_t *)a2;
        a2 = how ? how[0] : 0;
        // -> openat(dirfd, path, flags, mode); resolve flags ignored (jail confines)
        a3 = how ? how[1] : 0;
    } /* fall through to openat */
    // faccessat2(dirfd,path,mode,flags) -- glibc access() uses it; same path/confinement, flags ignored
    case 439:
    case 48: {
        char pb[4200];
        // faccessat
        const char *p = atpath((int)a0, (const char *)a1, pb, sizeof pb);
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

    // ===================== Memory — mmap/brk/mprotect/madvise (anon charged against cgroup memory.max)
    // =====================
    // brk
    case 214: {
        if (!G_BRK_GROWABLE) { // fixed, non-growable break -> glibc/musl fall back to their mmap allocator
            G_RET(c) = brk_lo;
            break;
        }
        if (a0 == 0) {
            G_RET(c) = brk_cur;
            break;
        }
        if (a0 >= brk_lo && a0 <= brk_hi) {
            // heap growth -> charge cgroup memory.max
            if (g_mem_max && a0 > brk_cur) {
                uint64_t delta = a0 - brk_cur;
                if (atomic_fetch_add(&g_mem_charged, delta) + delta > g_mem_max) {
                    atomic_fetch_sub(&g_mem_charged, delta);
                    G_RET(c) = brk_cur;
                    // over limit -> break unchanged (ENOMEM)
                    break;
                }
            // shrink -> uncharge
            } else if (g_mem_max && a0 < brk_cur) {
                uint64_t delta = brk_cur - a0, cur = atomic_load(&g_mem_charged);
                atomic_fetch_sub(&g_mem_charged, delta > cur ? cur : delta);
            }
            brk_cur = a0;
        }
        G_RET(c) = brk_cur;
        break;
    }
    case 215: {
        // munmap
        int r = munmap((void *)a0, (size_t)a1);
        if (r == 0) {
            gmap_del(a0); // drop from the execve() teardown registry
            anon_untrack(a0, (size_t)a1); // and from the DONTNEED anon-range registry
        }
        if (r == 0 && g_mem_max) {
            // uncharge (clamp >=0)
            uint64_t cur = atomic_load(&g_mem_charged), d = (uint64_t)a1;
            atomic_fetch_sub(&g_mem_charged, d > cur ? cur : d);
        }
        G_RET(c) = (uint64_t)r;
        break;
    }
    case 216: {
        // mremap (copy+grow)
        void *r = mmap(0, (size_t)a2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (r == MAP_FAILED) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        size_t old = (size_t)a1, n = old < (size_t)a2 ? old : (size_t)a2;
        memcpy(r, (void *)a0, n);
        // W4-B leak fix: mremap MOVES the mapping, so free the OLD region (it was leaked — every
        // realloc-driven grow leaked the whole previous buffer; busybox `sort` of a 2M-line file does
        // ~3,874 grows leaking ~31 GB -> ~30 GB RSS -> OOM SIGKILL). Free the tracked extent (incl. the
        // 64 KB guard tail); allocate-before-free preserved above.
        uint64_t old_full = gmap_find_len(a0); // tracked extent (incl. guard tail); fall back to old length
        if (old_full < old) old_full = old;
        if (a0) {
            munmap((void *)a0, (size_t)old_full);
            gmap_del(a0);
            anon_untrack(a0, (size_t)old_full); // untrack the FULL extent (was untracking only `old`)
        }
        gmap_add((uint64_t)r, (uint64_t)a2);    // track the new region for execve() teardown
        anon_track((uint64_t)r, (size_t)a2, PROT_READ | PROT_WRITE); // fresh private-anon copy
        G_RET(c) = (uint64_t)r;
        break;
    }
    // mmap
    case 222: {
        // charge anon, but NOT MAP_NORESERVE
        int charge = g_mem_max && (a3 & 0x20) && !(a3 & 0x4000);
        //   (libc reserves huge virtual arenas it never commits;
        if (charge) {
            if (atomic_fetch_add(&g_mem_charged, (uint64_t)a1) + (uint64_t)a1 >
                // real memory.max counts RSS, not reservations)
                g_mem_max) {
                atomic_fetch_sub(&g_mem_charged, (uint64_t)a1);
                G_RET(c) = (uint64_t)(-ENOMEM);
                break;
            }
        }
        // glibc's vectorized string ops over-read up to 16 bytes past a buffer's logical end; on Darwin
        // that hits an unmapped page -> SIGBUS. Map a 64KB guard tail on non-fixed anon maps so the
        // over-read lands in mapped zero memory (x86 glibc relies on this; harmless for aarch64).
        size_t guard = (!(a3 & 0x10) && (a3 & 0x20)) ? 0x10000 : 0;
        // mprotect (case 226) is a no-op (the JIT never executes guest pages), so a later PROT_READ ->
        // PROT_READ|WRITE upgrade would be silently dropped. Map ANON memory writable up front so the
        // upgrade is already in effect (redis' checkLinuxMadvFreeForkBug mmaps R then mprotects RW then stores).
        int prot = (a3 & 0x20) ? ((int)a2 | PROT_READ | PROT_WRITE) : (int)a2;
        void *r = mmap((void *)a0, (size_t)a1 + guard, prot, mmap_flags((int)a3), (a3 & 0x20) ? -1 : (int)a4,
                       (off_t)a5);
        // refund
        if (r == MAP_FAILED && charge) atomic_fetch_sub(&g_mem_charged, (uint64_t)a1);
        if (r != MAP_FAILED) {
            gmap_add((uint64_t)r, (uint64_t)a1 + guard); // track for execve() teardown
            // DONTNEED anon registry: record PRIVATE-ANON ranges (incl. the guard tail); for any other
            // (file-backed/shared) mapping, forget overlapping anon coverage -- a MAP_FIXED file map may
            // now sit where anon used to, and we must never anon-remap over it.
            if ((a3 & 0x20) && (a3 & 0x02))
                anon_track((uint64_t)r, (uint64_t)a1 + guard, prot);
            else
                anon_untrack((uint64_t)r, (uint64_t)a1 + guard);
        }
        G_RET(c) = (r == MAP_FAILED) ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // mprotect
    case 226: // mprotect: NO-OP. The JIT translates guest code and never executes guest pages, so it does
        // not enforce guest page protection. Actually calling mprotect is harmful on macOS -- it would make
        // a region truly read-only and then fault the guest's own legitimate writes to it (e.g. RELRO).
        G_RET(c) = 0;
        break;
    case 227: // msync: stores through a MAP_SHARED mapping are already in the unified page cache, so the
        // file is coherent without an explicit flush; treat as success (avoids a spurious -ENOSYS).
        // Default/fast/none keep the no-op (page-cache coherent). Only `strict` issues a real host
        // msync for on-platter writeback durability, translating Linux MS_* flags to macOS (macOS
        // MS_SYNC=16 != Linux 4; MS_ASYNC=1/MS_INVALIDATE=2 match), tolerating EINVAL.
        if (s3db_durability() == 2) {
            int lf = (int)a2, mf = 0;
            if (lf & 0x1) mf |= MS_ASYNC;       // Linux MS_ASYNC=1
            if (lf & 0x2) mf |= MS_INVALIDATE;  // Linux MS_INVALIDATE=2
            if (lf & 0x4) mf |= MS_SYNC;        // Linux MS_SYNC=4 -> macOS MS_SYNC(16)
            if (!(mf & (MS_ASYNC | MS_SYNC))) mf |= MS_SYNC; // default to a sync flush
            int r = msync((void *)a0, (size_t)a1, mf);
            G_RET(c) = (r < 0 && errno != EINVAL) ? (uint64_t)(-errno) : 0;
        } else {
            G_RET(c) = 0;
        }
        break;
    case 228:
    case 229:
        G_RET(c) = 0;
        // mlock/munlock (no-op)
        break;
    // Container-init compat: in the single-process model these are no-ops that return success so
    // entrypoints (mount /proc, unshare, drop caps, set hostname) proceed; the path-jail is the
    // real boundary, and a faked namespace grants no actual privilege (program still runs as our uid).
    // mincore -> unsupported (callers fall back)
    case 232: G_RET(c) = (uint64_t)(-ENOSYS); break;
    case 233: {
        // madvise: best-effort, advisory (never fail the guest). Only forward advice values whose
        // meaning is identical on both kernels -- NORMAL/RANDOM/SEQUENTIAL/WILLNEED/DONTNEED(0..4)
        // match, and Linux MADV_FREE(8) -> macOS MADV_FREE. Every OTHER Linux advice number collides
        // with an unrelated macOS one (e.g. Linux DONTFORK=10 vs macOS PAGEOUT=10), so no-op those.
        // (Note: macOS MADV_DONTNEED does not zero anonymous pages the way Linux's does.)
        int adv = (int)a2, hadv = -1;
        // MADV_DONTNEED(4): Linux drops the pages so the NEXT access faults in fresh ZERO pages. macOS
        // MADV_DONTNEED does not zero anon pages, so a reread would return stale data (breaks
        // redis/jemalloc, which lean on the zeroing). For a range fully inside a tracked PRIVATE-ANON
        // region we re-establish it with a fresh MAP_FIXED|MAP_ANON|MAP_PRIVATE mapping -> next read
        // faults in zeros. File-backed/shared mappings are NEVER touched here (the containment check
        // fails for them); they keep the safe advisory passthrough below.
        if (adv == 4 && a1) {
            int aprot = anon_prot_if_contained(a0, (size_t)a1);
            if (aprot >= 0) {
                void *r = mmap((void *)a0, (size_t)a1, aprot, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
                if (r != MAP_FAILED) {
                    G_RET(c) = 0;
                    break;
                }
                // remap failed (e.g. unaligned) -> never fail the guest; fall through to advisory
            }
        }
        if (adv >= 0 && adv <= 4) hadv = adv;
        else if (adv == 8) hadv = MADV_FREE;
        if (hadv >= 0 && madvise((void *)a0, (size_t)a1, hadv) < 0) { /* advisory: ignore */ }
        G_RET(c) = 0;
        break;
    }

    // ===================== Process & scheduling — clone/exec/wait/ids/prctl/futex/caps/sched =====================
    case 90: {
        if (a1) memset((void *)a1, 0xff, 12);
        G_RET(c) = 0;
        break;
    // capget -> all caps present
    }
    // capset -> ok
    case 91: G_RET(c) = 0; break;
    case 93:
        c->exited = 1;
        c->exit_code = (int)a0;
        // exit: end THIS thread
        break;
    // exit_group: end the whole process
    case 94:
        if (getenv("PROF"))
            fprintf(stderr,
                    "[prof] crossings=%llu syscalls=%llu ibtc_miss=%llu branch_cross=%llu translations=%llu lse=%llu\n",
                    (unsigned long long)g_prof_cross, (unsigned long long)g_prof_sys, (unsigned long long)g_prof_miss,
                    (unsigned long long)(g_prof_cross - g_prof_sys - g_prof_miss), (unsigned long long)g_prof_xlate,
                    (unsigned long long)g_lse_n);
        ep_prof_dump(); // w3e: flush epoll kevent-syscall counter (atexit is bypassed by _exit)
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
    // sched_setaffinity
    case 122: G_RET(c) = 0; break;
    case 123: {
        size_t n = (size_t)a1;
        // sched_getaffinity(pid,size,MASK=a2!)
        if (n > 128) n = 128;
        if (a2 && n) {
            memset((void *)a2, 0, n);
            *(uint8_t *)a2 = 1;
        // cpu 0 set; mask is a2 not a1
        }
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
    case 144:
    case 146:
    case 147:
    // setgid/setfsuid/setresuid/setresgid -> ok
    case 149: G_RET(c) = 0; break;
    // getpgid
    case 145: G_RET(c) = (uint64_t)getpgrp(); break;
    case 148: {
        // getresuid(r,e,s)
        if (a0) *(uint32_t *)a0 = cuid();
        if (a1) *(uint32_t *)a1 = cuid();
        if (a2) *(uint32_t *)a2 = cuid();
        G_RET(c) = 0;
        break;
    }
    case 150: {
        // getresgid(r,e,s)
        if (a0) *(uint32_t *)a0 = cgid();
        if (a1) *(uint32_t *)a1 = cgid();
        if (a2) *(uint32_t *)a2 = cgid();
        G_RET(c) = 0;
        break;
    }
    // setpgid
    case 154: G_RET(c) = setpgid((pid_t)a0, (pid_t)a1) < 0 ? (uint64_t)(-errno) : 0; break;
    // getpgid (bash job control)
    case 155: G_RET(c) = (uint64_t)getpgid((pid_t)a0); break;
    // getsid
    case 156: G_RET(c) = (uint64_t)getsid((pid_t)a0); break;
    case 158: {
        if (g_gid >= 0) {
            if ((int)a0 >= 1 && a1) *(gid_t *)a1 = (gid_t)cgid();
            G_RET(c) = 1;
            break;
        // getgroups -> [container gid]
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
        if ((int)a0 == 15) { snprintf(g_procname, sizeof g_procname, "%.15s", (const char *)a1); G_RET(c) = 0; break; } // PR_SET_NAME
        if ((int)a0 == 16) { snprintf((char *)a1, 16, "%s", g_procname); G_RET(c) = 0; break; }                          // PR_GET_NAME
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
    case 174:
    // getuid/geteuid -> container uid (0=root by default)
    case 175: G_RET(c) = (uint64_t)cuid(); break;
    case 176:
    // getgid/getegid
    case 177: G_RET(c) = (uint64_t)cgid(); break;
    // gettid
    case 178: G_RET(c) = (uint64_t)container_pid(); break;
    // clone(flags,stack,ptid,tls,ctid)
    case 220: {
        // CLONE_THREAD: stack arg IS the top
        if (a0 & 0x10000) {
            G_RET(c) = (uint64_t)spawn_thread(c, a0, a1, a3, a2, a4);
            break;
        }
        // fork/vfork: COW copy; child continues
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
        break;
    }
    // execve(path, argv, envp)
    case 221: {
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
        uint64_t *gv = (uint64_t *)a1;
        while (gv && gv[ac] && ac < 255) {
            argv[ac] = (char *)gv[ac];
            ac++;
        }
        argv[ac] = NULL;
        // shebang: exec the #! interpreter instead (parse_shebang is shared with the initial loader)
        char sh_interp[256], sh_arg[256], shpb[4200];
        if (parse_shebang(p, sh_interp, sizeof sh_interp, sh_arg, sizeof sh_arg) == 1) {
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
            // load the interpreter, not the script
            p = xresolve_exec(sh_interp, shpb, sizeof shpb);
            if (access(p, F_OK) != 0) {
                G_RET(c) = (uint64_t)(-2);
                break;
            }
            for (int i = 0; i <= ni; i++)
                argv[i] = na[i];
            ac = ni;
        }
        // Tear down the inherited guest address space before loading the new image: a post-fork exec
        // otherwise keeps the parent's DENSE layout, and load_elf must bias a non-PIE ET_EXEC off its
        // fixed vaddr (__PAGEZERO blocks the low 4 GB) -> its baked absolute refs collide -> SIGSEGV.
        // argv + path live in guest memory we're about to munmap, so copy them to the host heap first.
        char *xpath = strdup(p);
        char *xargv[256];
        for (int i = 0; i < ac && i < 255; i++) xargv[i] = strdup(argv[i]);
        xargv[ac < 255 ? ac : 255] = NULL;
        gmap_reset_all();
        g_nonpie_lo = g_nonpie_hi = 0; // reset; load_elf re-sets it iff the new main image is non-PIE
        p = xpath;
        for (int i = 0; i < ac && i < 255; i++) argv[i] = xargv[i];
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
        // execve: drop IBTC + §B shadow (old image)
        G_SHADOW_RESET(c);
        uint8_t *heap = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        gmap_add((uint64_t)heap, 256u << 20);
        brk_lo = brk_cur = (uint64_t)heap;
        brk_hi = brk_lo + (256u << 20);
        uint64_t sp = build_stack(ac, argv, &lm, at_base);
        free(xpath);
        for (int i = 0; i < ac && i < 255; i++) free(xargv[i]);
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
        pid_t r = wait4((pid_t)(int)a0, &st, (int)a2, (struct rusage *)a3);
        if (r < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        // WIFSIGNALED: macOS termsig -> Linux
        if ((st & 0x7f) != 0 && (st & 0x7f) != 0x7f)
            st = (st & ~0x7f) | (sig_m2l(st & 0x7f) & 0x7f);
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
            uint64_t *o = (uint64_t *)a3;
            // RLIMIT_STACK=8MB, else unlimited
            o[0] = ((int)a1 == 3) ? (8ull << 20) : ~0ull;
            o[1] = ~0ull;
        }
        G_RET(c) = 0;
        break;
    }
    // clone3(clone_args*, size)
    case 435: {
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
            G_SHADOW_RESET(c);
            rc_reset();
        }
        G_RET(c) = pid < 0 ? (uint64_t)(-errno) : (uint64_t)pid;
        break;
    }

    // ===================== Signals — Linux signal numbers -> macOS; kill/sigaction/sigreturn =====================
    // kill(pid,sig)
    case 129:
        if ((int)a0 == container_pid() || (int)a0 <= 0) {
            raise_guest_signal(c, (int)a1);
            G_RET(c) = 0;
        // self / pgrp (PID-ns aware)
        }
        else
            G_RET(c) = kill((pid_t)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0;
        break;
    case 130:
        raise_guest_signal(c, (int)a1);
        G_RET(c) = 0;
        // tkill(tid,sig)
        break;
    case 131:
        raise_guest_signal(c, (int)a2);
        G_RET(c) = 0;
        // tgkill(tgid,tid,sig)
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
            // can't touch SIGKILL/SIGSTOP (Linux nums)
            if (sig != 9 && sig != 19) {
                // host(macOS) signo to install on
                int ms = sig_l2m(sig);
                if (h == 0)
                    signal(ms, SIG_DFL);
                else if (h == 1)
                    // honor SIG_IGN (e.g. SIGPIPE)
                    signal(ms, SIG_IGN);
                // async: flag pending, deliver in dispatcher
                else if (!sig_is_sync(sig)) {
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

    // ===================== Network — sockets; port-map (-p) + NET-ns private loopback =====================
    case 198: {
        int ty = (int)a1;
        // socket (translate Linux domain -> macOS: AF_INET6 10->30, others unchanged)
        int r = socket(af_l2m((int)a0), ty & 0xf, (int)a2);
        if (r >= 0) {
            if (ty & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC);
            if (ty & 0x800) fcntl(r, F_SETFL, O_NONBLOCK);
            if (r < 1024) {
                g_sock_stream[r] = ((ty & 0xf) == SOCK_STREAM && (int)a0 == AF_INET);
                g_lo_port[r] = 0;
                g_br_port[r] = 0;
                g_br_ip[r] = 0;
            }
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 199: {
        int sv[2];
        // socketpair (translate Linux domain -> macOS)
        int r = socketpair(af_l2m((int)a0), (int)a1 & 0xf, (int)a2, sv);
        if (r == 0) {
            ((int *)a3)[0] = sv[0];
            ((int *)a3)[1] = sv[1];
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // bind -- port-map: bind the published host port
    case 200: {
        // GUEST Linux sockaddr_in: family@0(u16 LE), port@2(BE)
        uint8_t *sa = (uint8_t *)a1;
        if (lo_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] &&
            // private loopback
            lo_is(sa, (socklen_t)a2)) {
            uint16_t p = ntohs(*(uint16_t *)(sa + 2));
            if (p == 0) p = lo_alloc_ephemeral(); // bind(:0) -> a real, round-trippable port
            char up[200];
            lo_path(p, up, sizeof up);
            if (lo_swap((int)a0) < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
            }
            unlink(up);
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = bind((int)a0, (struct sockaddr *)&un, sizeof un);
            if (r == 0) g_lo_port[(int)a0] = p ? p : 1;
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        // NET bridge: bind(0.0.0.0 / own-ip / in-subnet :port) -> LISTEN on /tmp/.ddbr-<netid>/<ownip>:<port>
        if (br_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] &&
            br_bind_is(sa, (socklen_t)a2)) {
            uint16_t p = ntohs(*(uint16_t *)(sa + 2));
            if (p == 0) p = br_alloc_ephemeral(); // bind(:0) -> a real, round-trippable port
            char up[200];
            br_path(g_myip, p, up, sizeof up); // we always listen on OUR endpoint IP
            if (lo_swap((int)a0) < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
            }
            unlink(up);
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = bind((int)a0, (struct sockaddr *)&un, sizeof un);
            if (r == 0) {
                g_br_port[(int)a0] = p ? p : 1;
                g_br_ip[(int)a0] = g_myip;
            }
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        // abstract AF_UNIX (sun_path[0]==0): macOS has no abstract ns -> bind a real fs socket keyed by
        // DD_NETNS. Must run BEFORE any general AF_UNIX passthrough below.
        if (abs_is(sa, (socklen_t)a2)) {
            char up[200];
            abs_path(sa, (socklen_t)a2, up, sizeof up);
            unlink(up); // replace stale (cf. lo_/br_ above)
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = bind((int)a0, (struct sockaddr *)&un, sizeof un);
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        if (g_nportmap && sa && a2 >= 8 && *(uint16_t *)(sa + 0) == AF_INET) {
            uint16_t cp = ntohs(*(uint16_t *)(sa + 2)), hp = pm_host(cp);
            // remember for getsockname
            if ((int)a0 >= 0 && (int)a0 < 1024) g_fd_cport[(int)a0] = cp;
            if (hp != cp) {
                uint8_t buf[128];
                socklen_t L = a2 < 128 ? (socklen_t)a2 : 128;
                memcpy(buf, sa, L);
                // publish on :H instead of :C (port @2)
                *(uint16_t *)(buf + 2) = htons(hp);
                // Linux->macOS sockaddr translation (sin_len/family) before the real host bind.
                struct sockaddr_storage ss;
                socklen_t hl = sa_l2m(buf, L, &ss);
                int br = (hl != (socklen_t)-1) ? bind((int)a0, (struct sockaddr *)&ss, hl)
                                               : bind((int)a0, (struct sockaddr *)buf, L);
                G_RET(c) = br < 0 ? (uint64_t)(-errno) : 0;
                break;
            }
        }
        // Real host bind: translate Linux AF_INET/INET6 sockaddr -> macOS (sin_len/family); AF_UNIX
        // and others pass through unchanged. (Was: raw bind of the Linux struct -> AF_UNSPEC bind.)
        {
            struct sockaddr_storage ss;
            socklen_t hl = sa_l2m(sa, (socklen_t)a2, &ss);
            int br = (hl != (socklen_t)-1) ? bind((int)a0, (struct sockaddr *)&ss, hl)
                                           : bind((int)a0, (void *)a1, (socklen_t)a2);
            G_RET(c) = br < 0 ? (uint64_t)(-errno) : 0;
        }
        break;
    }
    case 201: G_RET(c) = listen((int)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0; break;
    case 202:
    case 242: {
        int lfd = (int)a0;
        // accept / accept4
        int pl = (lfd >= 0 && lfd < 1024) ? g_lo_port[lfd] : 0;
        int pbr = (lfd >= 0 && lfd < 1024) ? g_br_port[lfd] : 0;
        uint32_t pbrip = (lfd >= 0 && lfd < 1024) ? g_br_ip[lfd] : 0;
        // Real host accept writes a macOS sockaddr; receive into a host scratch then translate the
        // peer addr back to Linux layout for the guest. (private-lo / bridge: don't expose unix peer.)
        struct sockaddr_storage hss;
        socklen_t hsl = sizeof hss;
        int want_peer = (!pl && !pbr && a1);
        int r = (pl || pbr) ? accept(lfd, NULL, NULL)
                            : accept(lfd, want_peer ? (struct sockaddr *)&hss : NULL,
                                     want_peer ? &hsl : (socklen_t *)a2);
        if (r >= 0) {
            if (nr == 242) {
                if ((int)a3 & 0x800) fcntl(r, F_SETFL, fcntl(r, F_GETFL) | O_NONBLOCK);
                if ((int)a3 & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC);
            }
            if (want_peer) {
                socklen_t gcap = a2 ? *(socklen_t *)a2 : 0;
                int ll = sa_m2l((struct sockaddr *)&hss, (uint8_t *)a1, gcap);
                if (ll < 0) { // non-inet peer (e.g. AF_UNIX): copy raw host bytes
                    socklen_t n = hsl < gcap ? hsl : gcap;
                    if (gcap) memcpy((void *)a1, &hss, n);
                    if (a2) *(socklen_t *)a2 = hsl;
                } else if (a2)
                    *(socklen_t *)a2 = (socklen_t)ll;
            }
            if (pl) {
                if (r < 1024) {
                    g_lo_port[r] = pl;
                    g_sock_stream[r] = 1;
                }
                fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, pl);
            } else if (pbr) {
                if (r < 1024) {
                    g_br_port[r] = pbr;
                    g_br_ip[r] = pbrip;
                    g_sock_stream[r] = 1;
                }
                // peer reported as our virtual listen addr (cf. lo_* simplification)
                fill_inet_br((uint8_t *)a1, (socklen_t *)a2, pbrip, pbr);
            }
        // peer = 127.0.0.1:lport
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // connect
    case 203: {
        // --network none: no external egress (DD_NET_ISOLATE). Loopback is redirected by the lo_* path
        // below; any non-127/8 AF_INET destination is refused, matching docker's null network.
        static int net_isolate = -1;
        if (net_isolate < 0) net_isolate = getenv("DD_NET_ISOLATE") != NULL;
        uint8_t *sa = (uint8_t *)a1;
        if (net_isolate && sa && (socklen_t)a2 >= 8 && *(uint16_t *)(sa + 0) == AF_INET &&
            (ntohl(*(uint32_t *)(sa + 4)) >> 24) != 127) {
            G_RET(c) = (uint64_t)(-ENETUNREACH);
            break;
        }
        if (lo_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] &&
            // private loopback
            lo_is(sa, (socklen_t)a2)) {
            uint16_t p = ntohs(*(uint16_t *)(sa + 2));
            char up[200];
            lo_path(p, up, sizeof up);
            if (lo_swap((int)a0) < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
            }
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = connect((int)a0, (struct sockaddr *)&un, sizeof un);
            if (r == 0 || errno == EINPROGRESS) g_lo_port[(int)a0] = p ? p : 1;
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        // NET bridge: connect(peer-ip:port in our subnet) -> dial /tmp/.ddbr-<netid>/<peerip>:<port>
        if (br_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] &&
            br_connect_is(sa, (socklen_t)a2)) {
            uint32_t dip = *(uint32_t *)(sa + 4);
            uint16_t p = ntohs(*(uint16_t *)(sa + 2));
            char up[200];
            br_path(dip, p, up, sizeof up);
            if (lo_swap((int)a0) < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
            }
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = connect((int)a0, (struct sockaddr *)&un, sizeof un);
            if (r == 0 || errno == EINPROGRESS) {
                g_br_port[(int)a0] = p ? p : 1;
                g_br_ip[(int)a0] = dip; // peer ip for getpeername
            }
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        // abstract AF_UNIX (sun_path[0]==0): dial the same DD_NETNS-keyed fs socket bind used. Must run
        // BEFORE the general AF_UNIX passthrough below.
        if (abs_is(sa, (socklen_t)a2)) {
            char up[200];
            abs_path(sa, (socklen_t)a2, up, sizeof up);
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = connect((int)a0, (struct sockaddr *)&un, sizeof un);
            G_RET(c) = (r < 0 && errno != EINPROGRESS) ? (uint64_t)(-errno) : 0;
            break;
        }
        // Real host connect: translate Linux AF_INET/INET6 sockaddr -> macOS; others pass through.
        {
            struct sockaddr_storage ss;
            socklen_t hl = sa_l2m(sa, (socklen_t)a2, &ss);
            int cr = (hl != (socklen_t)-1) ? connect((int)a0, (struct sockaddr *)&ss, hl)
                                           : connect((int)a0, (void *)a1, (socklen_t)a2);
            G_RET(c) = cr < 0 ? (uint64_t)(-errno) : 0;
        }
        break;
    }
    case 204: {
        // getsockname
        int fd = (int)a0;
        if (fd >= 0 && fd < 1024 && g_lo_port[fd]) {
            fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, g_lo_port[fd]);
            G_RET(c) = 0;
            break;
        }
        if (fd >= 0 && fd < 1024 && g_br_port[fd]) {
            fill_inet_br((uint8_t *)a1, (socklen_t *)a2, g_br_ip[fd], g_br_port[fd]);
            G_RET(c) = 0;
            break;
        }
        // Real host getsockname returns a macOS sockaddr; receive into host scratch, translate back to
        // Linux layout for the guest (fixes sin_family/sin_len), preserving the portmap port rewrite.
        struct sockaddr_storage hss;
        socklen_t hsl = sizeof hss;
        int r = getsockname(fd, (struct sockaddr *)&hss, &hsl);
        if (r == 0 && a1) {
            socklen_t gcap = a2 ? *(socklen_t *)a2 : 0;
            int ll = sa_m2l((struct sockaddr *)&hss, (uint8_t *)a1, gcap);
            if (ll < 0) {
                socklen_t n = hsl < gcap ? hsl : gcap;
                if (gcap) memcpy((void *)a1, &hss, n);
                if (a2) *(socklen_t *)a2 = hsl;
            } else {
                if (a2) *(socklen_t *)a2 = (socklen_t)ll;
                if (g_nportmap && fd >= 0 && fd < 1024 && g_fd_cport[fd] && gcap >= 4)
                    // app sees the port it asked for (port @2)
                    *(uint16_t *)((uint8_t *)a1 + 2) = htons(g_fd_cport[fd]);
            }
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 205: {
        // getpeername
        int fd = (int)a0;
        if (fd >= 0 && fd < 1024 && g_lo_port[fd]) {
            fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, g_lo_port[fd]);
            G_RET(c) = 0;
            break;
        }
        if (fd >= 0 && fd < 1024 && g_br_port[fd]) {
            fill_inet_br((uint8_t *)a1, (socklen_t *)a2, g_br_ip[fd], g_br_port[fd]);
            G_RET(c) = 0;
            break;
        }
        // Real host getpeername: translate macOS sockaddr back to Linux layout for the guest.
        struct sockaddr_storage hss;
        socklen_t hsl = sizeof hss;
        int r = getpeername(fd, (struct sockaddr *)&hss, &hsl);
        if (r == 0 && a1) {
            socklen_t gcap = a2 ? *(socklen_t *)a2 : 0;
            int ll = sa_m2l((struct sockaddr *)&hss, (uint8_t *)a1, gcap);
            if (ll < 0) {
                socklen_t n = hsl < gcap ? hsl : gcap;
                if (gcap) memcpy((void *)a1, &hss, n);
                if (a2) *(socklen_t *)a2 = hsl;
            } else if (a2)
                *(socklen_t *)a2 = (socklen_t)ll;
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 206: {
        // MSG_NOSIGNAL(0x4000) has no per-call equivalent on macOS; emulate it with the SO_NOSIGPIPE
        // socket option so the send returns EPIPE instead of raising a fatal SIGPIPE.
        if ((int)a3 & 0x4000) { int on = 1; setsockopt((int)a0, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on); }
        // dest addr (UDP): translate Linux AF_INET/INET6 sockaddr -> macOS; NULL/non-inet pass through.
        struct sockaddr_storage dss;
        socklen_t dhl = a4 ? sa_l2m((uint8_t *)a4, (socklen_t)a5, &dss) : (socklen_t)-1;
        const void *dst = (dhl != (socklen_t)-1) ? (void *)&dss : (void *)a4;
        socklen_t dl = (dhl != (socklen_t)-1) ? dhl : (socklen_t)a5;
        ssize_t r = sendto((int)a0, (void *)a1, (size_t)a2, msgflags_l2m((int)a3), dst, dl);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 207: {
        // src addr: receive into host scratch (macOS layout) then translate back to Linux for the guest.
        struct sockaddr_storage hss;
        socklen_t hsl = sizeof hss;
        int want = a4 != 0;
        ssize_t r = recvfrom((int)a0, (void *)a1, (size_t)a2, msgflags_l2m((int)a3),
                             want ? (struct sockaddr *)&hss : NULL, want ? &hsl : NULL);
        if (r >= 0 && want) {
            socklen_t gcap = a5 ? *(socklen_t *)a5 : 0;
            int ll = sa_m2l((struct sockaddr *)&hss, (uint8_t *)a4, gcap);
            if (ll < 0) {
                socklen_t n = hsl < gcap ? hsl : gcap;
                if (gcap) memcpy((void *)a4, &hss, n);
                if (a5) *(socklen_t *)a5 = hsl;
            } else if (a5)
                *(socklen_t *)a5 = (socklen_t)ll;
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // setsockopt(fd, level, optname, val, len)
    case 208: {
        int lvl = (int)a1, opt = (int)a2;
        if (lvl == 1) {
            lvl = SOL_SOCKET;
            opt = so_opt_l2m((int)a2);
            if (opt < 0) {
                G_RET(c) = 0;
                break;
            }
        // translate SOL_SOCKET; ignore unknown
        }
        int r = setsockopt((int)a0, lvl, opt, (void *)a3,
                           // other levels (TCP/IP) pass through (TCP_NODELAY matches)
                           (socklen_t)a4);
        G_RET(c) = r < 0 ? 0 : 0;
        (void)r;
        // never fail the guest on an unsupported option
        break;
    }
    // getsockopt(fd, level, optname, val, len)
    case 209: {
        int lvl = (int)a1, opt = (int)a2;
        if (lvl == 1) {
            lvl = SOL_SOCKET;
            opt = so_opt_l2m((int)a2);
            if (opt < 0) {
                if (a4 && *(socklen_t *)a4 >= 4 && a3) *(int *)a3 = 0;
                G_RET(c) = 0;
                break;
            }
        // unknown -> report 0
        }
        int r = getsockopt((int)a0, lvl, opt, (void *)a3, (socklen_t *)a4);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 210:
        G_RET(c) = shutdown((int)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0;
        // shutdown(fd, how) -- SHUT_RD/WR/RDWR match
        break;
    case 211:
    // sendmsg/recvmsg -- translate Linux msghdr -> macOS
    case 212: {
        uint8_t *g = (uint8_t *)a1;
        struct msghdr mh;
        // Linux: iovlen/controllen are 8-byte; macOS 4
        memset(&mh, 0, sizeof mh);
        mh.msg_name = (void *)*(uint64_t *)(g + 0);
        mh.msg_namelen = *(uint32_t *)(g + 8);
        mh.msg_iov = (void *)*(uint64_t *)(g + 16);
        mh.msg_iovlen = (int)*(uint64_t *)(g + 24);
        mh.msg_flags = *(uint32_t *)(g + 48);
        // msg_name sockaddr: Linux<->macOS translation through a host scratch (AF_INET/INET6 only).
        struct sockaddr_storage nss;
        uint8_t *gname = (uint8_t *)mh.msg_name;
        socklen_t gnamelen = mh.msg_namelen;
        if (nr == 211 && gname && gnamelen) { // sendmsg: guest -> host
            socklen_t hl = sa_l2m(gname, gnamelen, &nss);
            if (hl != (socklen_t)-1) {
                mh.msg_name = &nss;
                mh.msg_namelen = hl;
            }
        } else if (nr == 212 && gname && gnamelen) { // recvmsg: receive into host scratch
            mh.msg_name = &nss;
            mh.msg_namelen = sizeof nss;
        }
        // Ancillary data: the guest control buf is Linux-cmsg layout; macOS reads a different cmsghdr,
        // so route it through a host-layout scratch buffer (NULL-control left untouched, so edge/msgflags
        // with no control buffer stays on the old path).
        uint8_t *gc = (void *)*(uint64_t *)(g + 32);
        size_t gcl = *(uint64_t *)(g + 40);
        uint8_t hctl[4096]; // host-layout scratch (macOS hdr is smaller, so this is ample)
        if (nr == 211) {    // sendmsg: translate guest -> host before the call
            ssize_t hn = (gc && gcl) ? cmsg_l2m(gc, gcl, hctl, sizeof hctl) : 0;
            mh.msg_control = hn > 0 ? hctl : NULL;
            mh.msg_controllen = hn > 0 ? (socklen_t)hn : 0;
        } else { // recvmsg: receive into host scratch
            mh.msg_control = (gc && gcl) ? hctl : NULL;
            mh.msg_controllen = (gc && gcl) ? (socklen_t)sizeof hctl : 0;
        }
        // MSG_NOSIGNAL(0x4000) -> SO_NOSIGPIPE (macOS has no per-call flag); EPIPE instead of SIGPIPE.
        if (nr == 211 && ((int)a2 & 0x4000)) { int on = 1; setsockopt((int)a0, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on); }
        ssize_t r =
            (nr == 211) ? sendmsg((int)a0, &mh, msgflags_l2m((int)a2)) : recvmsg((int)a0, &mh, msgflags_l2m((int)a2));
        if (nr == 212 && r >= 0) {
            // recvmsg writes back name len + (host->guest) control + translated flags
            if (gname && gnamelen) { // translate received host sockaddr back to Linux layout
                int ll = sa_m2l((struct sockaddr *)&nss, gname, gnamelen);
                *(uint32_t *)(g + 8) = (ll >= 0) ? (uint32_t)ll : mh.msg_namelen;
                if (ll < 0 && mh.msg_namelen) // non-inet: copy raw host bytes back
                    memcpy(gname, &nss, mh.msg_namelen < gnamelen ? mh.msg_namelen : gnamelen);
            } else
                *(uint32_t *)(g + 8) = mh.msg_namelen;
            size_t ln = (gc && gcl) ? (size_t)cmsg_m2l(&mh, gc, gcl) : 0;
            *(uint64_t *)(g + 40) = ln;
            *(uint32_t *)(g + 48) = (uint32_t)msgflags_m2l((int)mh.msg_flags);
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 269:
    // sendmmsg/recvmmsg(fd, mmsghdr[], vlen, flags, [timeout])
    case 243: {
        uint8_t *vec = (uint8_t *)a1;
        unsigned vlen = (unsigned)a2;
        // mmsghdr = msghdr(56) + msg_len(4) + pad
        int done = 0, err = 0;
        // MSG_NOSIGNAL(0x4000) -> SO_NOSIGPIPE once before the fan-out (macOS has no per-call flag).
        if (nr == 269 && ((int)a3 & 0x4000)) { int on = 1; setsockopt((int)a0, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on); }
        for (unsigned i = 0; i < vlen; i++) {
            uint8_t *g = vec + (size_t)i * 64;
            struct msghdr mh;
            memset(&mh, 0, sizeof mh);
            mh.msg_name = (void *)*(uint64_t *)(g + 0);
            mh.msg_namelen = *(uint32_t *)(g + 8);
            mh.msg_iov = (void *)*(uint64_t *)(g + 16);
            mh.msg_iovlen = (int)*(uint64_t *)(g + 24);
            mh.msg_flags = *(uint32_t *)(g + 48);
            // msg_name sockaddr: Linux<->macOS translation through a host scratch (AF_INET/INET6 only).
            struct sockaddr_storage nss;
            uint8_t *gname = (uint8_t *)mh.msg_name;
            socklen_t gnamelen = mh.msg_namelen;
            if (nr == 269 && gname && gnamelen) { // sendmmsg: guest -> host
                socklen_t hl = sa_l2m(gname, gnamelen, &nss);
                if (hl != (socklen_t)-1) {
                    mh.msg_name = &nss;
                    mh.msg_namelen = hl;
                }
            } else if (nr == 243 && gname && gnamelen) { // recvmmsg: receive into host scratch
                mh.msg_name = &nss;
                mh.msg_namelen = sizeof nss;
            }
            // Ancillary data: route the per-submessage control buf through a host-layout scratch buffer.
            uint8_t *gc = (void *)*(uint64_t *)(g + 32);
            size_t gcl = *(uint64_t *)(g + 40);
            uint8_t hctl[4096];
            if (nr == 269) { // sendmmsg: translate guest -> host
                ssize_t hn = (gc && gcl) ? cmsg_l2m(gc, gcl, hctl, sizeof hctl) : 0;
                mh.msg_control = hn > 0 ? hctl : NULL;
                mh.msg_controllen = hn > 0 ? (socklen_t)hn : 0;
            } else { // recvmmsg: receive into host scratch
                mh.msg_control = (gc && gcl) ? hctl : NULL;
                mh.msg_controllen = (gc && gcl) ? (socklen_t)sizeof hctl : 0;
            }
            int rf = (int)a3;
            // after the first, don't block (MSG_WAITFORONE-ish)
            if (nr == 243 && i > 0) rf |= 0x40;
            ssize_t r = (nr == 269) ? sendmsg((int)a0, &mh, msgflags_l2m(rf)) : recvmsg((int)a0, &mh, msgflags_l2m(rf));
            if (r < 0) {
                err = errno;
                break;
            }
            // msg_len
            *(uint32_t *)(g + 56) = (uint32_t)r;
            if (nr == 243) {
                if (gname && gnamelen) { // translate received host sockaddr back to Linux layout
                    int ll = sa_m2l((struct sockaddr *)&nss, gname, gnamelen);
                    *(uint32_t *)(g + 8) = (ll >= 0) ? (uint32_t)ll : mh.msg_namelen;
                    if (ll < 0 && mh.msg_namelen)
                        memcpy(gname, &nss, mh.msg_namelen < gnamelen ? mh.msg_namelen : gnamelen);
                } else
                    *(uint32_t *)(g + 8) = mh.msg_namelen;
                size_t ln = (gc && gcl) ? (size_t)cmsg_m2l(&mh, gc, gcl) : 0;
                *(uint64_t *)(g + 40) = ln;
                *(uint32_t *)(g + 48) = (uint32_t)msgflags_m2l((int)mh.msg_flags);
            }
            done++;
        }
        G_RET(c) = (done == 0 && err) ? (uint64_t)(-(int64_t)err) : (uint64_t)done;
        break;
    }

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
            g_eventfd_sema[fds[0]] = (a1 & 1) != 0;   // EFD_SEMAPHORE
            g_eventfd_count[fds[0]] = a0;             // initval
            if (a0 > 0) { char b = 1; if (write(fds[1], &b, 1) < 0) {} } // make it readable
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
            memcpy(&data, (void *)(a3 + 4), 8);
        // struct epoll_event {u32 events; u64 data} packed
        }
        // op: 1=ADD 2=DEL 3=MOD ; EPOLLET=0x80000000 -> EV_CLEAR ; EPOLLONESHOT=0x40000000 -> EV_ONESHOT
        uint16_t xf = (uint16_t)((ev & 0x80000000u ? EV_CLEAR : 0) | (ev & 0x40000000u ? EV_ONESHOT : 0));
        int want_rd = (op != 2) && (ev & 0x1);   // EPOLLIN
        int want_wr = (op != 2) && (ev & 0x4);   // EPOLLOUT
        if (epopt_on() && (int)a0 >= 0 && (int)a0 < 1024 && fd >= 0 && fd < 1024) {
            // W3E fast path: track armed filters, defer the change to the next epoll_wait kevent().
            int ep = (int)a0;
            if (want_rd) { ep_push(ep, fd, EVFILT_READ, EV_ADD | xf, (void *)data); g_ep_rd[fd] = 1; }
            else if (g_ep_rd[fd]) { ep_push(ep, fd, EVFILT_READ, EV_DELETE, (void *)data); g_ep_rd[fd] = 0; }
            if (want_wr) { ep_push(ep, fd, EVFILT_WRITE, EV_ADD | xf, (void *)data); g_ep_wr[fd] = 1; }
            else if (g_ep_wr[fd]) { ep_push(ep, fd, EVFILT_WRITE, EV_DELETE, (void *)data); g_ep_wr[fd] = 0; }
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
        int r = kevent(ep, chg, nchg, kv, maxev, tp);
        ep_count();
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
            *(uint32_t *)(out + oi * 12) = ev;
            memcpy(out + oi * 12 + 4, &kv[i].udata, 8);
            // EPOLLONESHOT: the kernel auto-removed this registration; keep our armed map in sync.
            if (opt && kv[i].ident < 1024 && g_ep_os[kv[i].ident]) {
                if (kv[i].filter == EVFILT_READ) g_ep_rd[kv[i].ident] = 0;
                else if (kv[i].filter == EVFILT_WRITE) g_ep_wr[kv[i].ident] = 0;
            }
            oi++;
        }
        // Safety net: if the combined call returned only change-errors (no readiness) yet the guest
        // asked to block, honor the wait with a clean no-change kevent so it can't busy-spin.
        if (opt && oi == 0 && r > 0 && nchg > 0 && (int)a3 != 0) {
            int r2 = kevent(ep, NULL, 0, kv, maxev, tp);
            ep_count();
            if (r2 < 0) { G_RET(c) = (uint64_t)(-errno); break; }
            for (int i = 0; i < r2 && oi < maxev; i++) {
                if (kv[i].flags & EV_ERROR) continue;
                uint32_t ev = (kv[i].filter == EVFILT_READ) ? 0x1u : (kv[i].filter == EVFILT_WRITE) ? 0x4u : 0u;
                if (kv[i].flags & EV_EOF) ev |= 0x10u;
                *(uint32_t *)(out + oi * 12) = ev;
                memcpy(out + oi * 12 + 4, &kv[i].udata, 8);
                if (kv[i].ident < 1024 && g_ep_os[kv[i].ident]) {
                    if (kv[i].filter == EVFILT_READ) g_ep_rd[kv[i].ident] = 0;
                    else if (kv[i].filter == EVFILT_WRITE) g_ep_wr[kv[i].ident] = 0;
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
        const char *p = atpath(-100, (const char *)a1, pb, sizeof pb);
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
        if (a4) { ts = *(struct timespec *)a4; tsp = &ts; }
        int r = pselect((int)a0, (fd_set *)a1, (fd_set *)a2, (fd_set *)a3, tsp, NULL);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 73: {
        struct pollfd *fds = (void *)a0;
        // ppoll -> poll
        struct timespec *ts = (void *)a2;
        int tmo = ts ? (int)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000) : -1;
        int r = poll(fds, (nfds_t)a1, tmo);
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

    // ===================== Misc — uname/sysinfo/getrandom/hostname =====================
    case 160: {
        char *u = (char *)a0;
        // uname
        memset(u, 0, 6 * 65);
        strcpy(u, "Linux");
        strcpy(u + 65, g_hostname[0] ? g_hostname : "jit");
        strcpy(u + 130, "6.1.0");
        strcpy(u + 195, "#1 jit");
        strcpy(u + 260, "aarch64");
        G_RET(c) = 0;
        break;
    }
    case 161: {
        int n = (int)a1;
        if (n > 64) n = 64;
        if (n > 0) {
            memcpy(g_hostname, (void *)a0, n);
            g_hostname[n] = 0;
        // sethostname (UTS ns)
        }
        G_RET(c) = 0;
        break;
    }
    // setdomainname -> ignore
    case 162: G_RET(c) = 0; break;
    case 179:
        memset((void *)a0, 0, 112);
        G_RET(c) = 0;
        // sysinfo
        break;
    case 278:
        arc4random_buf((void *)a0, (size_t)a1);
        G_RET(c) = a1;
        // getrandom
        break;
    case 293:
        G_RET(c) = (uint64_t)(-ENOSYS);
        // rseq -> ENOSYS (glibc falls back)
        break;

    // ===================== SysV shared memory (per-container key namespace) =====================
    case 194: { // shmget(key, size, shmflg)
        int r = shmget(ipc_ns_key((key_t)a0), (size_t)a1, (int)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 196: { // shmat(shmid, shmaddr, shmflg) -- the guest runs in-process so the host map is usable
        void *p = shmat((int)a0, (const void *)a1, (int)a2);
        G_RET(c) = (p == (void *)-1) ? (uint64_t)(-errno) : (uint64_t)p;
        break;
    }
    case 197: { // shmdt(shmaddr)
        int r = shmdt((const void *)a0);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 195: { // shmctl(shmid, cmd, buf): IPC_RMID supported; STAT/SET deferred (macOS struct differs)
        if ((int)a1 == IPC_RMID) {
            int r = shmctl((int)a0, IPC_RMID, NULL);
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        } else {
            G_RET(c) = (uint64_t)(-ENOSYS);
        }
        break;
    }

    // ===================== SysV semaphores =====================
    case 190: { // semget(key, nsems, semflg)
        int r = semget(ipc_ns_key((key_t)a0), (int)a1, (int)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 192: // semtimedop -> semop (glibc routes semop() through it; macOS has no timed variant)
    case 193: { // semop(semid, sops, nsops) -- struct sembuf is layout-compatible with the guest's
        int r = semop((int)a0, (struct sembuf *)a1, (size_t)a2);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 191: { // semctl(semid, semnum, cmd, arg)
        int lc = (int)a2, mc = sem_cmd_l2m(lc), r;
        union semun_ { int val; struct semid_ds *buf; unsigned short *array; } arg;
        if (lc == 16) { arg.val = (int)a3; r = semctl((int)a0, (int)a1, mc, arg); }                       // SETVAL
        else if (lc == 13 || lc == 17) { arg.array = (unsigned short *)a3; r = semctl((int)a0, (int)a1, mc, arg); } // GET/SETALL
        else if (lc == 0 || lc == 11 || lc == 12 || lc == 14 || lc == 15) { r = semctl((int)a0, (int)a1, mc); }    // RMID/GETPID/GETVAL/GETNCNT/GETZCNT
        else { G_RET(c) = (uint64_t)(-ENOSYS); break; }                                                   // IPC_STAT/SET: struct differs
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }

    // ===================== SysV message queues =====================
    case 186: { // msgget(key, msgflg)
        int r = msgget(ipc_ns_key((key_t)a0), (int)a1);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 189: { // msgsnd(msqid, msgp, msgsz, msgflg) -- msgbuf {long mtype; char mtext[]} is compatible
        int r = msgsnd((int)a0, (const void *)a1, (size_t)a2, (int)a3);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 188: { // msgrcv(msqid, msgp, msgsz, msgtyp, msgflg)
        ssize_t r = msgrcv((int)a0, (void *)a1, (size_t)a2, (long)a3, (int)a4);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 187: { // msgctl(msqid, cmd, buf): IPC_RMID supported; STAT/SET deferred (macOS struct differs)
        if ((int)a1 == IPC_RMID) {
            int r = msgctl((int)a0, IPC_RMID, NULL);
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        } else {
            G_RET(c) = (uint64_t)(-ENOSYS);
        }
        break;
    }

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
    // flock(fd, op): macOS flock(2); Linux LOCK_SH/EX/UN/NB op values match the host
    case 32: G_RET(c) = flock((int)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0; break;
    // preadv/pwritev: struct iovec layout is identical Linux<->macOS
    case 69: {
        ssize_t r = preadv((int)a0, (const struct iovec *)a1, (int)a2, (off_t)a3);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 70: {
        ssize_t r = pwritev((int)a0, (const struct iovec *)a1, (int)a2, (off_t)a3);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // setsid(): new session / process-group leader
    case 157: { pid_t s = setsid(); G_RET(c) = s < 0 ? (uint64_t)(-errno) : (uint64_t)s; break; }
    // scheduling: stub with sane SCHED_OTHER values (real-time priorities aren't offered)
    case 118: // sched_setparam
    case 119: G_RET(c) = 0; break;                            // sched_setscheduler -> ok (ignored)
    case 120: G_RET(c) = 0; break;                            // sched_getscheduler -> SCHED_OTHER(0)
    case 121: if (a1) *(int *)a1 = 0; G_RET(c) = 0; break;    // sched_getparam -> priority 0
    case 125: G_RET(c) = (a0 == 1 || a0 == 2) ? 99 : 0; break; // sched_get_priority_max: FIFO/RR=99 else 0
    case 126: G_RET(c) = 0; break;                            // sched_get_priority_min -> 0
    case 127: // sched_rr_get_interval -> a nominal 100ms slice
        if (a1) { ((struct timespec *)a1)->tv_sec = 0; ((struct timespec *)a1)->tv_nsec = 100000000L; }
        G_RET(c) = 0; break;
    // mlockall/munlockall: no macOS equivalent; the guest's "don't swap" intent is a safe no-op
    case 230:
    case 231: G_RET(c) = 0; break;
    // getitimer/setitimer: wrap the host (ITIMER_* + struct itimerval layouts match Linux<->macOS)
    case 102: G_RET(c) = getitimer((int)a0, (struct itimerval *)a1) < 0 ? (uint64_t)(-errno) : 0; break;
    case 103:
        G_RET(c) = setitimer((int)a0, (const struct itimerval *)a1, (struct itimerval *)a2) < 0
                       ? (uint64_t)(-errno) : 0;
        break;
    case 84: G_RET(c) = s3db_sync_fd((int)a0); break; // sync_file_range -> fsync, durability policy
    case 112: G_RET(c) = (uint64_t)(-1); break; // clock_settime: container has no CAP_SYS_TIME -> EPERM
    case 143: G_RET(c) = setregid((gid_t)a0, (gid_t)a1) < 0 ? (uint64_t)(-errno) : 0; break; // setregid
    case 151: G_RET(c) = (uint64_t)cuid(); break; // setfsuid -> previous fsuid (container uid)
    case 152: G_RET(c) = (uint64_t)cgid(); break; // setfsgid -> previous fsgid
    case 168: // getcpu(cpu, node, tcache) -> cpu 0 / node 0
        if (a0) *(unsigned *)a0 = 0;
        if (a1) *(unsigned *)a1 = 0;
        G_RET(c) = 0; break;
    case 213: G_RET(c) = 0; break; // readahead: advisory, no-op
    case 274: G_RET(c) = 0; break; // sched_setattr -> ok (ignored)
    // preadv2/pwritev2: flags (a5) ignored; offset in a3 (pos_high a4 is 0 on LP64)
    case 286: {
        ssize_t r = preadv((int)a0, (const struct iovec *)a1, (int)a2, (off_t)a3);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 287: {
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
                *(int *)(gi + 0) = 17;            // si_signo = Linux SIGCHLD
                *(int *)(gi + 4) = 0;             // si_errno
                *(int *)(gi + 8) = code;          // si_code (CLD_* values match Linux<->macOS)
                *(int *)(gi + 16) = (int)si.si_pid;
                *(int *)(gi + 20) = (int)si.si_uid;
                *(int *)(gi + 24) = status;       // si_status
            }
        }
        G_RET(c) = 0;
        break;
    }
    // truncate(path, length): resolve the guest path through the overlay (same helper execve uses), then
    // truncate by host path. Evict the stat cache so the new size is observed.
    case 45: {
        char pb[4200];
        const char *p = xresolve_overlay((const char *)a0, pb, sizeof pb);
        int r = truncate(p, (off_t)a1);
        if (r >= 0) mc_evict(p);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // getrlimit(resource, rlim) / setrlimit(resource, rlim): alias prlimit64 (case 261). RLIMIT_STACK(3)
    // reports 8MB, everything else unlimited; setrlimit is accepted (no-op).
    case 163: {
        if (a1) {
            uint64_t *o = (uint64_t *)a1;
            o[0] = ((int)a0 == 3) ? (8ull << 20) : ~0ull; // rlim_cur
            o[1] = ~0ull;                                 // rlim_max
        }
        G_RET(c) = 0;
        break;
    }
    case 164: G_RET(c) = 0; break; // setrlimit -> accepted

    // clock_adjtime(clk_id, timex): container has no CAP_SYS_TIME -> EPERM (mirrors clock_settime case 112)
    case 266: G_RET(c) = (uint64_t)(-1); break;
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
            g_sigcode[sig] = *(int *)(a3 + 8);     // siginfo.si_code
            g_sigval[sig] = *(uint64_t *)(a3 + 24); // siginfo.si_value (sival_int/ptr)
        }
        raise_guest_signal(c, sig);
        G_RET(c) = 0;
        break;
    }

    // ===================== unhandled =====================
    default:
        fprintf(stderr, "[jit] unhandled syscall %llu (a0=%llx a1=%llx) at pc=%llx\n", (unsigned long long)nr,
                (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)G_PC(c));
        G_RET(c) = (uint64_t)(-ENOSYS);
        // ENOSYS, keep going so we can see what's next
        break;
    }
    // Boundary errno translation: every case sets G_RET(c) to a host(macOS) errno on error
    // (-errno, saved e, helper returns, or a macOS E* constant). Map to the Linux errno the guest
    // expects. Skip redirect (sigreturn restored an already-Linux x0 from the signal frame).
    if (!c->redirect) {
        int64_t rv = (int64_t)G_RET(c);
        if (rv < 0 && rv >= -4095) G_RET(c) = (uint64_t)(-(int64_t)m2l_errno((int)(-rv)));
    }
}
