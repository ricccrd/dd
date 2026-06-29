// ppoll(2) Linux: poll with a timespec timeout and a sigmask; readiness after a write.
#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    pipe(fds);
    struct pollfd pr = {.fd = fds[0], .events = POLLIN};
    struct timespec to = {0, 50000000};
    int timed_out = ppoll(&pr, 1, &to, NULL) == 0;
    write(fds[1], "x", 1);
    sigset_t empty; sigemptyset(&empty);
    int readable = ppoll(&pr, 1, &to, &empty) == 1 && (pr.revents & POLLIN);
    close(fds[0]); close(fds[1]);
    printf("ppoll timeout=%d readable=%d\n", timed_out, readable);
    return 0;
}
