// POSIX shared memory: shm_open + ftruncate + mmap MAP_SHARED, then fork; the child writes a
// pattern into the shared region and the parent reads it back after the child exits. Exercises
// shm_open/shm_unlink + cross-process shared mappings. Portable -> all engines, golden-checked.
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define SZ 4096

int main(void) {
    const char *name = "/dd_shmposix";
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(fd, SZ) < 0) { perror("ftruncate"); return 1; }
    int *p = mmap(0, SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }

    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < 256; i++) p[i] = i * i;
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    long sum = 0;
    for (int i = 0; i < 256; i++) sum += p[i];
    munmap(p, SZ);
    close(fd);
    shm_unlink(name);
    printf("shmposix sum=%ld\n", sum); // sum of i*i, i=0..255 = 5559680
    return 0;
}
