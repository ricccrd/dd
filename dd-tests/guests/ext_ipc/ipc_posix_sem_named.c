// POSIX named semaphore across a fork: child posts 5 times (incrementing a shared anon-mmap counter
// each post), parent waits 5 times. Verifies a named sem synchronizes two processes. Portable, golden.
#include <semaphore.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdatomic.h>
int main(void) {
    sem_unlink("/dd_named_sem");
    sem_t *s = sem_open("/dd_named_sem", O_CREAT, 0600, 0);
    if (s == SEM_FAILED) { perror("sem_open"); return 1; }
    atomic_long *c = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    atomic_store(c, 0);
    pid_t pid = fork();
    if (pid == 0) { for (int i = 0; i < 5; i++) { atomic_fetch_add(c, 1); sem_post(s); } _exit(0); }
    for (int i = 0; i < 5; i++) sem_wait(s);
    long v = atomic_load(c);
    waitpid(pid, 0, 0); sem_close(s); sem_unlink("/dd_named_sem");
    printf("posix_sem_named c=%ld\n", v); // 5
    return 0;
}
