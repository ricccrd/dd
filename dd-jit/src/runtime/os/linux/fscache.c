// dd/runtime/os/linux -- ELF loader fwd-decls + the FS-metadata cache (stat/access/readlink memoized).

static void load_elf(const char *path, struct loaded *out);
static int elf_interp(const char *path, char *out, size_t n);
static uint64_t build_stack(int argc, char **argv, struct loaded *lm, uint64_t at_base);

// Linux AT_FDCWD(-100) -> macOS AT_FDCWD; real dir-fds pass through unchanged.
#define ATFD(a) (((int)(a) == -100) ? AT_FDCWD : (int)(a))
// ---- S2 path-resolution cache (forward decls; impl after mc_hash, which it reuses) ----
// Memoizes the absolute guest-path -> resolved host-path STRING only (the real syscall still
// runs on the result, so existence/contents are never cached). A global epoch -- bumped by
// service.c on every FS-namespace mutation -- invalidates the whole cache; rc_reset() hard-clears
// it in the fork child so a child never serves the parent's stale mappings. Kill: DD_NOPATHCACHE=1.
static int rc_lookup(const char *g, char *out, size_t n);
static void rc_store(const char *g, const char *host);
static void res_bump(void);
static void rc_reset(void);
// ---- W4D openat resolution cache (forward decls; impl after the S2 rc_* cache it extends) ----
// Extends item-9's rc_* (which memoizes only the read-only atpath() resolver) to the open-heavy half:
// guest-abs-path -> canonical symlink-free host path, so a repeated open collapses the TOCTOU-safe
// per-component jail walk to a single open(host, O_NOFOLLOW). Shares g_res_epoch. Kill: W4_NOOPENCACHE=1.
static int oc_lookup(const char *g, char *out, size_t n);
static void oc_store(const char *g, const char *host);
static void oc_reset(void);
// Rewrite ABSOLUTE guest paths into the rootfs; relative paths pass through (resolved
// against the dir-fd by the *at syscall, e.g. ls stat-ing entries relative to a dir).
// nofollow=1 leaves the FINAL component unresolved (lstat/AT_SYMLINK_NOFOLLOW unlink), so a
// symlink is stat'd/removed as the link itself rather than its target.
static const char *atpath(int dirfd, const char *raw, char *buf, size_t n, int nofollow) {
    if (!raw) return raw;
    // POSIX shm + named semaphores: glibc backs both with files under /dev/shm (shm_open -> /dev/shm/<name>,
    // sem_open -> /dev/shm/sem.<name>). The rootfs has no tmpfs, so route EVERY op (open/link/unlink/stat/
    // rename) at a stable host file -- the SAME mapping the open(2) handler uses (case 56) -- so glibc's
    // multi-step named-sem create (temp file + link to the final name) and sem_unlink all resolve together.
    if (raw[0] == '/' && !strncmp(raw, "/dev/shm/", 9)) {
        int m = snprintf(buf, n, "/tmp/.ddshm-%s", raw + 9);
        if (m > (int)n - 1) m = (int)n - 1;
        for (int i = 12; i < m; i++)
            if (buf[i] == '/') buf[i] = '_';
        return buf;
    }
    // absolute -> rootfs-relative + confine (final component followed unless nofollow)
    if (raw[0] == '/') {
        // S2: serve the memoized host path (only when a rootfs is configured -- without one the
        // resolvers below return `raw` untouched and leave `buf` garbage, so there's nothing to cache).
        // Follow-path only: the rc_* cache memoizes followed results, so a nofollow lookup must bypass it.
        if (!nofollow && g_rootfs && rc_lookup(raw, buf, n)) return buf;
        if (g_nlower) {
            overlay_resolve(raw, buf, n, nofollow);
            if (!nofollow && g_rootfs) rc_store(raw, buf);
            return buf;
            // overlay: search upper+lowers
        }
        if (g_rootfs) {
            if (nofollow)
                xlate(raw, buf, n);
            else {
                xresolve_exec(raw, buf, n);
                rc_store(raw, buf);
            }
            return buf;
        }
        return nofollow ? xlate(raw, buf, n) : xresolve_exec(raw, buf, n);
    }
    if (!g_rootfs) return raw;
    // relative via a real dir-fd
    if (dirfd >= 0) {
        // untracked dir-fd (dup/inherited/high): FAIL CLOSED
        if (dirfd >= 1024 || !g_fdpath[dirfd][0]) {
            snprintf(buf, n, "%s/.jail-escape-denied", g_rootfs_canon);
            return buf;
        }
        // turn it into a confined absolute path
        const char *gdir = g_fdpath[dirfd];
        if (strncmp(gdir, g_rootfs_canon, g_rootfs_canon_len) == 0)
            // upper -> guest dir
            gdir += g_rootfs_canon_len;
        else
            for (int i = 0; i < g_nlower; i++)
                if (strncmp(gdir, g_lower[i].canon, g_lower[i].clen) == 0) {
                    gdir += g_lower[i].clen;
                    break;
                    // a lower -> guest dir
                }
        char combined[8400];
        snprintf(combined, sizeof combined, "/%s/%s", gdir, raw);
        if (g_nlower) {
            overlay_resolve(combined, buf, n, nofollow);
            return buf;
        }
        // openat then ignores dirfd (path absolute)
        return nofollow ? xlate(combined, buf, n) : xresolve(combined, buf, n);
    }
    {
        char j[8400];
        // AT_FDCWD-relative -> join the guest cwd, then confine
        snprintf(j, sizeof j, "%s/%s", g_cwd, raw);
        if (g_nlower) {
            overlay_resolve(j, buf, n, nofollow);
            return buf;
        }
        return nofollow ? xlate(j, buf, n) : xresolve_exec(j, buf, n);
    }
}

// ---- FS-metadata cache ----
// Container processes (ld.so, shells, build tools) hammer redundant stat() on
// read-only image layers; the runtime owns the syscall stream, so it can answer
// from cache. Precise invalidation: record fd->path on open, evict that path's
// entry on write/truncate/create. Single-threaded only (no cross-thread races).
#define MCACHE_N 8192
static struct mcent {
    uint64_t hash;
    char path[192];
    int rc;
    struct stat st;
} g_mc[MCACHE_N];
static uint64_t mc_hash(const char *s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) {
        h ^= (uint8_t)*s;
        h *= 1099511628211ull;
    }
    return h ? h : 1;
}
static int mc_lookup(const char *p, int *rc, struct stat *out) {
    if (!p || strlen(p) >= 192) return 0;
    CLK;
    int hit = 0;
    uint64_t h = mc_hash(p);
    struct mcent *e = &g_mc[h & (MCACHE_N - 1)];
    if (e->hash == h && !strcmp(e->path, p)) {
        *rc = e->rc;
        *out = e->st;
        hit = 1;
    }
    CUL;
    return hit;
}
static void mc_store(const char *p, int rc, const struct stat *s) {
    if (!p || strlen(p) >= 192) return;
    // don't cache mutable volume paths
    if (g_nvols && strncmp(p, g_rootfs_canon, g_rootfs_canon_len)) return;
    CLK;
    uint64_t h = mc_hash(p);
    struct mcent *e = &g_mc[h & (MCACHE_N - 1)];
    e->hash = h;
    strcpy(e->path, p);
    e->rc = rc;
    e->st = *s;
    CUL;
}
static void mc_evict(const char *p) {
    if (!p || !p[0]) return;
    CLK;
    uint64_t h = mc_hash(p);
    struct mcent *e = &g_mc[h & (MCACHE_N - 1)];
    if (e->hash == h && !strcmp(e->path, p)) e->hash = 0;
    CUL;
}
// readlink cache (ld.so resolves symlinks on every library search path)
static struct rlent {
    uint64_t hash;
    char path[176];
    int rc;
    char link[200];
    int linklen;
} g_rl[2048];
static int rl_lookup(const char *p, int *rc, char *out, int bs, int *len) {
    if (!p || strlen(p) >= 176) return 0;
    CLK;
    int hit = 0;
    uint64_t h = mc_hash(p);
    struct rlent *e = &g_rl[h & 2047];
    if (e->hash == h && !strcmp(e->path, p)) {
        *rc = e->rc;
        int n = e->linklen < bs ? e->linklen : bs;
        if (e->rc >= 0) memcpy(out, e->link, n);
        *len = n;
        hit = 1;
    }
    CUL;
    return hit;
}
static void rl_store(const char *p, int rc, const char *link, int len) {
    if (!p || strlen(p) >= 176 || len > 200) return;
    CLK;
    uint64_t h = mc_hash(p);
    struct rlent *e = &g_rl[h & 2047];
    e->hash = h;
    strcpy(e->path, p);
    e->rc = rc;
    e->linklen = len;
    if (rc >= 0) memcpy(e->link, link, len);
    CUL;
}
static void rl_evict(const char *p) {
    if (!p || !p[0]) return;
    CLK;
    uint64_t h = mc_hash(p);
    struct rlent *e = &g_rl[h & 2047];
    if (e->hash == h && !strcmp(e->path, p)) e->hash = 0;
    CUL;
}
// access(F_OK) existence cache (ld.so probes every library candidate)
static struct acent {
    uint64_t hash;
    char path[176];
    int rc;
} g_ac[2048];
static int ac_lookup(const char *p, int *rc) {
    if (!p || strlen(p) >= 176) return 0;
    CLK;
    int hit = 0;
    uint64_t h = mc_hash(p);
    struct acent *e = &g_ac[h & 2047];
    if (e->hash == h && !strcmp(e->path, p)) {
        *rc = e->rc;
        hit = 1;
    }
    CUL;
    return hit;
}
static void ac_store(const char *p, int rc) {
    if (!p || strlen(p) >= 176) return;
    CLK;
    uint64_t h = mc_hash(p);
    struct acent *e = &g_ac[h & 2047];
    e->hash = h;
    strcpy(e->path, p);
    e->rc = rc;
    CUL;
}
static void ac_evict(const char *p) {
    if (!p || !p[0]) return;
    CLK;
    uint64_t h = mc_hash(p);
    struct acent *e = &g_ac[h & 2047];
    if (e->hash == h && !strcmp(e->path, p)) e->hash = 0;
    CUL;
}

// ---- S2 path-resolution cache (impl) ----
// The metadata caches above (mc_/rl_/ac_) memoize the *result* of a syscall, keyed on the resolved
// HOST path -- so the caller must FIRST pay the full atpath()/realpath()+lstat() walk to obtain that
// key. This cache fills that gap: it memoizes the walk itself (guest abs path -> host path string),
// which is a pure function of the FS namespace (dirs + symlinks). The real syscall ALWAYS still runs
// on the returned string, so a stale entry can never fabricate existence or contents -- the only
// failure mode is returning the wrong host *path*, which the epoch guard below prevents:
//   * Whole-cache invalidation by epoch: service.c bumps g_res_epoch on EVERY namespace mutation
//     (mknod/mkdir/unlink/rmdir/symlink/link/rename/(u)mount, open/openat2 w/ O_CREAT). A lookup
//     compares epochs, so every entry stamped before the mutation instantly misses. This covers all
//     stale-entry hazards conservatively (over-invalidates, never under): create-after-negative,
//     delete-after-positive, rename, mkdir/rmdir -- when in doubt we MISS and re-resolve.
//   * Hard reset on fork (rc_reset, called in the clone/clone3 child) so a child never serves a
//     mapping the parent populated before the FS diverged.
#define RCACHE_N 8192
static struct rcent {
    uint64_t hash;
    uint32_t epoch;
    uint16_t hlen; // strlen(host), cached so a hit copies via memcpy instead of re-scanning/snprintf
    char guest[200];
    char host[256];
} g_rc[RCACHE_N];
static uint32_t g_res_epoch = 1; // 0 is reserved as "never matches"
// kill switch (read once): DD_NOPATHCACHE=1 -> exact baseline resolution, no memoization.
static int res_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DD_NOPATHCACHE");
        on = (e && e[0] == '1') ? 0 : 1;
    }
    return on;
}
// Bump the epoch -> the whole cache misses. Skip 0 (the reserved "never matches" stamp).
// Locked under threads (same model as mc_*) so a bump can't race a concurrent lookup's epoch read.
static void res_bump(void) {
    CLK;
    g_res_epoch++;
    if (!g_res_epoch) g_res_epoch = 1;
    CUL;
}
// fork child: drop every inherited (COW) entry so it cannot outlive a parent-side mutation.
// This covers BOTH the path-string caches (rc_/oc_) and the metadata caches (mc_/rl_/ac_). The
// metadata caches memoize stat/readlink/access RESULTS -- including NEGATIVE (ENOENT) ones -- and a
// negative entry the parent recorded before some OTHER process created the file would otherwise be
// inherited COW and make the child read the now-existing file as missing (build systems hit this hard:
// gmake stats an output as absent, a child compiler creates it, a sibling link/stat then sees the stale
// ENOENT -> "no such file" / "No rule to make target"). The epoch only invalidates this process's own
// mutations, so it can't cover a cross-process create; dropping the inherited entries at the fork point
// makes the child re-resolve against the real FS.
static void rc_reset(void) {
    CLK;
    memset(g_rc, 0, sizeof g_rc);
    memset(g_mc, 0, sizeof g_mc);
    memset(g_rl, 0, sizeof g_rl);
    memset(g_ac, 0, sizeof g_ac);
    g_res_epoch = 1;
    oc_reset(); // W4D: drop the inherited open-resolution cache too (same COW hazard, under the same lock)
    CUL;
}
static int rc_lookup(const char *g, char *out, size_t n) {
    if (!res_enabled() || !g || g[0] != '/' || strlen(g) >= sizeof(((struct rcent *)0)->guest)) return 0;
    CLK;
    int hit = 0;
    uint64_t h = mc_hash(g);
    struct rcent *e = &g_rc[h & (RCACHE_N - 1)];
    if (e->hash == h && e->epoch == g_res_epoch && !strcmp(e->guest, g)) {
        if (e->hlen < n) {
            memcpy(out, e->host, (size_t)e->hlen + 1); // hot path: bounded copy incl. NUL, no format parse
        } else {
            memcpy(out, e->host, n - 1);
            out[n - 1] = 0;
        }
        hit = 1;
    }
    CUL;
    return hit;
}
static void rc_store(const char *g, const char *host) {
    if (!res_enabled() || !g || g[0] != '/' || !host) return;
    // over-length paths simply bypass the cache (fixed-size slot) -> re-resolved every time, safely.
    size_t hl = strlen(host);
    if (strlen(g) >= sizeof(((struct rcent *)0)->guest) || hl >= sizeof(((struct rcent *)0)->host)) return;
    CLK;
    uint64_t h = mc_hash(g);
    struct rcent *e = &g_rc[h & (RCACHE_N - 1)];
    e->hash = h;
    e->epoch = g_res_epoch; // stamp with the CURRENT epoch; a later mutation invalidates it
    e->hlen = (uint16_t)hl;
    strcpy(e->guest, g);
    memcpy(e->host, host, hl + 1);
    CUL;
}

// ---- W4D openat resolution cache (impl) ----
// item-9's rc_* cache above memoizes the atpath() resolver used by the read-only metadata syscalls
// (stat/access/readlink/exec). openat takes a DIFFERENT, TOCTOU-safe resolver -- jail_at()/resolve_at()
// walks the path one component at a time on pinned dir-fds (~6 host syscalls) so a guest can't swap a
// component for an out-of-jail symlink between check and use -- and item-9 left THAT uncached. This fills
// the gap for the open-heavy half: it memoizes the WALK as a guest-abs-path -> canonical, symlink-free
// host path obtained via F_GETPATH on a SUCCESSFUL real open. On a HIT the ~6-syscall walk collapses to a
// single open(host, O_NOFOLLOW); the REAL open ALWAYS still runs, so a stale entry can never fabricate
// existence/contents -- the only failure mode is a wrong host *path*, which the SHARED g_res_epoch guard
// prevents: every FS-namespace mutation (service.c res_bump on mknod/mkdir/unlink/rmdir/symlink/link/
// rename/(u)mount + openat O_CREAT) bumps the epoch, so the whole cache misses -- conservative
// over-invalidation, identical threat model to item 9 (the host outside the jail is not in scope; when in
// doubt we MISS and re-walk). oc_store additionally refuses to cache any host path that escaped the rootfs
// (defensive strncmp). The caller EXCLUDES O_CREAT/O_EXCL/O_TRUNC (mutating/creating) and O_DIRECTORY
// (deep-host-path reopen regressed -21%; see optimization-research/w4d-openat.md). Hard reset on fork via
// oc_reset() (from rc_reset). Kill switch (read once): W4_NOOPENCACHE=1 -> the original uncached walk.
#define OCACHE_N 8192
static struct ocent {
    uint64_t hash;
    uint32_t epoch;
    uint16_t hlen; // strlen(host), cached so a hit copies via memcpy instead of re-scanning/snprintf
    char guest[200];
    char host[256];
} g_oc[OCACHE_N];
static int oc_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("W4_NOOPENCACHE");
        on = (e && e[0] == '1') ? 0 : 1;
    }
    return on;
}
static int oc_lookup(const char *g, char *out, size_t n) {
    if (!oc_enabled() || !g || g[0] != '/' || strlen(g) >= sizeof(((struct ocent *)0)->guest)) return 0;
    CLK;
    int hit = 0;
    uint64_t h = mc_hash(g);
    struct ocent *e = &g_oc[h & (OCACHE_N - 1)];
    if (e->hash == h && e->epoch == g_res_epoch && !strcmp(e->guest, g)) {
        if (e->hlen < n) {
            memcpy(out, e->host, (size_t)e->hlen + 1); // hot path: bounded copy incl. NUL, no format parse
        } else {
            memcpy(out, e->host, n - 1);
            out[n - 1] = 0;
        }
        hit = 1;
    }
    CUL;
    return hit;
}
static void oc_store(const char *g, const char *host) {
    if (!oc_enabled() || !g || g[0] != '/' || !host) return;
    // over-length paths simply bypass the cache (fixed-size slot) -> re-walked every time, safely.
    size_t hl = strlen(host);
    if (strlen(g) >= sizeof(((struct ocent *)0)->guest) || hl >= sizeof(((struct ocent *)0)->host)) return;
    // defensive: never cache a host path that resolved OUTSIDE the rootfs jail (item-9-style confinement).
    if (g_rootfs_canon_len && strncmp(host, g_rootfs_canon, g_rootfs_canon_len)) return;
    CLK;
    uint64_t h = mc_hash(g);
    struct ocent *e = &g_oc[h & (OCACHE_N - 1)];
    e->hash = h;
    e->epoch = g_res_epoch; // stamp the CURRENT epoch; a later mutation invalidates it
    e->hlen = (uint16_t)hl;
    strcpy(e->guest, g);
    memcpy(e->host, host, hl + 1);
    CUL;
}
// fork child: drop every inherited (COW) entry so it cannot outlive a parent-side mutation. Called from
// rc_reset() which already holds the cache lock, so this does NOT re-take it (non-recursive mutex).
static void oc_reset(void) {
    memset(g_oc, 0, sizeof g_oc);
}

static void fd_setpath(int fd, const char *p) {
    if (fd >= 0 && fd < 1024 && p && strlen(p) < 192) strcpy(g_fdpath[fd], p);
}
static void fd_evict(int fd) {
    if (fd >= 0 && fd < 1024 && g_fdpath[fd][0]) mc_evict(g_fdpath[fd]);
}
static void fd_clear(int fd) {
    if (fd >= 0 && fd < 1024) g_fdpath[fd][0] = 0;
}

// A create/rename/link/symlink makes a host path appear -- or changes the identity of an existing
// one. Drop every memoized existence (mc_/ac_) and readlink (rl_) entry for it so a later stat/
// access/readlink -- including a forked child that inherited this COW cache -- re-resolves the real
// file instead of a stale NEGATIVE lookup left over from before the name existed. These metadata
// caches are precise-evict, NOT epoch-gated like the rc_/oc_ path-string caches, so every mutation
// must name the exact path whose existence it changed. Two shapes: a full host path, and the jail
// idiom of a dir-fd + final component (resolved to its host path via F_GETPATH).
static void fc_evict_path(const char *hp) {
    mc_evict(hp);
    ac_evict(hp);
    rl_evict(hp);
}
static void fc_evict_at(int pfd, const char *name) {
    char dp[4200];
    if (pfd >= 0 && fcntl(pfd, F_GETPATH, dp) == 0) {
        char hp[4400];
        snprintf(hp, sizeof hp, "%s/%s", dp, name);
        fc_evict_path(hp);
    }
}

// macOS errno -> Linux errno. They agree on 1..10 and 12..34 but diverge at 11 (EDEADLK<->EAGAIN)
// and everything >=35 (macOS EAGAIN=35 vs Linux 11, ENOSYS=78 vs 38, ELOOP=62 vs 40, ...). Every
// syscall that returns a host errno is translated at the boundary (QEMU-style). Identity outside.
static int m2l_errno(int m) {
    static const short T[107] = {0,   1,   2,   3,   4,   5,   6,   7,   8,  9,  10,  35,  12, 13,  14,  15,  16,  17,
                                 18,  19,  20,  21,  22,  23,  24,  25,  26, 27, 28,  29,  30, 31,  32,  33,  34,  11,
                                 115, 114, 88,  89,  90,  91,  92,  93,  94, 95, 96,  97,  98, 99,  100, 101, 102, 103,
                                 104, 105, 106, 107, 108, 109, 110, 111, 40, 36, 112, 113, 39, 22,  87,  122, 116, 66,
                                 22,  22,  22,  22,  22,  37,  38,  22,  22, 22, 22,  75,  22, 22,  22,  22,  125, 43,
                                 42,  84,  61,  74,  72,  61,  67,  63,  60, 71, 62,  95,  22, 131, 130, 122};
    return (m >= 0 && m < 107) ? T[m] : m;
}
