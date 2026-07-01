// frontend/aarch64/fill_stat.c -- the aarch64 Linux `struct stat` layout (per-arch: x86_64 differs).
// Provided by the frontend so os/linux/ (vfs synth + the stat syscalls) stays layout-agnostic.

// container uid/gid virtualization (cuid/cgid defined in os/linux/container/state.c, included later in
// the unity TU): files the container creates live in the writable upper owned by the engine's REAL host
// uid -- report them as the container's uid/gid so guest ownership checks pass (e.g. postgres initdb
// "data directory has wrong ownership" when running as a non-root DD_UID).
static int cuid(void);
static int cgid(void);
// BUG #181: a guest chown is persisted as a host xattr on the overlay-upper file; prefer it over the
// #156 cuid/cgid default (defined in os/linux/container/state.c, later in this unity TU). hostpath/fd
// identify the just-stat'd backing file (NULL/-1 when synthetic or unavailable -> default applies).
static int chown_xattr_get(const char *hostpath, int fd, int *uid, int *gid);

static void fill_linux_stat(uint8_t *d, const struct stat *s, const char *hostpath, int fd) {
    memset(d, 0, 128);
    *(uint64_t *)(d + 0) = s->st_dev;
    *(uint64_t *)(d + 8) = s->st_ino;
    *(uint32_t *)(d + 16) = s->st_mode;
    *(uint32_t *)(d + 20) = s->st_nlink;
    uint32_t uid = (s->st_uid == (uid_t)getuid()) ? (uint32_t)cuid() : s->st_uid;
    uint32_t gid = (s->st_gid == (gid_t)getgid()) ? (uint32_t)cgid() : s->st_gid;
    int xu, xg;
    if (chown_xattr_get(hostpath, fd, &xu, &xg)) {
        if (xu >= 0) uid = (uint32_t)xu;
        if (xg >= 0) gid = (uint32_t)xg;
    }
    *(uint32_t *)(d + 24) = uid;
    *(uint32_t *)(d + 28) = gid;
    // st_rdev
    *(uint64_t *)(d + 32) = s->st_rdev;
    *(uint64_t *)(d + 48) = s->st_size;
    *(uint32_t *)(d + 56) = 4096;
    *(uint64_t *)(d + 64) = s->st_blocks;
    *(uint64_t *)(d + 72) = (uint64_t)s->st_atimespec.tv_sec;
    // st_atim
    *(uint64_t *)(d + 80) = (uint64_t)s->st_atimespec.tv_nsec;
    *(uint64_t *)(d + 88) = (uint64_t)s->st_mtimespec.tv_sec;
    // st_mtim
    *(uint64_t *)(d + 96) = (uint64_t)s->st_mtimespec.tv_nsec;
    *(uint64_t *)(d + 104) = (uint64_t)s->st_ctimespec.tv_sec;
    // st_ctim
    *(uint64_t *)(d + 112) = (uint64_t)s->st_ctimespec.tv_nsec;
}
