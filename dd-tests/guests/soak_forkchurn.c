// SOAK: process create/reap endurance. fork+waitpid 3000 children in sequence, each exits with a small
// code that the parent folds into a checksum. The bug class: pid/fd/host-process leakage or
// translation-state not reset cleanly across fork over a long run (a runtime fine for a handful of
// forks runs out of pids/fds/memory after thousands). Deterministic checksum -> golden, every engine.
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    long acc = 0, reaped = 0;
    for (int i = 0; i < 3000; i++) {
        pid_t p = fork();
        if (p == 0) _exit((i % 100) + 1);
        if (p < 0) { printf("soak forkchurn fork_fail@%d\n", i); return 1; }
        int st = 0;
        if (waitpid(p, &st, 0) == p && WIFEXITED(st)) { acc += WEXITSTATUS(st); reaped++; }
    }
    printf("soak forkchurn reaped=%ld acc=%ld\n", reaped, acc);
    return 0;
}
