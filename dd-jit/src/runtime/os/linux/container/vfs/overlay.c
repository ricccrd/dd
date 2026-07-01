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
// Map a canonical HOST directory path back to its GUEST path. The guest cwd is a guest-visible path, but
// chdir/fchdir only have the host path the dir actually resolved to -- which, under the overlay, may sit in
// the writable upper (g_rootfs_canon), in any read-only lower (the image), or in a bind-mount volume. Strip
// whichever jail prefix backs it so the guest cwd is tracked correctly regardless of layer (the bare
// "strip g_rootfs_canon" form silently left g_cwd stale for a dir that lives only in a lower). Boundary
// check ('/' or end) avoids a prefix collision between sibling layer roots. Unknown -> "/" (fail safe).
static void guest_from_host_raw(const char *host, char *out, size_t n) {
    if (g_rootfs && !strncmp(host, g_rootfs_canon, g_rootfs_canon_len) &&
        (host[g_rootfs_canon_len] == '/' || host[g_rootfs_canon_len] == 0)) {
        const char *g = host + g_rootfs_canon_len;
        snprintf(out, n, "%s", g[0] ? g : "/");
        return;
    }
    for (int i = 0; i < g_nlower; i++)
        if (!strncmp(host, g_lower[i].canon, g_lower[i].clen) &&
            (host[g_lower[i].clen] == '/' || host[g_lower[i].clen] == 0)) {
            const char *g = host + g_lower[i].clen;
            snprintf(out, n, "%s", g[0] ? g : "/");
            return;
        }
    for (int i = 0; i < g_nvols; i++)
        if (!strncmp(host, g_vols[i].hcanon, g_vols[i].hlen) &&
            (host[g_vols[i].hlen] == '/' || host[g_vols[i].hlen] == 0)) {
            snprintf(out, n, "%s%s", g_vols[i].guest, host + g_vols[i].hlen);
            return;
        }
    snprintf(out, n, "/");
}
// Map a canonical HOST dir to the GUEST path, then fold it into the active chroot frame: under a chroot,
// chdir/fchdir resolve to a host dir whose rootfs-relative guest path includes the chroot prefix, but the
// guest must see g_cwd in its OWN root, so strip the prefix. No-op (byte-identical) with no chroot.
static void guest_from_host(const char *host, char *out, size_t n) {
    guest_from_host_raw(host, out, n);
    if (g_chroot[0]) chroot_strip(out, n);
}
// Append name/type to a growable (realloc-doubling) parallel array pair. Returns -1 on OOM (leaving the
// arrays valid, just not grown) so the caller emits what it already has rather than overrunning.
static int ovl_push(char (**names)[256], uint8_t **types, int *cap, int n, const char *nm, uint8_t ty) {
    if (n == *cap) {
        int nc = *cap ? *cap * 2 : 64;
        char (*n2)[256] = realloc(*names, (size_t)nc * 256);
        uint8_t *t2 = realloc(*types, (size_t)nc);
        if (n2) *names = n2;
        if (t2) *types = t2;
        if (!n2 || !t2) return -1;
        *cap = nc;
    }
    snprintf((*names)[n], 256, "%s", nm);
    (*types)[n] = ty;
    return 0;
}
// Append to the growable dedup list. Returns -1 on OOM.
static int ovl_seen(char (**seen)[256], int *scap, int ns, const char *nm) {
    if (ns == *scap) {
        int nc = *scap ? *scap * 2 : 64;
        char (*s2)[256] = realloc(*seen, (size_t)nc * 256);
        if (!s2) return -1;
        *seen = s2;
        *scap = nc;
    }
    snprintf((*seen)[ns], 256, "%s", nm);
    return 0;
}
// Overlay whiteout for a delete: remove the upper copy (if any) and drop a .wh.NAME marker in the upper.
// Merged readdir across layers (upper first, then lowers). Higher layer wins; a .wh.NAME hides NAME
// in all lower layers; .wh.* markers are not emitted. Allocates the merged name/type arrays sized to the
// actual directory (no fixed cap -- a dir with >1024 merged entries enumerates fully; #179) and hands
// them back via *names_out/*types_out for the caller to free(); returns the entry count (0 leaves the
// out-pointers NULL). The internal `seen` dedup list grows with the directory too.
static int overlay_readdir(const char *gdir, char (**names_out)[256], uint8_t **types_out) {
    char(*names)[256] = NULL, (*seen)[256] = NULL;
    uint8_t *types = NULL;
    int cap = 0, nout = 0, scap = 0, ns = 0;
    // L=-1 is the upper (rootfs)
    for (int L = -1; L < g_nlower; L++) {
        const char *jc = L < 0 ? g_rootfs_canon : g_lower[L].canon;
        size_t jcl = L < 0 ? g_rootfs_canon_len : g_lower[L].clen;
        char host[4300];
        layer_follow(jc, jcl, gdir, host, sizeof host, 0);
        DIR *d = opendir(host);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
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
            // higher layer already decided this name -- record it (whiteouts included, so they keep
            // hiding the lower-layer copy) in the growable dedup list
            if (ovl_seen(&seen, &scap, ns, name) < 0) break;
            ns++;
            if (!wh) {
                if (ovl_push(&names, &types, &cap, nout, name, e->d_type) < 0) break;
                nout++;
            // whiteout -> hide, don't emit
            }
        }
        closedir(d);
    }
    // Bind-mount mount points: a volume is its own jail (in no layer), so a NESTED mount's parent dirs are
    // invisible to the layer scan above -- and the empty placeholder we create in the writable upper can be
    // served STALE by the host FS cache when the rootfs dir is held open for the container's lifetime.
    // Synthesize each volume's immediate child under `gdir` straight from the volume table so a parent
    // listing always shows the mount entry, exactly as Docker (which mkdir -p's every mount target). A bind
    // target is always a directory from the guest's view; deduped against the real layer entries. (A volume
    // dir fd lists via plain readdir, never here -- see the jail_is_vol() guard at the openat site -- so a
    // mount is never asked to enumerate itself.)
    size_t glen = strlen(gdir);
    int at_root = glen == 1 && gdir[0] == '/';
    for (int i = 0; i < g_nvols; i++) {
        const char *rest;
        if (at_root)
            rest = g_vols[i].guest + 1; // "/data" -> "data", "/x/y" -> "x/y"
        else if (!strncmp(g_vols[i].guest, gdir, glen) && g_vols[i].guest[glen] == '/')
            rest = g_vols[i].guest + glen + 1;
        else
            continue; // not under gdir
        if (!*rest) continue;
        char child[256];
        size_t k = 0;
        while (rest[k] && rest[k] != '/' && k < sizeof child - 1) {
            child[k] = rest[k];
            k++;
        }
        child[k] = 0;
        int dup = 0;
        for (int j = 0; j < ns; j++)
            if (!strcmp(seen[j], child)) {
                dup = 1;
                break;
            }
        if (dup) continue;
        if (ovl_seen(&seen, &scap, ns, child) < 0) break;
        ns++;
        if (ovl_push(&names, &types, &cap, nout, child, DT_DIR) < 0) break;
        nout++;
    }
    free(seen);
    *names_out = names;
    *types_out = types;
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
