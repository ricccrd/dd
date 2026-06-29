// Filesystem stat: statvfs("/tmp") and fstatvfs on an open fd. Values are host-specific, so we
// print only a sanity verdict (block size a power of two, non-zero totals, consistent fd vs path).
// Exercises statfs/fstatfs. Portable -> all engines, golden-checked.
#include <fcntl.h>
#include <stdio.h>
#include <sys/statvfs.h>
#include <unistd.h>

int main(void) {
    struct statvfs a, b;
    int r1 = statvfs("/tmp", &a);
    int fd = open("/tmp", O_RDONLY);
    int r2 = fstatvfs(fd, &b);
    close(fd);
    int bsize_pow2 = a.f_bsize && (a.f_bsize & (a.f_bsize - 1)) == 0;
    int blocks_ok = a.f_blocks > 0 && a.f_bfree <= a.f_blocks;
    int consistent = a.f_bsize == b.f_bsize && a.f_blocks == b.f_blocks;
    printf("statvfs ok=%d bsize_pow2=%d blocks_ok=%d consistent=%d\n",
           (r1 | r2) == 0, bsize_pow2, blocks_ok, consistent); // 1 1 1 1
    return 0;
}
