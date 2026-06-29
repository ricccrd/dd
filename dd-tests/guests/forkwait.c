// Process-tree fan-out: fork 8 children, each _exit()s with a distinct code; the parent
// reaps them all with waitpid and sums the WEXITSTATUS values. Exercises fork, wait4,
// and exit-status propagation across the JIT's process boundary. Deterministic -> oracle.
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#define N 8

int main(void) {
    pid_t pids[N];
    for (int i = 0; i < N; i++) {
        pid_t p = fork();
        if (p == 0) _exit(i + 1); // codes 1..8
        pids[i] = p;
    }
    int sum = 0, reaped = 0;
    for (int i = 0; i < N; i++) {
        int st = 0;
        if (waitpid(pids[i], &st, 0) == pids[i] && WIFEXITED(st)) {
            sum += WEXITSTATUS(st);
            reaped++;
        }
    }
    printf("forkwait reaped=%d sum=%d\n", reaped, sum); // 8, 36
    return 0;
}
