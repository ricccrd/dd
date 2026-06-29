// pipe2(2) Linux: O_NONBLOCK + O_CLOEXEC applied atomically at creation.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    int rc = pipe2(fds, O_NONBLOCK | O_CLOEXEC);
    int nb = (fcntl(fds[0], F_GETFL) & O_NONBLOCK) != 0;
    int ce = (fcntl(fds[0], F_GETFD) & FD_CLOEXEC) != 0;
    // empty + nonblocking read -> EAGAIN
    char c;
    int eagain = read(fds[0], &c, 1) < 0 && errno == EAGAIN;
    close(fds[0]); close(fds[1]);
    printf("pipe2 rc=%d nonblock=%d cloexec=%d eagain=%d\n", rc, nb, ce, eagain);
    return 0;
}
