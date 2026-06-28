// Extracted service() helper block: file-scope globals + static helper functions used by service().
// Not standalone -- #included by ../service.c after its system headers and before static void service().
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
