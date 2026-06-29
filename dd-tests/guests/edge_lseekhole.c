// EDGE: sparse-file seeking. Write a byte at offset 0 and at offset 1 MiB (leaving a hole), then use
// SEEK_DATA / SEEK_HOLE to discover the layout: SEEK_HOLE from 0 finds the hole start, SEEK_DATA from
// the middle finds the next data. macOS/HFS+APFS report different granularity or ENXIO. Diffed vs
// native -> oracle.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    const char *p = "/tmp/dd_sparse";
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
    pwrite(fd, "A", 1, 0);
    pwrite(fd, "B", 1, 1 << 20); // 1 MiB -> creates a hole between
    off_t end = lseek(fd, 0, SEEK_END);
    off_t hole = lseek(fd, 0, SEEK_HOLE);   // first hole at/after 0
    off_t data = lseek(fd, 4096, SEEK_DATA); // next data at/after 4K -> ~1 MiB
    int hole_ok = (hole > 0 && hole < (1 << 20)); // a hole exists before the 1 MiB mark
    int data_ok = (data >= (1 << 20) - 4096 && data <= (1 << 20)); // data resumes near 1 MiB
    close(fd);
    unlink(p);
    printf("lseekhole end=%ld hole_found=%d data_found=%d\n", (long)end, hole_ok, data_ok); // 1048577 1 1
    return 0;
}
