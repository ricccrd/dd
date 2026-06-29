// poll(2): read end not ready -> timeout; after a write -> POLLIN; write end is POLLOUT-ready.
#include <poll.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    pipe(fds);
    struct pollfd pr = {.fd = fds[0], .events = POLLIN};
    int timed_out = poll(&pr, 1, 50) == 0; // nothing written yet
    struct pollfd pw = {.fd = fds[1], .events = POLLOUT};
    int writable = poll(&pw, 1, 50) == 1 && (pw.revents & POLLOUT);
    write(fds[1], "x", 1);
    int readable = poll(&pr, 1, 50) == 1 && (pr.revents & POLLIN);
    close(fds[0]); close(fds[1]);
    printf("pollpipe timeout=%d writable=%d readable=%d\n", timed_out, writable, readable);
    return 0;
}
