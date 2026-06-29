// Classic producer/consumer over a pipe across a fork: the child writes 1000 integers,
// the parent reads and sums them. Exercises pipe(2) + read/write blocking across the
// fork boundary with backpressure. Deterministic -> oracle-checked.
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    if (pipe(fds) < 0) { perror("pipe"); return 1; }
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        for (int i = 1; i <= 1000; i++)
            write(fds[1], &i, sizeof i);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    long sum = 0;
    int v;
    ssize_t n;
    while ((n = read(fds[0], &v, sizeof v)) == sizeof v) sum += v;
    waitpid(pid, NULL, 0);
    printf("pipeproc sum=%ld\n", sum); // 500500
    return 0;
}
