// epoll readiness loop over a pipe: register the read end, write 5 chunks, drain via
// epoll_wait until all bytes seen. Exercises epoll_create1/epoll_ctl/epoll_wait + EPOLLIN
// edge of readiness. Deterministic -> oracle-checked.
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    if (pipe(fds) < 0) { perror("pipe"); return 1; }
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fds[0]};
    epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev);

    for (int i = 0; i < 5; i++) write(fds[1], "xyz", 3);
    close(fds[1]);

    long total = 0;
    int events_seen = 0;
    for (;;) {
        struct epoll_event out[4];
        int n = epoll_wait(ep, out, 4, 1000);
        if (n <= 0) break;
        events_seen++;
        char buf[64];
        ssize_t r = read(fds[0], buf, sizeof buf);
        if (r <= 0) break;
        total += r;
    }
    close(ep);
    printf("epoll bytes=%ld events_ge1=%d\n", total, events_seen >= 1); // 15, 1
    return 0;
}
