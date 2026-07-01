// dd/runtime/os/linux/container -- the container VFS: TOCTOU-free path jail, overlay image layers
// (lower/upper + copy-up + whiteout + merged readdir), and /proc + /sys synthesis.

// ---- rootfs path rewriting (ported from mac_elf.c) ----
static const char *g_rootfs = NULL;
// guest CWD (within the rootfs) -- AT_FDCWD resolution + getcwd
static char g_cwd[4200] = "/";
static uint8_t g_auxv_data[1024];
// serialized auxv for /proc/self/auxv
static int g_auxv_len;
// Guest main-thread stack bounds, published by build_stack. Used to synthesize a [stack] line in
// /proc/self/maps so glibc's pthread_getattr_np(pthread_self()) finds the main stack (it scans the
// maps for the line containing %rsp). Without it that call returns ENOENT, which derails Rust std's
// startup (stack-overflow guard init) and cascades into wrong behavior later. 0 => not published yet.
uint64_t g_stack_lo, g_stack_hi;
// Sandbox: normalize a guest absolute path -- drop '.', collapse '//', and clamp '..' at the
// ROOT so a translated path can never escape the rootfs ("/../../etc" -> "/etc"). Without this,
// the guest reads host files by traversing above $rootfs. Result always starts with '/'.
static void confine(const char *p, char *out, size_t n) {
    const char *comp[512];
    int clen[512], nc = 0;
    for (const char *s = p; *s;) {
        while (*s == '/')
            s++;
        if (!*s) break;
        const char *st = s;
        while (*s && *s != '/')
            s++;
        int L = (int)(s - st);
        // "."  -> skip
        if (L == 1 && st[0] == '.') continue;
        if (L == 2 && st[0] == '.' && st[1] == '.') {
            if (nc > 0) nc--;
            continue;
        // ".." -> pop, never past root
        }
        if (nc < 512) {
            comp[nc] = st;
            clen[nc] = L;
            nc++;
        }
    }
    size_t o = 0;
    for (int i = 0; i < nc; i++) {
        if (o + 1 < n) out[o++] = '/';
        for (int j = 0; j < clen[i] && o + 1 < n; j++)
            out[o++] = comp[i][j];
    }
    // empty -> "/"
    if (o == 0 && n > 1) out[o++] = '/';
    out[o < n ? o : n - 1] = 0;
}
// Guest chroot(2) prefix: a rootfs-relative guest path ("" = none). chroot(2) re-roots the guest WITHIN
// the existing rootfs jail -- its target is resolved through the jail first (so it can never name a host
// path) and recorded here; every absolute guest path is then walked under this prefix yet STILL confined
// to g_root_fd, so a guest can never reach the host fs (a `..` still clamps at the rootfs root). Inherited
// across fork and preserved across execve, exactly as on Linux.
static char g_chroot[4200];
// Re-root an absolute guest path under the active chroot: clamp its `..` (after chroot the guest's own
// root IS the chroot dir) and prepend the prefix. The result is still a rootfs-absolute guest path, which
// the resolvers below confine to g_root_fd as usual. Callers invoke this only while a chroot is active.
static void chroot_apply(const char *guest, char *out, size_t n) {
    char norm[4200];
    confine(guest ? guest : "/", norm, sizeof norm);
    if (!g_chroot[0])
        snprintf(out, n, "%s", norm);
    else if (norm[1] == 0)
        snprintf(out, n, "%s", g_chroot); // the chroot root itself
    else
        snprintf(out, n, "%s%s", g_chroot, norm);
}
// Strip the active chroot prefix from a rootfs-relative guest path, yielding the chroot-relative view the
// guest sees (used to keep g_cwd in the guest's own frame after chdir under a chroot). No-op with no
// chroot, or for a path that lies outside the chroot subtree (clamped to "/" -- the guest cannot be there).
static void chroot_strip(char *guest, size_t n) {
    if (!g_chroot[0] || !guest || guest[0] != '/') return;
    size_t cl = strlen(g_chroot);
    if (strncmp(guest, g_chroot, cl) == 0 && (guest[cl] == '/' || guest[cl] == 0)) {
        char tmp[4200];
        snprintf(tmp, sizeof tmp, "%s", guest[cl] ? guest + cl : "/");
        snprintf(guest, n, "%s", tmp);
    } else {
        snprintf(guest, n, "/");
    }
}
// realpath(g_rootfs) -- the true rootfs boundary
static char g_rootfs_canon[4200];
static size_t g_rootfs_canon_len;
// fd -> host path it was opened with (dir-fd confinement + cache)
static char g_fdpath[1024][192];
// overlay: dir-fd -> its GUEST path (for merged getdents); "" = not an overlay dir
static char g_ovldir[1024][192];
// eventfd(read-end) -> pipe write-end + 1 (0 = not an eventfd)
static int g_eventfd_peer[1024];
// eventfd accumulating counter: write() adds, read() returns + resets (the pipe is only readiness).
// _xproc-eventfd-lockf_: the counter array lives in a MAP_SHARED anonymous region so a child created by
// dd's real host fork() updates the SAME physical counters the parent reads -- the readiness pipe is
// already fork-shared (inherited fds), but the accumulating count must be too, or the parent reads 0
// while the child's write()s land in its COW-private copy. Created ONCE at startup (constructor, before
// any guest fork) so every forked worker inherits the same physical array. All g_eventfd_count[fd]
// indexing (io.c, the eventfd2 creation in service.c) is unchanged.
static uint64_t *g_eventfd_count;
static void eventfd_count_init(void) {
    if (g_eventfd_count) return;
    size_t sz = sizeof(uint64_t) * 1024;
    void *mem = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) // cross-process counters degrade, but in-process eventfd still works
        mem = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED) abort();
    g_eventfd_count = (uint64_t *)mem;
}
__attribute__((constructor)) static void eventfd_count_ctor(void) { eventfd_count_init(); }
static uint8_t g_eventfd_sema[1024]; // EFD_SEMAPHORE: read() returns 1 and decrements by 1, not the whole counter

// ===================== in-memory temp-file backing (sqlite sorter/index spill) =====================
// A genuinely-PRIVATE scratch file is served from a host RAM buffer instead of issuing pread/pwrite to
// a host temp file. SQLite's sorter/index spill ("etilqs_*") opens O_RDWR|O_CREAT|O_EXCL under the temp
// dir and unlink()s it IMMEDIATELY while still open (delete-on-close), and glibc/rustix also use
// O_TMPFILE. Once a regular file has been unlinked while open with link count 0 it has NO name and CANNOT
// be reached by any other path -> it is private scratch, exactly equivalent to an anonymous memfd, so it
// is safe to back with RAM (this is the same anonymity O_TMPFILE has from birth).
//
// PLUMBING: the guest fd stays a REAL host fd (a created-then-unlinked regular file), so the fd NUMBER,
// poll/select/epoll readiness, fcntl, and fork inheritance all behave exactly like a normal file. The RAM
// buffer is a transparent write-back cache: read/write/pread/pwrite/lseek/ftruncate/fstat/fsync on the fd
// hit RAM (memcpy), turning the per-block host I/O syscalls into memory copies. On ANY operation that
// could let another observer see the bytes through the real fd -- dup/sendfile/splice/copy_file_range,
// mmap, an SCM_RIGHTS send, a /proc/self/fd reopen, fork, or execve -- we first "materialize" (flush the
// RAM buffer back into the real fd, restore its size+offset) and drop the cache, after which the fd is an
// ordinary host file and behaves identically to the unoptimized path. This materialize-on-escape rule is
// the bit-exact safety argument: backing a file changes only WHERE the bytes live, never any observable
// byte/size/seek/stat result.
//
// KILL SWITCH: NOTMPFS=1 disables all backing (pure host-file behaviour). BOUND: a file larger than
// MEMF_CAP, or once the process-wide RAM total would exceed MEMF_TOTAL_CAP, is materialized and spills to
// the real host file (host I/O resumes) -- RAM use is bounded, never unbounded.
#define MEMF_CAP (256ull * 1024 * 1024)        // per-file RAM cap; beyond this, spill to the host file
#define MEMF_TOTAL_CAP (1024ull * 1024 * 1024) // process-wide RAM cap for all backed files
struct memf {
    uint8_t *buf;
    size_t size; // logical file size (bytes)
    size_t cap;  // allocated bytes of buf
    off_t pos;   // current file offset (for read/write/lseek SEEK_CUR)
};
static struct memf *g_memf[1024];
static _Atomic uint64_t g_memf_total; // sum of logical sizes of all backed files

static int memf_disabled(void) {
    static int v = -1;
    if (v < 0) v = getenv("NOTMPFS") ? 1 : 0;
    return v;
}
static inline struct memf *memf_get(int fd) { return (fd >= 0 && fd < 1024) ? g_memf[fd] : NULL; }

// grow buf to >= need bytes, zero-filling the new tail (so a sparse write reads back as zeros).
static int memf_reserve(struct memf *m, size_t need) {
    if (need <= m->cap) return 0;
    size_t nc = m->cap ? m->cap : 65536;
    while (nc < need) nc = nc < (16u << 20) ? nc << 1 : nc + (16u << 20); // double, then +16MiB chunks
    uint8_t *nb = realloc(m->buf, nc);
    if (!nb) return -1;
    memset(nb + m->cap, 0, nc - m->cap);
    m->buf = nb;
    m->cap = nc;
    return 0;
}
// Attach a RAM cache to real host fd `fd`, slurping `init` bytes already present in the fd. Returns 1 if
// backed, 0 if left as a plain host fd (kill switch / over cap / OOM). The fd becomes anonymous.
static int memf_attach(int fd, off_t init, off_t pos) {
    if (memf_disabled() || fd < 0 || fd >= 1024 || g_memf[fd]) return 0;
    if (init < 0 || (uint64_t)init > MEMF_CAP) return 0;
    if (atomic_load(&g_memf_total) + (uint64_t)init > MEMF_TOTAL_CAP) return 0;
    struct memf *m = calloc(1, sizeof *m);
    if (!m) return 0;
    if (init > 0) {
        if (memf_reserve(m, (size_t)init)) { free(m); return 0; }
        off_t got = 0;
        for (off_t o = 0; o < init;) { // slurp existing bytes from the real fd into RAM
            ssize_t r = pread(fd, m->buf + o, (size_t)(init - o), o);
            if (r <= 0) break;
            o += r;
            got = o;
        }
        if (got != init) { // unreadable fd / short read: zero-filling the tail would read back as zeros and
            free(m->buf);  // a later memf_materialize would pwrite those zeros over real on-disk bytes (data
            free(m);       // loss). Abort the adoption and fall back to the plain host fd (F1).
            g_memf[fd] = NULL;
            return 0;
        }
        m->size = (size_t)init;
    }
    m->pos = pos < 0 ? 0 : pos;
    g_memf[fd] = m;
    atomic_fetch_add(&g_memf_total, (uint64_t)m->size);
    g_fdpath[fd][0] = 0; // anonymous: no tracked host path
    return 1;
}
// Flush the RAM buffer back into the real fd (size + offset restored) and drop the cache: the fd reverts
// to a plain host file behaving exactly as if it had never been backed.
static void memf_materialize(int fd) {
    struct memf *m = memf_get(fd);
    if (!m) return;
    g_memf[fd] = NULL;
    for (size_t o = 0; o < m->size;) {
        ssize_t w = pwrite(fd, m->buf + o, m->size - o, (off_t)o);
        if (w <= 0) break;
        o += (size_t)w;
    }
    if (ftruncate(fd, (off_t)m->size) < 0) {}
    lseek(fd, m->pos, SEEK_SET);
    atomic_fetch_sub(&g_memf_total, (uint64_t)m->size);
    free(m->buf);
    free(m);
}
static void memf_materialize_all(void) {
    for (int fd = 0; fd < 1024; fd++)
        if (g_memf[fd]) memf_materialize(fd);
}
static void memf_close(int fd) { // fd is being closed: just discard the RAM buffer
    struct memf *m = memf_get(fd);
    if (!m) return;
    g_memf[fd] = NULL;
    atomic_fetch_sub(&g_memf_total, (uint64_t)m->size);
    free(m->buf);
    free(m);
}
// I/O served from RAM. pread/pwrite are positional; read/write advance m->pos.
static ssize_t memf_pread(struct memf *m, void *buf, size_t n, off_t off) {
    if (off < 0) return -EINVAL;
    size_t avail = (size_t)off < m->size ? m->size - (size_t)off : 0;
    size_t k = n < avail ? n : avail;
    if (k) memcpy(buf, m->buf + off, k);
    return (ssize_t)k;
}
static ssize_t memf_pwrite(struct memf *m, const void *buf, size_t n, off_t off) {
    if (off < 0) return -EINVAL;
    size_t end = (size_t)off + n;
    if (memf_reserve(m, end)) return -ENOMEM;
    memcpy(m->buf + off, buf, n);
    if (end > m->size) {
        atomic_fetch_add(&g_memf_total, end - m->size);
        m->size = end;
    }
    return (ssize_t)n;
}
static ssize_t memf_read_pos(struct memf *m, void *buf, size_t n) {
    ssize_t k = memf_pread(m, buf, n, m->pos);
    if (k > 0) m->pos += k;
    return k;
}
static ssize_t memf_write_pos(struct memf *m, const void *buf, size_t n) {
    ssize_t k = memf_pwrite(m, buf, n, m->pos);
    if (k > 0) m->pos += k;
    return k;
}
static ssize_t memf_preadv(struct memf *m, const struct iovec *iov, int cnt, off_t off, int advance) {
    off_t p = advance ? m->pos : off;
    ssize_t tot = 0;
    for (int i = 0; i < cnt; i++) {
        ssize_t k = memf_pread(m, iov[i].iov_base, iov[i].iov_len, p);
        if (k < 0) return tot ? tot : k;
        tot += k;
        p += k;
        if ((size_t)k < iov[i].iov_len) break; // short read -> EOF
    }
    if (advance) m->pos = p;
    return tot;
}
static ssize_t memf_pwritev(struct memf *m, const struct iovec *iov, int cnt, off_t off, int advance) {
    off_t p = advance ? m->pos : off;
    ssize_t tot = 0;
    for (int i = 0; i < cnt; i++) {
        ssize_t k = memf_pwrite(m, iov[i].iov_base, iov[i].iov_len, p);
        if (k < 0) return tot ? tot : k;
        tot += k;
        p += k;
    }
    if (advance) m->pos = p;
    return tot;
}
// lseek on RAM. Returns the new offset, -1 for EINVAL, or -2 to mean "unsupported whence -> materialize".
static off_t memf_lseek(struct memf *m, off_t off, int whence) {
    off_t np;
    if (whence == 0) np = off;                  // SEEK_SET
    else if (whence == 1) np = m->pos + off;    // SEEK_CUR
    else if (whence == 2) np = (off_t)m->size + off; // SEEK_END
    else return -2;                             // SEEK_DATA/SEEK_HOLE: let the host fd handle it
    if (np < 0) return -1;
    m->pos = np;
    return np;
}
static int memf_fstat(int fd, struct stat *s) { // real-file metadata, RAM size/blocks
    if (fstat(fd, s) != 0) return -1;
    struct memf *m = g_memf[fd];
    s->st_size = (off_t)m->size;
    s->st_blocks = (blkcnt_t)((m->size + 511) / 512);
    return 0;
}
// Returns 1 if writing up to byte `end` stays within the caps; otherwise materializes fd (spills to the
// host file) and returns 0 so the caller falls through to the real host write.
static int memf_room_or_spill(int fd, off_t end) {
    struct memf *m = g_memf[fd];
    if (end < 0 || (uint64_t)end <= m->size) return 1;
    uint64_t grow = (uint64_t)end - m->size;
    if ((uint64_t)end > MEMF_CAP || atomic_load(&g_memf_total) + grow > MEMF_TOTAL_CAP) {
        memf_materialize(fd);
        return 0;
    }
    return 1;
}
// After the guest unlinked a temp file (dev/ino captured before the unlink), adopt it as RAM-backed iff
// EXACTLY ONE open fd now holds the last (zero) link to that regular file -- i.e. it is now anonymous and
// privately owned by this one description. More than one matching fd (a dup) shares an offset we don't
// model, so we leave those as a plain host file.
static void memf_try_adopt(uint64_t dev, uint64_t ino) {
    if (memf_disabled() || !ino) return;
    int found = -1;
    for (int fd = 0; fd < 1024; fd++) {
        if (g_memf[fd]) continue;
        struct stat s;
        if (fstat(fd, &s) != 0) continue;
        if ((uint64_t)s.st_dev == dev && (uint64_t)s.st_ino == ino) {
            if (found >= 0) return; // duped: shared description -> don't risk it
            found = fd;
        }
    }
    if (found < 0) return;
    struct stat s;
    if (fstat(found, &s) != 0 || !S_ISREG(s.st_mode) || s.st_nlink != 0) return;
    int fl = fcntl(found, F_GETFL); // only adopt an O_RDWR fd: a RAM cache serves both reads and writes, so
    if (fl < 0 || (fl & O_ACCMODE) != O_RDWR) return; // adopting an O_RDONLY/O_WRONLY scratch fd would accept
    memf_attach(found, s.st_size, lseek(found, 0, SEEK_CUR)); // I/O the kernel would reject with EBADF (F2).
}

#include "vfs/gmap.c"
// A non-PIE ET_EXEC is linked at a fixed low vaddr but __PAGEZERO forbids mapping there, so load_elf biases
// it high. Its un-relocated absolute refs still point at the low link range; when the guest takes an
// absolute jump there, the dispatcher redirects pc into the biased image (pc += bias) instead of faulting
// on the unmapped low address. [lo,hi) is the un-biased link span of the current main image (0 if PIE).
static uint64_t g_nonpie_lo, g_nonpie_hi, g_nonpie_bias;
// fd is a timerfd (a kqueue with an EVFILT_TIMER) -> read() drains it
static uint8_t g_timerfd[1024];
// fd is an inotify (a kqueue with EVFILT_VNODE watches) -> read() drains it
static uint8_t g_inotify[1024];
// inotify-on-a-directory emulation: kqueue says "the dir changed" but not which entry, so we keep the
// watched dir's path + a snapshot of its names and diff on read() to synthesize IN_CREATE/IN_DELETE+name.
static char g_inotify_wpath[1024][512];
static char *g_inotify_snap[1024]; // newline-joined entry names of the last snapshot (malloc'd)
// pinned O_DIRECTORY fd to the rootfs (set at startup)
static int g_root_fd = -1;
// Bind-mount volumes: a guest path prefix -> a host directory, each its own confined jail root.
struct vol {
    char guest[256];
    size_t glen;
    char hcanon[1024];
    size_t hlen;
    int fd;
    int ro; // 1 = read-only bind (`-v …:ro`): write-intent syscalls under `guest` fail EROFS
};
static struct vol g_vols[32];
static int g_nvols;
// Materialize a volume's mount point (and every ancestor) as empty dirs in the writable rootfs/upper, the
// way Docker mkdir -p's each mount target inside the container rootfs. Without it a NESTED mount leaves its
// parent absent: `-v H:/x/y` makes `/x/y` resolve to the host dir, but `ls /x` ENOENTs because `/x` exists
// in no layer. Creating /x (and /x/y) in the upper lets the merged readdir list `/x` -> `y`; the mount
// itself still wins in jail_pick(), so `/x/y` shows the host files, not the empty placeholder. The rootfs
// is the per-container overlay upper (daemon) or the plain rootfs (manual) -- both writable & private.
// No-op until the rootfs is known (the bridge sets DDVOL after container_init resolves g_rootfs_canon).
static void vol_mkmountpoint(const char *guest) {
    if (!g_rootfs_canon[0] || !guest || guest[0] != '/') return;
    char mp[4300];
    if ((size_t)snprintf(mp, sizeof mp, "%s%s", g_rootfs_canon, guest) >= sizeof mp) return;
    for (char *s = mp + g_rootfs_canon_len + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(mp, 0755);
            *s = '/';
        }
    mkdir(mp, 0755);
}
static void add_vol(const char *spec) { // "[ro:]guestpath:hostdir" -> a confined bind-mount volume
    if (g_nvols >= 32) return;
    // Optional read-only marker. A guest path always begins with '/', so a leading "ro:"/"rw:" token is
    // unambiguous; absent (the legacy `guest:host` form) it defaults to read-write -> byte-identical.
    int ro = 0;
    if (!strncmp(spec, "ro:", 3)) { ro = 1; spec += 3; }
    else if (!strncmp(spec, "rw:", 3)) { spec += 3; }
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s", spec);
    char *col = strchr(tmp, ':');
    if (!col || tmp[0] != '/') return;
    *col = 0;
    struct vol *v = &g_vols[g_nvols];
    v->ro = ro;
    snprintf(v->guest, sizeof v->guest, "%s", tmp);
    v->glen = strlen(v->guest);
    while (v->glen > 1 && v->guest[v->glen - 1] == '/') v->guest[--v->glen] = 0;
    if (!realpath(col + 1, v->hcanon)) return;
    v->hlen = strlen(v->hcanon);
    if ((v->fd = open(v->hcanon, O_RDONLY | O_DIRECTORY)) < 0) return;
    g_nvols++;
    vol_mkmountpoint(v->guest);
}
// Longest matching bind-mount volume for an absolute guest path (the DEEPEST mount wins, exactly as the
// kernel routes a path to the innermost mount), or -1 for the rootfs/overlay jail. Longest-prefix so a
// nested volume (`-v H1:/x/y -v H2:/x/y/z`) routes /x/y/z to the inner mount regardless of registration
// order; for non-nested volumes (no guest is a prefix of another) it is identical to a first-match scan.
static int jail_match(const char *abs) {
    int best = -1;
    size_t blen = 0;
    for (int i = 0; i < g_nvols; i++)
        if (g_vols[i].glen > blen && !strncmp(abs, g_vols[i].guest, g_vols[i].glen) &&
            (abs[g_vols[i].glen] == '/' || abs[g_vols[i].glen] == 0)) {
            best = i;
            blen = g_vols[i].glen;
        }
    return best;
}
// Pick the jail (rootfs or a volume) for an absolute guest path; *rel = the path within that jail.
static int jail_pick(const char *abs, const char **canon, size_t *clen, const char **rel) {
    int i = jail_match(abs);
    if (i >= 0) {
        if (canon) {
            *canon = g_vols[i].hcanon;
            *clen = g_vols[i].hlen;
        }
        *rel = abs[g_vols[i].glen] ? abs + g_vols[i].glen : "/";
        return g_vols[i].fd;
    }
    if (canon) {
        *canon = g_rootfs_canon;
        *clen = g_rootfs_canon_len;
    }
    *rel = abs;
    return g_root_fd;
}
// SECURE path resolution. confine() handles '..' lexically, but symlinks resolve in the kernel
// BELOW that layer (a mid-path symlink to '/' walks straight out), so lexical clamping is NOT a
// boundary. This realpath()s the deepest existing prefix (following ALL symlinks) and verifies
// the canonical result is inside g_rootfs_canon; anything that escapes is redirected to a
// guaranteed-nonexistent in-jail path (-> ENOENT). `nofollow` keeps the final component
// unresolved (for readlink/lstat). Returns 1 if inside the jail, 0 if an escape was blocked.
// Core: confine `rel` within an explicit jail root (jcanon). Generalized from secure_resolve so the
// overlay can resolve the SAME guest path inside each layer's root, reusing the realpath boundary.
static int confine_in(const char *jcanon, size_t jclen, const char *rel, char *out, size_t n, int nofollow) {
    char norm[4200];
    confine(rel, norm, sizeof norm);
    char h[8400];
    snprintf(h, sizeof h, "%s%s", jcanon, norm);
    char rem[4400] = "";
    // peel the final component, resolve its dir
    if (nofollow) {
        char *sl = strrchr(h, '/');
        if (sl && (size_t)(sl - h) >= jclen) {
            snprintf(rem, sizeof rem, "/%s", sl + 1);
            *sl = 0;
        }
        if (!h[0]) snprintf(h, sizeof h, "/");
    }
    for (;;) {
        char canon[4200];
        if (realpath(h, canon)) {
            int inside = strncmp(canon, jcanon, jclen) == 0 && (canon[jclen] == '/' || canon[jclen] == 0);
            if (!inside) {
                snprintf(out, n, "%s/.jail-escape-denied", jcanon);
                return 0;
            }
            snprintf(out, n, "%s%s", canon, rem);
            return 1;
        }
        // final missing? climb to the deepest existing dir
        char *sl = strrchr(h, '/');
        if (!sl || strlen(h) <= jclen) {
            snprintf(out, n, "%s/.jail-escape-denied", jcanon);
            return 0;
        }
        char tmp[4400];
        snprintf(tmp, sizeof tmp, "/%s%s", sl + 1, rem);
        snprintf(rem, sizeof rem, "%s", tmp);
        *sl = 0;
    }
}
static int secure_resolve(const char *guest, char *out, size_t n, int nofollow) {
    // Normalize '.'/'//'/'..' and clamp at the ROOTFS root FIRST, then route. Jail selection must see the
    // post-`..` path: a `..` that pops above a volume's own root crosses the bind-mount boundary back to
    // the dir holding the mount point ("/x/y/.." -> "/x"), which lives in the rootfs/overlay jail, not the
    // volume. Routing the raw path would prefix-match "/x/y/.." to the volume and clamp `..` at the volume
    // root. confine() already collapses `..` lexically below (so this only changes WHICH jail is chosen,
    // not the symlink-via-realpath confinement) and never ascends past '/', so the result stays in rootfs.
    char cr[4200];
    if (g_chroot[0]) { // re-root under the guest's chroot first (no-op cost when no chroot is in effect)
        chroot_apply(guest, cr, sizeof cr);
        guest = cr;
    }
    char norm[4200];
    confine(guest, norm, sizeof norm);
    const char *jcanon;
    size_t jclen;
    const char *rel;
    // rootfs or a volume root (jcanon is absolute)
    jail_pick(norm, &jcanon, &jclen, &rel);
    return confine_in(jcanon, jclen, rel, out, n, nofollow);
}
#include "vfs/overlay.c"
// final NOT followed (readlink/lstat)
static const char *xlate(const char *p, char *buf, size_t n) {
    if (g_rootfs && p && p[0] == '/') {
        secure_resolve(p, buf, n, 1);
        return buf;
    }
    return p;
}
// follow symlinks (open/stat/exec)
static const char *xresolve(const char *p, char *buf, size_t n) {
    if (g_rootfs && p && p[0] == '/') {
        secure_resolve(p, buf, n, 0);
        return buf;
    }
    return p;
}
// Resolve an EXEC entrypoint (or PT_INTERP) to a host path, following symlinks the way the kernel
// would INSIDE the rootfs: an absolute symlink target (`/bin/sh -> /bin/busybox`) is rootfs-relative,
// not host-relative -- realpath() can't do this (it follows the target against the host root). Each
// hop is re-confined via secure_resolve, so an escaping link lands on .jail-escape-denied and fails.
static const char *xresolve_exec(const char *p, char *buf, size_t n) {
    if (!(g_rootfs && p && p[0] == '/')) return p;
    char cur[4200];
    snprintf(cur, sizeof cur, "%s", p);
    // bounded symlink chain
    for (int i = 0; i < 40; i++) {
        char hb[4200];
        // host path, final component NOT followed
        secure_resolve(cur, hb, sizeof hb, 1);
        struct stat st;
        // missing -> let the loader report it
        if (lstat(hb, &st) != 0) break;
        if (!S_ISLNK(st.st_mode)) {
            snprintf(buf, n, "%s", hb);
            return buf;
        // real file -> done
        }
        char tgt[4200];
        ssize_t k = readlink(hb, tgt, sizeof tgt - 1);
        if (k <= 0) break;
        tgt[k] = 0;
        if (tgt[0] == '/')
            // absolute target: rootfs-relative
            snprintf(cur, sizeof cur, "%s", tgt);
        else {
            char d[4200];
            snprintf(d, sizeof d, "%s", cur);
            char *sl = strrchr(d, '/');
            if (sl) *sl = 0;
            char j[8400];
            snprintf(j, sizeof j, "%s/%s", d, tgt);
            snprintf(cur, sizeof cur, "%s", j);
        // relative to its dir
        }
    }
    secure_resolve(cur, buf, n, 0);
    // fallback: realpath-confine the last hop
    return buf;
}

// Resolve a bare program name (no '/') against the container PATH, like execvp -- docker passes `sh`,
// not `/bin/sh`. Returns a guest path ("/bin/sh") that exists in the rootfs, or `prog` unchanged.
static const char *find_in_path(const char *prog, char *gbuf, size_t n) {
    if (!prog || strchr(prog, '/')) return prog;
    static const char *const dirs[] = {"/usr/local/sbin", "/usr/local/bin", "/usr/sbin",
                                       "/usr/bin",        "/sbin",          "/bin", NULL};
    char hb[4200];
    for (int i = 0; dirs[i]; i++) {
        snprintf(gbuf, n, "%s/%s", dirs[i], prog);
        // Search the FULL overlay (upper THEN lowers), not the upper alone: a fresh container's upper is
        // empty and the program (e.g. python/node/ruby) lives only in a read-only image lower, so a bare
        // xresolve_exec ENOENTs every PATH dir -> the fallback /bin/<prog> doesn't exist -> the loader
        // reports "open: No such file". The ELF-loader sites already use the overlay resolver; this was the
        // one missed call site. No-op with no lowers (flat rootfs): xresolve_overlay == xresolve_exec there.
        if (access(xresolve_overlay(gbuf, hb, sizeof hb), X_OK) == 0) return gbuf;
    }
    snprintf(gbuf, n, "/bin/%s", prog); // not found anywhere: let the loader report the error against /bin
    return gbuf;
}
#include "vfs/resolve.c"
// ===================== /proc/[self|pid] process introspection =====================
// macOS has no /proc, so the per-process files Linux servers read are synthesized here. All of these
// answer for the GUEST's own process only -- "self", the host pid, the container pid, or init's "1".

// Back a synthesized text file with an anonymous temp fd (mkstemp + immediate unlink): the fd holds the
// content, has no name, and behaves like an ordinary read-only file. Returns the fd, or -1 on error.
static int proc_text_fd(const char *buf, int n) {
    char tn[] = "/tmp/.ddprocXXXXXX";
    int fd = mkstemp(tn);
    if (fd >= 0) {
        unlink(tn);
        if (write(fd, buf, (size_t)n) < 0) {}
        lseek(fd, 0, SEEK_SET);
    }
    return fd;
}
// The guest task name (Linux comm, max 15 chars): the basename of the running image (g_exe_path).
static void proc_comm(char *out, size_t n) {
    const char *p = (g_exe_path && g_exe_path[0]) ? g_exe_path : "init";
    const char *base = strrchr(p, '/');
    base = base ? base + 1 : p;
    if (!base[0]) base = "init";
    snprintf(out, n, "%.15s", base);
}
// If `rp` addresses THIS process -- "/proc/self/<leaf>" or "/proc/<our-pid>/<leaf>" (host pid, container
// pid, or init's "1") -- return the <leaf> tail; else NULL. Foreign pids are not introspectable.
static const char *proc_self_leaf(const char *rp) {
    if (!strncmp(rp, "/proc/self/", 11)) return rp + 11;
    if (strncmp(rp, "/proc/", 6)) return NULL;
    const char *q = rp + 6;
    int i = 0;
    while (q[i] >= '0' && q[i] <= '9' && i < 15)
        i++;
    if (i == 0 || q[i] != '/') return NULL;
    char num[16];
    memcpy(num, q, (size_t)i);
    num[i] = 0;
    int pid = atoi(num);
    if (pid != (int)getpid() && pid != container_pid()) return NULL;
    return q + i + 1;
}
// One /proc/.../maps line for [lo,hi), plus the per-region smaps fields when `smaps` is set. The smaps
// fields are what redis's COW self-test parses; rss/dirty are reported equal to the region size (a
// resident private mapping) so any field a parser looks up is present and consistent. Returns the length.
static int proc_map_region(char *b, size_t n, unsigned long lo, unsigned long hi, const char *name, int smaps) {
    unsigned long kb = (hi - lo) / 1024;
    int m = snprintf(b, n, "%012lx-%012lx rw-p 00000000 00:00 0 %*s%s\n", lo, hi, name[0] ? 20 : 0, "", name);
    if (smaps)
        m += snprintf(b + m, (size_t)n - (size_t)m,
                      "Size:%15lu kB\nKernelPageSize:%6d kB\nMMUPageSize:%9d kB\n"
                      "Rss:%16lu kB\nPss:%16lu kB\nShared_Clean:%7d kB\nShared_Dirty:%7d kB\n"
                      "Private_Clean:%6d kB\nPrivate_Dirty:%6lu kB\nReferenced:%9lu kB\n"
                      "Anonymous:%10lu kB\nAnonHugePages:%6d kB\nSwap:%15d kB\nLocked:%13d kB\n"
                      "VmFlags: rd wr mr mw me ac\n",
                      kb, 4, 4, kb, kb, 0, 0, 0, kb, kb, kb, 0, 0, 0);
    return m;
}
// Synthesize /proc/[pid]/maps (smaps=0) or /proc/[pid]/smaps (smaps=1) from the tracked guest mappings
// (g_gmap) plus the published main-stack bounds. The [stack] line (with a guard line below it, as the
// kernel shows) is what glibc's pthread_getattr_np scans for; the region list makes the file non-empty
// and parseable. Returns an anonymous fd holding the content, or -1 on error.
static int proc_maps_fd(int smaps) {
    char tn[] = "/tmp/.ddprocXXXXXX";
    int fd = mkstemp(tn);
    if (fd < 0) return -1;
    unlink(tn);
    char b[768];
    if (g_stack_hi) {
        unsigned long lo = (unsigned long)g_stack_lo, hi = (unsigned long)g_stack_hi;
        int m = snprintf(b, sizeof b, "%012lx-%012lx ---p 00000000 00:00 0 \n", lo > 0x1000 ? lo - 0x1000 : 0, lo);
        if (write(fd, b, (size_t)m) < 0) {}
        m = proc_map_region(b, sizeof b, lo, hi, "[stack]", smaps);
        if (write(fd, b, (size_t)m) < 0) {}
    }
    for (int i = 0; i < g_ngmap; i++) {
        unsigned long lo = (unsigned long)g_gmap[i].addr, hi = lo + (unsigned long)g_gmap[i].len;
        if (g_stack_hi && lo >= (unsigned long)g_stack_lo && hi <= (unsigned long)g_stack_hi)
            continue; // already emitted as [stack]
        int m = proc_map_region(b, sizeof b, lo, hi, "", smaps);
        if (write(fd, b, (size_t)m) < 0) {}
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}
// /proc/[pid]/status -- the Name:/State:/VmRSS: key:value format (NOT the stat one-liner). VmRSS/VmSize
// reflect the cgroup memory charge so a reader sees a plausible footprint.
static int proc_status_text(char *b, size_t n) {
    char comm[16];
    proc_comm(comm, sizeof comm);
    int pid = container_pid();
    int ppid = pid == 1 ? 0 : (int)getppid();
    unsigned long rss = (unsigned long)(atomic_load(&g_mem_charged) / 1024);
    unsigned long vsz = g_mem_max ? (unsigned long)(g_mem_max / 1024) : rss + 4096;
    if (vsz < rss) vsz = rss;
    return snprintf(b, n,
                    "Name:\t%s\nUmask:\t0022\nState:\tR (running)\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\n"
                    "TracerPid:\t0\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nFDSize:\t256\nGroups:\t\n"
                    "VmPeak:\t%8lu kB\nVmSize:\t%8lu kB\nVmLck:\t       0 kB\nVmHWM:\t%8lu kB\nVmRSS:\t%8lu kB\n"
                    "VmData:\t%8lu kB\nVmStk:\t     132 kB\nVmExe:\t     512 kB\nVmLib:\t    2048 kB\nVmPTE:\t      32 kB\n"
                    "VmSwap:\t       0 kB\nThreads:\t1\nSigQ:\t0/31000\nSigPnd:\t0000000000000000\n"
                    "SigBlk:\t0000000000000000\nSigIgn:\t0000000000000000\nSigCgt:\t0000000000000000\n"
                    "Cpus_allowed:\t1\nCpus_allowed_list:\t0\nvoluntary_ctxt_switches:\t1\n"
                    "nonvoluntary_ctxt_switches:\t0\n",
                    comm, pid, pid, ppid, vsz, vsz, rss, rss, rss);
}
// /proc/[pid]/stat -- the 52-field single line (pid (comm) state ppid ...). Field 23 = vsize (bytes),
// field 24 = rss (pages); the rest are plausible zeros. mongod's FTDC collector parses this.
static int proc_stat_text(char *b, size_t n) {
    char comm[16];
    proc_comm(comm, sizeof comm);
    int pid = container_pid();
    int ppid = pid == 1 ? 0 : (int)getppid();
    long pg = sysconf(_SC_PAGESIZE);
    unsigned long pgsz = pg > 0 ? (unsigned long)pg : 4096;
    unsigned long rss_pg = (unsigned long)(atomic_load(&g_mem_charged)) / pgsz;
    unsigned long vsize = g_mem_max ? (unsigned long)g_mem_max : rss_pg * pgsz + (1ul << 20);
    return snprintf(b, n,
                    "%d (%s) R %d %d %d 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 1 0 100 %lu %lu 18446744073709551615 "
                    "0 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                    pid, comm, ppid, pid, pid, vsize, rss_pg);
}
// /proc/[pid]/environ -- the guest environment as NUL-separated KEY=VALUE. The authoritative source is
// DD_GUEST_ENV (the container env the daemon forwards, "K=V\nK=V"); absent it (manual/direct mode), fall
// back to the same defaults build_stack hands the guest. Returns the byte count written.
static int proc_environ_text(char *b, size_t n) {
    int o = 0;
    const char *ge = getenv("DD_GUEST_ENV");
    if (ge && ge[0]) {
        for (const char *s = ge; *s;) {
            const char *e = s;
            while (*e && *e != '\n')
                e++;
            int L = (int)(e - s);
            if (o + L + 1 > (int)n) break;
            memcpy(b + o, s, (size_t)L);
            o += L;
            b[o++] = 0;
            s = *e ? e + 1 : e;
        }
    } else {
        static const char *const def[] = {"PATH=/usr/bin:/bin", "HOME=/root", "TERM=dumb", "LANG=C", NULL};
        for (int i = 0; def[i]; i++) {
            int L = (int)strlen(def[i]);
            if (o + L + 1 > (int)n) break;
            memcpy(b + o, def[i], (size_t)L);
            o += L;
            b[o++] = 0;
        }
    }
    return o;
}
// A synthesized /proc/<pid>/fd directory is backed by a REAL temp dir of "N -> target" symlinks, so the
// guest's opendir/getdents enumerate it through the ordinary fdopendir path and readlink/lstat of an
// entry resolves the symlink. The dir persists until the guest closes its fd; we reap it lazily on the
// next open (when the tracked fd is no longer open) and fully at exit.
static struct {
    int fd;
    char path[32];
} g_procfd_dirs[64];
static void procfd_dir_rm(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.' && (!e->d_name[1] || (e->d_name[1] == '.' && !e->d_name[2]))) continue;
            char p[64];
            snprintf(p, sizeof p, "%s/%s", path, e->d_name);
            unlink(p);
        }
        closedir(d);
    }
    rmdir(path);
}
static void procfd_dirs_reap(int force) {
    for (int i = 0; i < 64; i++) {
        if (!g_procfd_dirs[i].path[0]) continue;
        if (force || fcntl(g_procfd_dirs[i].fd, F_GETFD) == -1) {
            procfd_dir_rm(g_procfd_dirs[i].path);
            g_procfd_dirs[i].path[0] = 0;
        }
    }
}
static void procfd_dirs_atexit(void) { procfd_dirs_reap(1); }
// Build the temp dir of fd symlinks and return its fd. The guest fd numbers ARE the host fd numbers here,
// so this process's open fds are exactly the guest's; each link's target is the fd's path (or an
// anon_inode placeholder for a pipe/socket/eventfd with no path). -1 on error.
static int proc_fd_dir_open(void) {
    static int registered = 0;
    if (!registered) {
        atexit(procfd_dirs_atexit);
        registered = 1;
    }
    procfd_dirs_reap(0);
    char tmpl[] = "/tmp/.ddfddirXXXXXX";
    if (!mkdtemp(tmpl)) return -1;
    for (int fd = 0; fd < 1024; fd++) {
        if (fcntl(fd, F_GETFD) == -1) continue; // not open
        char tgt[4200];
        if (fcntl(fd, F_GETPATH, tgt) == 0 && tgt[0]) {
            if (g_rootfs && !strncmp(tgt, g_rootfs_canon, g_rootfs_canon_len)) {
                const char *g = tgt + g_rootfs_canon_len;
                if (!g[0]) g = "/";
                memmove(tgt, g, strlen(g) + 1);
            }
        } else {
            snprintf(tgt, sizeof tgt, "anon_inode:[%d]", fd);
        }
        char link[64];
        snprintf(link, sizeof link, "%s/%d", tmpl, fd);
        if (symlink(tgt, link) != 0) {}
    }
    int d = open(tmpl, O_RDONLY | O_DIRECTORY);
    if (d < 0) {
        procfd_dir_rm(tmpl);
        return -1;
    }
    for (int i = 0; i < 64; i++)
        if (!g_procfd_dirs[i].path[0]) {
            g_procfd_dirs[i].fd = d;
            snprintf(g_procfd_dirs[i].path, sizeof g_procfd_dirs[i].path, "%s", tmpl);
            break;
        }
    return d;
}
// Real macOS stat -> Linux struct stat (the fake S_IFCHR version corrupted libc buffering).
// fill_linux_stat (the guest struct-stat layout) is per-arch -> frontend/<arch>/fill_stat.c
// Synthesize the common /proc files Linux programs read (macOS has no /proc). Returns an fd
// holding the content, -1 on mkstemp error, or -2 if rp isn't a path we synthesize.
static int proc_open(const char *rp) {
    char buf[8192];
    int n = -1;
    // Per-process files for the guest's own pid: /proc/[self|pid]/{fd,maps,smaps,status,stat,environ}.
    const char *leaf = proc_self_leaf(rp);
    if (leaf) {
        if (!strcmp(leaf, "fd")) return proc_fd_dir_open();
        if (!strcmp(leaf, "maps") || !strcmp(leaf, "task/1/maps")) return proc_maps_fd(0);
        if (!strcmp(leaf, "smaps")) return proc_maps_fd(1);
        if (!strcmp(leaf, "status")) n = proc_status_text(buf, sizeof buf);
        else if (!strcmp(leaf, "stat")) n = proc_stat_text(buf, sizeof buf);
        else if (!strcmp(leaf, "environ")) n = proc_environ_text(buf, sizeof buf);
        if (n >= 0) return proc_text_fd(buf, n);
    }
    if (!strcmp(rp, "/proc/cpuinfo")) {
        int nc = (int)sysconf(_SC_NPROCESSORS_ONLN);
        if (nc < 1) nc = 1;
        if (nc > 64) nc = 64;
        n = 0;
        for (int i = 0; i < nc; i++)
            n += snprintf(buf + n, sizeof buf - (size_t)n,
                          "processor\t: %d\nBogoMIPS\t: 100.00\nFeatures\t: fp asimd\nCPU implementer\t: 0x61\n"
                          "CPU architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0x000\nCPU revision\t: 0\n\n",
                          i);
    } else if (!strcmp(rp, "/proc/meminfo")) {
        // reflect cgroup memory.max
        unsigned long long tot = g_mem_max ? g_mem_max / 1024 : 8388608;
        unsigned long long used = (unsigned long long)atomic_load(&g_mem_charged) / 1024;
        unsigned long long fre = tot > used ? tot - used : 0;
        n = snprintf(buf, sizeof buf,
                     "MemTotal:    %11llu kB\nMemFree:     %11llu kB\n"
                     "MemAvailable:%11llu kB\nBuffers:               0 kB\nCached:                0 kB\n"
                     "SwapTotal:             0 kB\nSwapFree:              0 kB\n",
                     tot, fre, fre);
    } else if (!strcmp(rp, "/proc/stat")) {
        n = snprintf(buf, sizeof buf, "cpu  0 0 0 0 0 0 0 0 0 0\nbtime 1700000000\nprocesses 1\n");
    } else if (!strcmp(rp, "/proc/mounts") || !strcmp(rp, "/proc/self/mounts")) {
        n = snprintf(buf, sizeof buf, "rootfs / rootfs rw 0 0\n");
    } else if (!strcmp(rp, "/proc/uptime")) {
        n = snprintf(buf, sizeof buf, "12345.00 12345.00\n");
    } else if (!strcmp(rp, "/proc/loadavg")) {
        n = snprintf(buf, sizeof buf, "0.00 0.00 0.00 1/1 1\n");
    } else if (!strcmp(rp, "/proc/sys/vm/overcommit_memory")) {
        n = snprintf(buf, sizeof buf, "0\n");
    } else if (!strcmp(rp, "/proc/sys/kernel/hostname")) {
        // UTS ns (hostname cmd reads this)
        n = snprintf(buf, sizeof buf, "%s\n", g_hostname[0] ? g_hostname : "jit");
    } else if (!strcmp(rp, "/proc/self/cgroup")) {
        // cgroup v2 unified
        n = snprintf(buf, sizeof buf, "0::/\n");
    } else if (!strcmp(rp, "/proc/version")) {
        n = snprintf(buf, sizeof buf, "Linux version 6.1.0 (ddockerd) aarch64\n");
    // cgroup v2: memory limit
    } else if (!strcmp(rp, "/sys/fs/cgroup/memory.max")) {
        if (g_mem_max)
            n = snprintf(buf, sizeof buf, "%llu\n", (unsigned long long)g_mem_max);
        else
            n = snprintf(buf, sizeof buf, "max\n");
    } else if (!strcmp(rp, "/sys/fs/cgroup/memory.current")) {
        n = snprintf(buf, sizeof buf, "%llu\n", (unsigned long long)atomic_load(&g_mem_charged));
    } else if (!strcmp(rp, "/sys/fs/cgroup/pids.max")) {
        if (g_pids_max)
            n = snprintf(buf, sizeof buf, "%d\n", g_pids_max);
        else
            n = snprintf(buf, sizeof buf, "max\n");
    } else if (!strcmp(rp, "/sys/fs/cgroup/pids.current")) {
        n = snprintf(buf, sizeof buf, "%d\n", atomic_load(&g_pids_cur));
    }
    if (n < 0) return -2;
    return proc_text_fd(buf, n);
}
// Linux-layout stat for a synthesized /proc or /sys file (so stat()/access() see it -- find, du,
// container runtimes that stat /etc/mtab -> /proc/mounts, JVM that stats cgroup files, etc.).
static void fill_linux_stat(uint8_t *d, const struct stat *s, const char *hostpath, int fd);
// -> macOS struct stat for a synth file
static int synth_stat_raw(const char *gp, struct stat *s) {
    if (!gp || (strncmp(gp, "/proc/", 6) && strncmp(gp, "/sys/fs/cgroup/", 15))) return 0;
    // /proc/<pid>/fd is a directory and /proc/<pid>/fd/N is a symlink -- answer these directly so stat()
    // sees the right type WITHOUT proc_open() materializing a temp dir as a stat side effect.
    const char *leaf = proc_self_leaf(gp);
    if (leaf) {
        if (!strcmp(leaf, "fd")) {
            memset(s, 0, sizeof *s);
            s->st_mode = S_IFDIR | 0555;
            s->st_nlink = 2;
            return 1;
        }
        if (!strncmp(leaf, "fd/", 3) && leaf[3]) {
            int isnum = 1;
            for (const char *t = leaf + 3; *t; t++)
                if (*t < '0' || *t > '9') isnum = 0;
            if (isnum) {
                int fdn = atoi(leaf + 3);
                char tgt[4200];
                memset(s, 0, sizeof *s);
                s->st_mode = S_IFLNK | 0777;
                s->st_nlink = 1;
                s->st_size = (fcntl(fdn, F_GETPATH, tgt) == 0 && tgt[0]) ? (off_t)strlen(tgt) : 64;
                return 1;
            }
        }
    }
    int fd = proc_open(gp);
    // -2 (not synth) or mkstemp fail
    if (fd < 0) return 0;
    if (fstat(fd, s) != 0) {
        close(fd);
        return 0;
    }
    close(fd);
    s->st_mode = S_IFREG | 0444;
    // present as a readable regular file
    s->st_nlink = 1;
    return 1;
}
// -> Linux struct stat buffer
static int synth_stat(const char *gp, uint8_t *out) {
    struct stat s;
    if (!synth_stat_raw(gp, &s)) return 0;
    fill_linux_stat(out, &s, NULL, -1); // synth /proc /sys file: no host backing -> no guest-chown xattr
    return 1;
}
