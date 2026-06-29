// File-backed MAP_SHARED mmap: write a pattern through the mapping, msync, then read it
// back through the file descriptor to prove the store hit the file. Exercises mmap(file),
// msync, and the page<->file coherence path. Deterministic -> oracle-checked.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SZ 8192

int main(void) {
    const char *path = "/tmp/dd_mmapshared.bin";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    if (ftruncate(fd, SZ) < 0) { perror("ftruncate"); return 1; }
    char *m = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    for (int i = 0; i < SZ; i++) m[i] = (char)(i & 0x7f);
    msync(m, SZ, MS_SYNC);
    munmap(m, SZ);

    char buf[SZ];
    lseek(fd, 0, SEEK_SET);
    ssize_t n = read(fd, buf, SZ);
    long sum = 0;
    int ok = (n == SZ);
    for (int i = 0; i < SZ; i++) {
        if (buf[i] != (char)(i & 0x7f)) ok = 0;
        sum += (unsigned char)buf[i];
    }
    close(fd);
    unlink(path);
    printf("mmapshared ok=%d sum=%ld\n", ok, sum);
    return 0;
}
