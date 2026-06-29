// epoll_pwait: poll with an atomically-applied signal mask + timeout.
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    pipe(fds);
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fds[0]};
    epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev);
    struct epoll_event out[4];
    sigset_t mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR1);
    int timed_out = epoll_pwait(ep, out, 4, 50, &mask) == 0;
    write(fds[1], "z", 1);
    int ready = epoll_pwait(ep, out, 4, 50, &mask) == 1;
    close(ep); close(fds[0]); close(fds[1]);
    printf("epoll_pwait timeout=%d ready=%d\n", timed_out, ready);
    return 0;
}
