// poll() on a pipe read end: blocks until the forked writer sends a byte (POLLIN), then reports
// POLLHUP once the writer closes. Verifies readiness + hangup edges on a pipe. Portable, golden.
#include <unistd.h>
#include <poll.h>
#include <stdio.h>
#include <time.h>
#include <sys/wait.h>
int main(void) {
    int p[2]; pipe(p);
    pid_t pid = fork();
    if (pid == 0) { close(p[0]); struct timespec ts = {0, 30000000}; nanosleep(&ts, 0); write(p[1], "X", 1); close(p[1]); _exit(0); }
    close(p[1]);
    struct pollfd pf = {.fd = p[0], .events = POLLIN};
    int r = poll(&pf, 1, 2000);
    int has = (r == 1) && (pf.revents & POLLIN);
    char c = 0; read(p[0], &c, 1);
    waitpid(pid, 0, 0);
    struct pollfd pf2 = {.fd = p[0], .events = POLLIN};
    int r2 = poll(&pf2, 1, 2000);
    int hup = (r2 >= 1) && (pf2.revents & (POLLHUP | POLLIN));
    close(p[0]);
    printf("pipe_poll readable=%d got=%c hup=%d\n", has, c, hup);
    return 0;
}
