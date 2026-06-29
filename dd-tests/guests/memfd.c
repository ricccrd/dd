// memfd_create: an anonymous in-memory file, sized with ftruncate, written via the fd, then read
// back through a MAP_SHARED mapping. Linux-specific. Diffed against a native Linux oracle.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    int fd = memfd_create("dd_memfd", 0);
    if (fd < 0) { perror("memfd_create"); return 1; }
    if (ftruncate(fd, 4096) < 0) { perror("ftruncate"); return 1; }
    const char *msg = "in-memory-file";
    write(fd, msg, strlen(msg));

    char *m = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    int ok = m != MAP_FAILED && memcmp(m, msg, strlen(msg)) == 0;
    if (m != MAP_FAILED) munmap(m, 4096);
    close(fd);
    printf("memfd ok=%d\n", ok); // 1
    return 0;
}
