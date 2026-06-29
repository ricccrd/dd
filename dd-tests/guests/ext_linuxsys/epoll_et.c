// EPOLLET edge-triggered: a level fd would re-fire while readable; edge fires only on NEW data.
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    pipe(fds);
    int ep = epoll_create1(0);
    struct epoll_event ev = {.events = EPOLLIN | EPOLLET, .data.fd = fds[0]};
    epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev);
    struct epoll_event out[4];

    write(fds[1], "a", 1);
    int first = epoll_wait(ep, out, 4, 100);    // 1: data arrived
    // do NOT read; edge-triggered must NOT re-fire with no new data
    int second = epoll_wait(ep, out, 4, 100);   // 0: no new edge
    write(fds[1], "b", 1);                       // new data -> new edge
    int third = epoll_wait(ep, out, 4, 100);     // 1
    close(ep); close(fds[0]); close(fds[1]);
    printf("epoll_et first=%d second=%d third=%d\n", first, second, third); // 1 0 1
    return 0;
}
