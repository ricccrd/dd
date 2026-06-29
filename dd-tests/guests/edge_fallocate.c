// EDGE: fallocate(FALLOC_FL_PUNCH_HOLE) must zero a range AND deallocate its blocks (the file stays
// the same size, but the punched region reads as zeros and st_blocks drops). A runtime that ignores
// the mode (e.g. just ftruncate-extends) leaves the old data. We check the data goes to zero across
// the hole while the bytes outside stay intact. Diffed vs native -> oracle.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SZ (128 * 1024)

int main(void) {
    const char *p = "/tmp/dd_falloc";
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
    char buf[SZ];
    memset(buf, 0x5a, SZ);
    write(fd, buf, SZ);
    // punch a hole in the middle [32K, 96K)
    int rc = fallocate(fd, 0x02 /*PUNCH_HOLE*/ | 0x01 /*KEEP_SIZE*/, 32 * 1024, 64 * 1024);
    char rb[SZ];
    lseek(fd, 0, SEEK_SET);
    read(fd, rb, SZ);
    long hole_nonzero = 0, edge_ok = (rb[0] == 0x5a) && (rb[SZ - 1] == 0x5a);
    for (int i = 32 * 1024; i < 96 * 1024; i++) if (rb[i] != 0) hole_nonzero++;
    close(fd);
    unlink(p);
    printf("fallocate rc=%d hole_nonzero=%ld edges_intact=%d\n", rc, hole_nonzero, edge_ok); // 0 0 1
    return 0;
}
