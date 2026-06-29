// EPOLLONESHOT: the fd reports once, then is disabled until re-armed with EPOLL_CTL_MOD.
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    pipe(fds);
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLONESHOT, .data.fd = fds[0]};
    epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev);
    struct epoll_event out[4];
    write(fds[1], "ab", 2);
    int first = epoll_wait(ep, out, 4, 100) == 1;
    // still readable (didn't drain) but ONESHOT disabled it -> no report
    int disabled = epoll_wait(ep, out, 4, 100) == 0;
    // re-arm
    ev.events = EPOLLIN | EPOLLONESHOT;
    epoll_ctl(ep, EPOLL_CTL_MOD, fds[0], &ev);
    int rearmed = epoll_wait(ep, out, 4, 100) == 1;
    close(ep); close(fds[0]); close(fds[1]);
    printf("epoll_oneshot first=%d disabled=%d rearmed=%d\n", first, disabled, rearmed);
    return 0;
}
