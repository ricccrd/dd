// frontend/aarch64/fill_stat.c -- the aarch64 Linux `struct stat` layout (per-arch: x86_64 differs).
// Provided by the frontend so os/linux/ (vfs synth + the stat syscalls) stays layout-agnostic.

static void fill_linux_stat(uint8_t *d, const struct stat *s) {
    memset(d, 0, 128);
    *(uint64_t *)(d + 0) = s->st_dev;
    *(uint64_t *)(d + 8) = s->st_ino;
    *(uint32_t *)(d + 16) = s->st_mode;
    *(uint32_t *)(d + 20) = s->st_nlink;
    *(uint32_t *)(d + 24) = s->st_uid;
    *(uint32_t *)(d + 28) = s->st_gid;
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
