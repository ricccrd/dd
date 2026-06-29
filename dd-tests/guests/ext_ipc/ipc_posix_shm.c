// POSIX shared memory as a cross-process atomic counter: shm_open + ftruncate + MAP_SHARED, then 4
// children each fetch_add 10000 to the shared atomic. Final == 40000. Portable -> all engines, golden.
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdatomic.h>
#define NP 4
int main(void) {
    const char *name = "/dd_posix_shm2";
    shm_unlink(name);
    int fd = shm_open(name, O_CREAT | O_RDWR, 0600);
    if (fd < 0) { perror("shm_open"); return 1; }
    ftruncate(fd, 4096);
    atomic_long *ctr = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    atomic_store(ctr, 0);
    for (int k = 0; k < NP; k++) { if (fork() == 0) { for (int i = 0; i < 10000; i++) atomic_fetch_add(ctr, 1); _exit(0); } }
    for (int k = 0; k < NP; k++) wait(0);
    long total = atomic_load(ctr);
    munmap(ctr, 4096); close(fd); shm_unlink(name);
    printf("posix_shm total=%ld\n", total); // 40000
    return 0;
}
