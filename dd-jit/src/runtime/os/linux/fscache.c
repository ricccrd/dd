// dd/runtime/os/linux -- ELF loader fwd-decls + the FS-metadata cache (stat/access/readlink memoized).

static void load_elf(const char *path, struct loaded *out);
static int elf_interp(const char *path, char *out, size_t n);
static uint64_t build_stack(int argc, char **argv, struct loaded *lm, uint64_t at_base);

// Linux AT_FDCWD(-100) -> macOS AT_FDCWD; real dir-fds pass through unchanged.
#define ATFD(a) (((int)(a) == -100) ? AT_FDCWD : (int)(a))
// Rewrite ABSOLUTE guest paths into the rootfs; relative paths pass through (resolved
// against the dir-fd by the *at syscall, e.g. ls stat-ing entries relative to a dir).
static const char *atpath(int dirfd, const char *raw, char *buf, size_t n) {
    if (!raw) return raw;
    if (raw[0] == '/') {                                             // absolute -> follow symlinks rootfs-relative + confine
        if (g_nlower) { overlay_resolve(raw, buf, n, 0); return buf; }   // overlay: search upper+lowers
        return xresolve_exec(raw, buf, n);
    }
    if (!g_rootfs) return raw;
    if (dirfd >= 0) {                                                // relative via a real dir-fd
        if (dirfd >= 1024 || !g_fdpath[dirfd][0]) {                  // untracked dir-fd (dup/inherited/high): FAIL CLOSED
            snprintf(buf, n, "%s/.jail-escape-denied", g_rootfs_canon); return buf;
        }
        const char *gdir = g_fdpath[dirfd];                          // turn it into a confined absolute path
        if (strncmp(gdir, g_rootfs_canon, g_rootfs_canon_len) == 0) gdir += g_rootfs_canon_len;       // upper -> guest dir
        else for (int i = 0; i < g_nlower; i++) if (strncmp(gdir, g_lower[i].canon, g_lower[i].clen) == 0) { gdir += g_lower[i].clen; break; }  // a lower -> guest dir
        char combined[8400]; snprintf(combined, sizeof combined, "/%s/%s", gdir, raw);
        if (g_nlower) { overlay_resolve(combined, buf, n, 0); return buf; }
        return xresolve(combined, buf, n);                           // openat then ignores dirfd (path absolute)
    }
    { char j[8400]; snprintf(j, sizeof j, "%s/%s", g_cwd, raw);      // AT_FDCWD-relative -> join the guest cwd, then confine
      if (g_nlower) { overlay_resolve(j, buf, n, 0); return buf; }
      return xresolve_exec(j, buf, n); }
}

// ---- FS-metadata cache ----
// Container processes (ld.so, shells, build tools) hammer redundant stat() on
// read-only image layers; the runtime owns the syscall stream, so it can answer
// from cache. Precise invalidation: record fd->path on open, evict that path's
// entry on write/truncate/create. Single-threaded only (no cross-thread races).
#define MCACHE_N 8192
static struct mcent { uint64_t hash; char path[192]; int rc; struct stat st; } g_mc[MCACHE_N];
static uint64_t g_mc_hits, g_mc_miss;                       // PROF
static uint64_t mc_hash(const char *s) { uint64_t h = 1469598103934665603ull; for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; } return h ? h : 1; }
static int mc_lookup(const char *p, int *rc, struct stat *out) {
    if (!p || strlen(p) >= 192) return 0;
    CLK; int hit = 0;
    struct mcent *e = &g_mc[mc_hash(p) & (MCACHE_N - 1)];
    if (e->hash == mc_hash(p) && !strcmp(e->path, p)) { *rc = e->rc; *out = e->st; g_mc_hits++; hit = 1; }
    CUL; return hit;
}
static void mc_store(const char *p, int rc, const struct stat *s) {
    if (!p || strlen(p) >= 192) return;
    if (g_nvols && strncmp(p, g_rootfs_canon, g_rootfs_canon_len)) return;   // don't cache mutable volume paths
    CLK;
    struct mcent *e = &g_mc[mc_hash(p) & (MCACHE_N - 1)];
    e->hash = mc_hash(p); strcpy(e->path, p); e->rc = rc; e->st = *s; g_mc_miss++;
    CUL;
}
static void mc_evict(const char *p) {
    if (!p || !p[0]) return;
    CLK;
    struct mcent *e = &g_mc[mc_hash(p) & (MCACHE_N - 1)];
    if (e->hash == mc_hash(p) && !strcmp(e->path, p)) e->hash = 0;
    CUL;
}
// readlink cache (ld.so resolves symlinks on every library search path)
static struct rlent { uint64_t hash; char path[176]; int rc; char link[200]; int linklen; } g_rl[2048];
static int rl_lookup(const char *p, int *rc, char *out, int bs, int *len) {
    if (!p || strlen(p) >= 176) return 0;
    CLK; int hit = 0;
    struct rlent *e = &g_rl[mc_hash(p) & 2047];
    if (e->hash == mc_hash(p) && !strcmp(e->path, p)) {
        *rc = e->rc; int n = e->linklen < bs ? e->linklen : bs;
        if (e->rc >= 0) memcpy(out, e->link, n); *len = n; g_mc_hits++; hit = 1;
    }
    CUL; return hit;
}
static void rl_store(const char *p, int rc, const char *link, int len) {
    if (!p || strlen(p) >= 176 || len > 200) return;
    CLK;
    struct rlent *e = &g_rl[mc_hash(p) & 2047];
    e->hash = mc_hash(p); strcpy(e->path, p); e->rc = rc; e->linklen = len; if (rc >= 0) memcpy(e->link, link, len);
    CUL;
}
static void rl_evict(const char *p) {
    if (!p || !p[0]) return; CLK; struct rlent *e = &g_rl[mc_hash(p) & 2047];
    if (e->hash == mc_hash(p) && !strcmp(e->path, p)) e->hash = 0;
    CUL;
}
// access(F_OK) existence cache (ld.so probes every library candidate)
static struct acent { uint64_t hash; char path[176]; int rc; } g_ac[2048];
static int ac_lookup(const char *p, int *rc) {
    if (!p || strlen(p) >= 176) return 0;
    CLK; int hit = 0;
    struct acent *e = &g_ac[mc_hash(p) & 2047];
    if (e->hash == mc_hash(p) && !strcmp(e->path, p)) { *rc = e->rc; g_mc_hits++; hit = 1; }
    CUL; return hit;
}
static void ac_store(const char *p, int rc) {
    if (!p || strlen(p) >= 176) return;
    CLK;
    struct acent *e = &g_ac[mc_hash(p) & 2047];
    e->hash = mc_hash(p); strcpy(e->path, p); e->rc = rc;
    CUL;
}
static void ac_evict(const char *p) {
    if (!p || !p[0]) return; CLK; struct acent *e = &g_ac[mc_hash(p) & 2047];
    if (e->hash == mc_hash(p) && !strcmp(e->path, p)) e->hash = 0;
    CUL;
}
static void fd_setpath(int fd, const char *p) { if (fd >= 0 && fd < 1024 && p && strlen(p) < 192) strcpy(g_fdpath[fd], p); }
static void fd_evict(int fd) { if (fd >= 0 && fd < 1024 && g_fdpath[fd][0]) mc_evict(g_fdpath[fd]); }
static void fd_clear(int fd) { if (fd >= 0 && fd < 1024) g_fdpath[fd][0] = 0; }

// macOS errno -> Linux errno. They agree on 1..10 and 12..34 but diverge at 11 (EDEADLK<->EAGAIN)
// and everything >=35 (macOS EAGAIN=35 vs Linux 11, ENOSYS=78 vs 38, ELOOP=62 vs 40, ...). Every
// syscall that returns a host errno is translated at the boundary (QEMU-style). Identity outside.
static int m2l_errno(int m) {
    static const short T[107] = {
        0,1,2,3,4,5,6,7,8,9,              10,35,12,13,14,15,16,17,18,19,
        20,21,22,23,24,25,26,27,28,29,    30,31,32,33,34,11,115,114,88,89,
        90,91,92,93,94,95,96,97,98,99,    100,101,102,103,104,105,106,107,108,109,
        110,111,40,36,112,113,39,22,87,122, 116,66,22,22,22,22,22,37,38,22,
        22,22,22,75,22,22,22,22,125,43,   42,84,61,74,72,61,67,63,60,71,
        62,95,22,131,130,122 };
    return (m >= 0 && m < 107) ? T[m] : m;
}

