// mmap a file PROT_READ|PROT_WRITE MAP_SHARED, modify through the mapping, msync, verify on disk.
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_mmapf_%d", (int)getpid());
    long ps = sysconf(_SC_PAGESIZE);
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    char *zero = calloc(1, ps);
    write(fd, zero, ps);
    free(zero);
    char *m = mmap(NULL, ps, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    int mapped = m != MAP_FAILED;
    memcpy(m, "MAPPED-DATA", 11);
    msync(m, ps, MS_SYNC);
    munmap(m, ps);
    // read it back through the fd
    char buf[11] = {0};
    lseek(fd, 0, SEEK_SET);
    read(fd, buf, 11);
    int persisted = memcmp(buf, "MAPPED-DATA", 11) == 0;
    close(fd);
    unlink(path);
    printf("mmapfile mapped=%d persisted=%d\n", mapped, persisted);
    return 0;
}
