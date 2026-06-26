// dd/runtime/os/linux/container -- the container VFS: TOCTOU-free path jail, overlay image layers
// (lower/upper + copy-up + whiteout + merged readdir), and /proc + /sys synthesis.

// ---- rootfs path rewriting (ported from mac_elf.c) ----
static const char *g_rootfs = NULL;
// guest CWD (within the rootfs) -- AT_FDCWD resolution + getcwd
static char g_cwd[4200] = "/";
static uint8_t g_auxv_data[1024];
// serialized auxv for /proc/self/auxv
static int g_auxv_len;
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
// realpath(g_rootfs) -- the true rootfs boundary
static char g_rootfs_canon[4200];
static size_t g_rootfs_canon_len;
// fd -> host path it was opened with (dir-fd confinement + cache)
static char g_fdpath[1024][192];
// overlay: dir-fd -> its GUEST path (for merged getdents); "" = not an overlay dir
static char g_ovldir[1024][192];
// eventfd(read-end) -> pipe write-end + 1 (0 = not an eventfd)
static int g_eventfd_peer[1024];
// fd is a timerfd (a kqueue with an EVFILT_TIMER) -> read() drains it
static uint8_t g_timerfd[1024];
// fd is an inotify (a kqueue with EVFILT_VNODE watches) -> read() drains it
static uint8_t g_inotify[1024];
// pinned O_DIRECTORY fd to the rootfs (set at startup)
static int g_root_fd = -1;
// Bind-mount volumes: a guest path prefix -> a host directory, each its own confined jail root.
struct vol {
    char guest[256];
    size_t glen;
    char hcanon[1024];
    size_t hlen;
    int fd;
};
static struct vol g_vols[32];
static int g_nvols;
// Pick the jail (rootfs or a volume) for an absolute guest path; *rel = the path within that jail.
static int jail_pick(const char *abs, const char **canon, size_t *clen, const char **rel) {
    for (int i = 0; i < g_nvols; i++)
        if (!strncmp(abs, g_vols[i].guest, g_vols[i].glen) &&
            (abs[g_vols[i].glen] == '/' || abs[g_vols[i].glen] == 0)) {
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
    const char *jcanon;
    size_t jclen;
    const char *rel;
    // rootfs or a volume root (jcanon is absolute)
    jail_pick(guest, &jcanon, &jclen, &rel);
    return confine_in(jcanon, jclen, rel, out, n, nofollow);
}
// ---- Overlay (OCI image layers): --rootfs is the writable UPPER; --lower dirs are read-only,
// searched top->down when a path isn't in the upper. Whiteout (.wh.NAME) hides a lower entry;
// copy-up brings a lower file into the upper on write. Off entirely when g_nlower==0.
static const char *xresolve_exec(const char *p, char *buf,
                                 // fwd (defined below; overlay uses it for the upper)
                                 size_t n);
struct olayer {
    char canon[1024];
    size_t clen;
};
static struct olayer g_lower[8];
// [0] = highest-priority lower (searched first)
static int g_nlower = 0;
// register a read-only lower layer (image layer)
static void add_lower(const char *dir) {
    if (g_nlower >= 8 || !dir || !dir[0]) return;
    if (!realpath(dir, g_lower[g_nlower].canon))
        snprintf(g_lower[g_nlower].canon, sizeof g_lower[g_nlower].canon, "%s", dir);
    g_lower[g_nlower].clen = strlen(g_lower[g_nlower].canon);
    g_nlower++;
}
static void wh_hostpath(const char *jcanon, size_t jclen, const char *guest, char *out,
                        // host path of the .wh.NAME marker
                        size_t n) {
    char par[4200];
    snprintf(par, sizeof par, "%s", guest);
    char *sl = strrchr(par, '/');
    char base[256];
    snprintf(base, sizeof base, "%s", sl ? sl + 1 : par);
    if (sl) *sl = 0;
    char gw[4500];
    snprintf(gw, sizeof gw, "%s/.wh.%s", par[0] ? par : "", base);
    confine_in(jcanon, jclen, gw, out, n, 1);
}
static int wh_exists(const char *jcanon, size_t jclen,
                     // is there a whiteout for `guest` in this layer?
                     const char *guest) {
    char host[4300];
    wh_hostpath(jcanon, jclen, guest, host, sizeof host);
    struct stat st;
    return lstat(host, &st) == 0;
}
// One layer's host path for `guest`, following symlinks LAYER-relative (absolute target = layer root,
// like xresolve_exec does for the rootfs). nofollow keeps the final component unresolved. Returns the
// confined host path; the caller lstats it to test existence in this layer.
static void layer_follow(const char *jc, size_t jcl, const char *guest, char *out, size_t n, int nofollow) {
    char cur[4200];
    snprintf(cur, sizeof cur, "%s", guest);
    for (int hop = 0; hop < 40; hop++) {
        char hb[4300];
        // host path, final NOT followed
        confine_in(jc, jcl, cur, hb, sizeof hb, 1);
        struct stat st;
        if (lstat(hb, &st) != 0) {
            confine_in(jc, jcl, cur, out, n, nofollow);
            return;
        // missing -> report (confined)
        }
        if (!S_ISLNK(st.st_mode)) {
            snprintf(out, n, "%s", hb);
            return;
        // real file/dir -> done
        }
        if (nofollow && !strcmp(cur, guest)) {
            snprintf(out, n, "%s", hb);
            return;
        // lstat/readlink: keep the final link
        }
        char tgt[4200];
        ssize_t k = readlink(hb, tgt, sizeof tgt - 1);
        if (k <= 0) {
            snprintf(out, n, "%s", hb);
            return;
        }
        tgt[k] = 0;
        if (tgt[0] == '/')
            // absolute -> layer-relative
            snprintf(cur, sizeof cur, "%s", tgt);
        else {
            char d[4200];
            snprintf(d, sizeof d, "%s", cur);
            char *sl = strrchr(d, '/');
            if (sl) *sl = 0;
            char j[8400];
            snprintf(j, sizeof j, "%s/%s", d, tgt);
            snprintf(cur, sizeof cur, "%s", j);
        }
    }
    confine_in(jc, jcl, cur, out, n, nofollow);
}
// Overlay READ resolve (follow symlinks): topmost layer that has `guest`. 1 + host on hit; 0 if absent
// or whiteout-hidden. The upper (rootfs) is searched first via xresolve_exec (handles its symlinks).
static int overlay_resolve(const char *guest, char *host, size_t hn, int nofollow) {
    char up[4300];
    if (nofollow)
        secure_resolve(guest, up, sizeof up, 1);
    else
        xresolve_exec(guest, up, sizeof up);
    struct stat st;
    if (lstat(up, &st) == 0) {
        snprintf(host, hn, "%s", up);
        return 1;
    // upper shadows lowers
    }
    if (wh_exists(g_rootfs_canon, g_rootfs_canon_len, guest)) {
        snprintf(host, hn, "%s", up);
        return 0;
    // deleted
    }
    // search lowers top->down
    for (int i = 0; i < g_nlower; i++) {
        char lp[4300];
        layer_follow(g_lower[i].canon, g_lower[i].clen, guest, lp, sizeof lp, nofollow);
        if (lstat(lp, &st) == 0) {
            snprintf(host, hn, "%s", lp);
            return 1;
        }
        if (wh_exists(g_lower[i].canon, g_lower[i].clen, guest)) {
            snprintf(host, hn, "%s", up);
            return 0;
        }
    }
    snprintf(host, hn, "%s", up);
    // absent -> upper path (for ENOENT/O_CREAT)
    return 0;
}
// Copy-up: bring a lower file into the UPPER so it can be modified, then return the upper host path.
// If the file is only in a lower, copy its bytes up; if absent everywhere, return the upper path (create).
static void overlay_copyup(const char *guest, char *host, size_t hn) {
    char up[4300];
    xresolve_exec(guest, up, sizeof up);
    snprintf(host, hn, "%s", up);
    struct stat st;
    // already in the upper (writable)
    if (lstat(up, &st) == 0) return;
    // was deleted -> drop the whiteout, create fresh
    if (wh_exists(g_rootfs_canon, g_rootfs_canon_len, guest)) {
        char wh[4300];
        wh_hostpath(g_rootfs_canon, g_rootfs_canon_len, guest, wh, sizeof wh);
        unlink(wh);
        return;
    }
    char src[4300];
    int have = 0;
    for (int i = 0; i < g_nlower && !have; i++) {
        layer_follow(g_lower[i].canon, g_lower[i].clen, guest, src, sizeof src, 0);
        if (lstat(src, &st) == 0 && S_ISREG(st.st_mode))
            have = 1;
        else if (wh_exists(g_lower[i].canon, g_lower[i].clen, guest))
            break;
    }
    // new file -> upper path as-is
    if (!have) return;
    char dir[4300];
    snprintf(dir, sizeof dir, "%s", up);
    // mkdir -p the upper parent
    char *sl = strrchr(dir, '/');
    if (sl) {
        *sl = 0;
        for (char *q = dir + g_rootfs_canon_len + 1; *q; q++)
            if (*q == '/') {
                *q = 0;
                mkdir(dir, 0755);
                *q = '/';
            }
        mkdir(dir, 0755);
    }
    int in = open(src, O_RDONLY),
        // copy lower -> upper
        out = open(up, O_CREAT | O_WRONLY | O_TRUNC, st.st_mode & 0777);
    if (in >= 0 && out >= 0) {
        char b[1 << 16];
        ssize_t r;
        while ((r = read(in, b, sizeof b)) > 0)
            if (write(out, b, r) < 0) break;
    }
    if (in >= 0) close(in);
    if (out >= 0) close(out);
}
// Absolute GUEST path for (dirfd, raw) -- combines a dir-fd's guest path (upper or lower) with raw.
static void abs_guest(int dirfd, const char *raw, char *out, size_t n) {
    if (raw && raw[0] == '/') {
        snprintf(out, n, "%s", raw);
        return;
    }
    if (dirfd >= 0 && dirfd < 1024 && g_fdpath[dirfd][0]) {
        const char *gdir = g_fdpath[dirfd];
        if (strncmp(gdir, g_rootfs_canon, g_rootfs_canon_len) == 0)
            gdir += g_rootfs_canon_len;
        else
            for (int i = 0; i < g_nlower; i++)
                if (strncmp(gdir, g_lower[i].canon, g_lower[i].clen) == 0) {
                    gdir += g_lower[i].clen;
                    break;
                }
        snprintf(out, n, "/%s/%s", gdir, raw ? raw : "");
    } else
        // AT_FDCWD-relative -> guest cwd
        snprintf(out, n, "%s/%s", g_cwd, raw ? raw : "");
}
// Overlay whiteout for a delete: remove the upper copy (if any) and drop a .wh.NAME marker in the upper.
// Merged readdir across layers (upper first, then lowers). Higher layer wins; a .wh.NAME hides NAME
// in all lower layers; .wh.* markers are not emitted. Fills names[]/types[], returns count.
static int overlay_readdir(const char *gdir, char names[][256], uint8_t *types, int max) {
    char seen[1024][256];
    int ns = 0, nout = 0;
    // L=-1 is the upper (rootfs)
    for (int L = -1; L < g_nlower && nout < max; L++) {
        const char *jc = L < 0 ? g_rootfs_canon : g_lower[L].canon;
        size_t jcl = L < 0 ? g_rootfs_canon_len : g_lower[L].clen;
        char host[4300];
        layer_follow(jc, jcl, gdir, host, sizeof host, 0);
        DIR *d = opendir(host);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) && nout < max) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            int wh = !strncmp(e->d_name, ".wh.", 4);
            const char *name = wh ? e->d_name + 4 : e->d_name;
            int dup = 0;
            for (int i = 0; i < ns; i++)
                if (!strcmp(seen[i], name)) {
                    dup = 1;
                    break;
                }
            if (dup) continue;
            // higher layer already decided this name
            if (ns < 1024) snprintf(seen[ns++], 256, "%s", name);
            if (!wh) {
                snprintf(names[nout], 256, "%s", name);
                types[nout] = e->d_type;
                nout++;
            // whiteout -> hide, don't emit
            }
        }
        closedir(d);
    }
    return nout;
}
static void overlay_whiteout(const char *guest) {
    char up[4300];
    xresolve_exec(guest, up, sizeof up);
    // drop any upper copy (file or empty dir)
    remove(up);
    char wh[4300];
    wh_hostpath(g_rootfs_canon, g_rootfs_canon_len, guest, wh, sizeof wh);
    char dir[4300];
    snprintf(dir, sizeof dir, "%s", wh);
    // mkdir -p parent
    char *s2 = strrchr(dir, '/');
    if (s2) {
        *s2 = 0;
        for (char *q = dir + g_rootfs_canon_len + 1; *q; q++)
            if (*q == '/') {
                *q = 0;
                mkdir(dir, 0755);
                *q = '/';
            }
        mkdir(dir, 0755);
    }
    int fd = open(wh, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
}
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
// TOCTOU-FREE confinement. Resolve `guest` (absolute) one component at a time on PINNED dir-fds,
// never following a symlink out of the jail. Returns a fresh dir-fd to the confined parent (caller
// closes) + the final component in `final`. -1 on escape/error. No check/use gap: each step
// operates on a held fd, symlinks are read+respliced (clamped to root), and the caller's
// openat(pfd, final, O_NOFOLLOW) is atomic -- a concurrent symlink swap cannot redirect it out.
// Fully stack-local (fds[] + buffers) -> thread-safe; g_root_fd is read-only after startup.
static int resolve_at(const char *guest, char *final, size_t fn, int nofollow) {
    if (g_root_fd < 0) return -1;
    const char *rel;
    // rootfs or a volume root
    int root_fd = jail_pick(guest, NULL, NULL, &rel);
    char rest[8192];
    snprintf(rest, sizeof rest, "%s", rel);
    int fds[260], nf = 0, budget = 40, ret = -EACCES;
    fds[nf++] = openat(root_fd, ".", O_RDONLY | O_DIRECTORY);
    if (fds[0] < 0) return -EACCES;
    final[0] = 0;
    for (;;) {
        char *s = rest;
        while (*s == '/')
            s++;
        if (!*s) break;
        char *e = s;
        while (*e && *e != '/')
            e++;
        int last = (*e == 0), L = (int)(e - s);
        if (L >= 255) {
            ret = -ENAMETOOLONG;
            goto out;
        }
        char comp[256];
        memcpy(comp, s, L);
        comp[L] = 0;
        char tail[8192];
        snprintf(tail, sizeof tail, "%s", e);
        if (!strcmp(comp, ".")) {
            snprintf(rest, sizeof rest, "%s", tail);
            continue;
        }
        if (!strcmp(comp, "..")) {
            if (nf > 1) close(fds[--nf]);
            snprintf(rest, sizeof rest, "%s", tail);
            continue;
        // clamp at root
        }
        if (last && nofollow) {
            snprintf(final, fn, "%s", comp);
            break;
        }
        struct stat st;
        if (fstatat(fds[nf - 1], comp, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode)) {
            if (--budget < 0) {
                ret = -ELOOP;
                goto out;
            }
            char lk[4096];
            ssize_t k = readlinkat(fds[nf - 1], comp, lk, sizeof lk - 1);
            if (k < 0) {
                ret = -errno;
                goto out;
            }
            lk[k] = 0;
            if (lk[0] == '/') {
                while (nf > 1)
                    close(fds[--nf]);
                snprintf(rest, sizeof rest, "%s%s", lk, tail);
            // abs -> back to root
            }
            else
                // tail already carries its leading '/' (or is empty)
                snprintf(rest, sizeof rest, "%s%s", lk, tail);
            continue;
        }
        if (last) {
            snprintf(final, fn, "%s", comp);
            break;
        }
        int d = openat(fds[nf - 1], comp, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (d < 0) {
            ret = -errno;
            goto out;
        // ENOENT/ENOTDIR/ELOOP -> natural
        }
        if (nf >= 260) {
            close(d);
            ret = -ELOOP;
            goto out;
        }
        fds[nf++] = d;
        snprintf(rest, sizeof rest, "%s", tail);
    }
    if (!final[0]) snprintf(final, fn, "%s", ".");
    ret = openat(fds[nf - 1], ".", O_RDONLY | O_DIRECTORY);
    if (ret < 0) ret = -errno;
out:
    for (int i = 0; i < nf; i++)
        close(fds[i]);
    return ret;
}
// Confined (parent-fd, final) for a guest *at path: absolute or tracked-dir-fd-relative; else deny.
static int jail_at(int dirfd, const char *raw, char *final, size_t fn, int nofollow) {
    char abs[8192];
    if (raw[0] == '/')
        snprintf(abs, sizeof abs, "%s", raw);
    else if (dirfd == -100)
        // AT_FDCWD -> guest cwd
        snprintf(abs, sizeof abs, "%s/%s", g_cwd, raw);
    else if (dirfd >= 0 && dirfd < 1024 && g_fdpath[dirfd][0]) {
        const char *gdir = g_fdpath[dirfd];
        if (strncmp(gdir, g_rootfs_canon, g_rootfs_canon_len) == 0) gdir += g_rootfs_canon_len;
        snprintf(abs, sizeof abs, "/%s/%s", gdir, raw);
    } else
        // untracked dir-fd: fail closed
        return -EACCES;
    size_t al = strlen(abs);
    while (al > 1 && abs[al - 1] == '/')
        // strip trailing '/' (mkdir foo/, rmdir foo/)
        abs[--al] = 0;
    return resolve_at(abs, final, fn, nofollow);
}
// Real macOS stat -> Linux struct stat (the fake S_IFCHR version corrupted libc buffering).
// fill_linux_stat (the guest struct-stat layout) is per-arch -> frontend/<arch>/fill_stat.c
// Synthesize the common /proc files Linux programs read (macOS has no /proc). Returns an fd
// holding the content, -1 on mkstemp error, or -2 if rp isn't a path we synthesize.
static int proc_open(const char *rp) {
    char buf[8192];
    int n = -1;
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
    } else if (!strcmp(rp, "/proc/self/status") || !strcmp(rp, "/proc/self/stat")) {
        // status (key:value)
        if (rp[11] == 'a' && rp[12] == 't' && rp[13] == 'u')
            n = snprintf(buf, sizeof buf,
                         "Name:\tinit\nState:\tR (running)\nTgid:\t1\nPid:\t1\nPPid:\t0\n"
                         "Uid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\nThreads:\t1\n");
        else
            // stat
            n = snprintf(buf, sizeof buf, "1 (init) R 0 1 1 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0 0\n");
    }
    if (n < 0) return -2;
    char tn[] = "/tmp/.ddprocXXXXXX";
    int fd = mkstemp(tn);
    if (fd >= 0) {
        unlink(tn);
        if (write(fd, buf, (size_t)n) < 0) {}
        lseek(fd, 0, SEEK_SET);
    }
    return fd;
}
// Linux-layout stat for a synthesized /proc or /sys file (so stat()/access() see it -- find, du,
// container runtimes that stat /etc/mtab -> /proc/mounts, JVM that stats cgroup files, etc.).
static void fill_linux_stat(uint8_t *d, const struct stat *s);
// -> macOS struct stat for a synth file
static int synth_stat_raw(const char *gp, struct stat *s) {
    if (!gp || (strncmp(gp, "/proc/", 6) && strncmp(gp, "/sys/fs/cgroup/", 15))) return 0;
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
    fill_linux_stat(out, &s);
    return 1;
}
