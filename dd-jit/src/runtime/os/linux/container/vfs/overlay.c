// Extracted from ../vfs.c: OCI overlay image layers (lower/upper, copy-up, whiteout, merged readdir) + abs_guest
// Not standalone -- #included by ../vfs.c at the original position (verbatim move, identical
// preprocessed TU). Relies on ../vfs.c's preceding globals/headers; see vfs.c for context.
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
// Resolve an executable/interpreter path through the FULL overlay (upper THEN lowers), returning the host
// path in `buf`. Drop-in for xresolve_exec at the ELF-loader sites so a program that lives only in a
// read-only --lower (empty upper) is found -- a bare xresolve_exec checks the upper alone and ENOENTs.
// With no lowers (the flat-rootfs pull case) this is identical to xresolve_exec.
static const char *xresolve_overlay(const char *p, char *buf, size_t n) {
    if (!(g_rootfs && p && p[0] == '/')) return p;
    overlay_resolve(p, buf, n, 0);
    return buf;
}
// Overlay: ensure every PARENT directory of `guest` exists in the writable upper, copying up (mkdir, with
// the lower's mode) each ancestor that currently lives only in a read-only lower layer. A create syscall
// (openat O_CREAT via overlay_copyup, or mkdirat/symlinkat/mknodat/renameat via jail_at) confines to the
// upper, so without this it fails with ENOENT whenever the target's parent dir is still only in the image.
// The FINAL component is never created (that is the syscall's job), and an ancestor present in NO layer is
// left missing so a genuine bad path still fails ENOENT as the kernel would. Overlay mode only (no-op when
// g_nlower==0 -> non-overlay behavior is byte-identical); rootfs-routed paths only (a volume has its own
// real backing dir and must not be mirrored into the upper).
static void overlay_mkparents(const char *guest) {
    if (!g_nlower || !guest || guest[0] != '/') return;
    const char *canon;
    size_t clen;
    const char *rel;
    if (jail_pick(guest, &canon, &clen, &rel) != g_root_fd) return;
    char par[4200];
    snprintf(par, sizeof par, "%s", guest);
    char *sl = strrchr(par, '/');
    // parent is the root dir -> always present
    if (!sl || sl == par) return;
    *sl = 0;
    // Build each ancestor prefix ("/a", "/a/b", ...) and copy it up if missing in the upper. Each level is
    // created before the next, so confine_in always resolves the (now-present) parent.
    char acc[4200];
    size_t al = 0;
    acc[0] = 0;
    for (char *seg = par + 1;;) {
        char *next = strchr(seg, '/');
        if (next) *next = 0;
        int w = snprintf(acc + al, sizeof acc - al, "/%s", seg);
        if (w < 0 || (size_t)w >= sizeof acc - al) return;
        al += (size_t)w;
        char up[4300];
        confine_in(g_rootfs_canon, g_rootfs_canon_len, acc, up, sizeof up, 1);
        struct stat st;
        if (lstat(up, &st) != 0)
            // missing in the upper -> copy it up from the first lower that has it as a directory
            for (int i = 0; i < g_nlower; i++) {
                char lo[4300];
                layer_follow(g_lower[i].canon, g_lower[i].clen, acc, lo, sizeof lo, 0);
                if (lstat(lo, &st) == 0 && S_ISDIR(st.st_mode)) {
                    mkdir(up, st.st_mode & 0777);
                    break;
                }
            }
        if (!next) break;
        *next = '/';
        seg = next + 1;
    }
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
    // brand-new file: nothing to copy, but its parent dir may be lower-only -> materialize it in the upper
    // so the caller's open(O_CREAT) lands there (otherwise the create ENOENTs on a missing upper parent).
    if (!have) {
        overlay_mkparents(guest);
        return;
    }
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
