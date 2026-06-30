// frontend/x86_64/fill_stat.c -- the x86-64 Linux `struct stat` byte layout (per-arch; differs from
// aarch64). Provided to the shared os/linux/ (vfs synth + the stat syscalls), which stay layout-agnostic.

static void fill_linux_stat(uint8_t *d, const struct stat *s) {
    memset(d, 0, 144);
    *(uint64_t *)(d + 0) = s->st_dev;
    *(uint64_t *)(d + 8) = s->st_ino;
    *(uint64_t *)(d + 16) = s->st_nlink;
    *(uint32_t *)(d + 24) = s->st_mode;
    *(uint32_t *)(d + 28) = s->st_uid;
    *(uint32_t *)(d + 32) = s->st_gid;
    *(uint64_t *)(d + 40) = s->st_rdev;
    *(uint64_t *)(d + 48) = s->st_size;
    *(uint64_t *)(d + 56) = 4096;
    *(uint64_t *)(d + 64) = s->st_blocks;
    *(uint64_t *)(d + 72) = s->st_atime;  // atime sec
    *(uint64_t *)(d + 88) = s->st_mtime;  // mtime sec
    *(uint64_t *)(d + 104) = s->st_ctime; // ctime sec
}
