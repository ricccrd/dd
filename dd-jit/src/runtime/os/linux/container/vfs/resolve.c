// Extracted from ../vfs.c: TOCTOU-free path-jail walk (resolve_at + jail_at)
// Not standalone -- #included by ../vfs.c at the original position (verbatim move, identical
// preprocessed TU). Relies on ../vfs.c's preceding globals/headers; see vfs.c for context.
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
