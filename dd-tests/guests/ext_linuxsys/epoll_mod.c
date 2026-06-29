// EPOLL_CTL_MOD and EPOLL_CTL_DEL: change interest, then drop the fd entirely.
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/epoll.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    pipe(fds);
    int ep = epoll_create1(0);
    struct epoll_event out[4];
    // initially watch only writability of the write end
    struct epoll_event ev = {.events = EPOLLOUT, .data.fd = fds[1]};
    epoll_ctl(ep, EPOLL_CTL_ADD, fds[1], &ev);
    int writable = epoll_wait(ep, out, 4, 100) == 1 && (out[0].events & EPOLLOUT);
    // watch the read end for input
    ev.events = EPOLLIN; ev.data.fd = fds[0];
    epoll_ctl(ep, EPOLL_CTL_ADD, fds[0], &ev);
    write(fds[1], "x", 1);
    // MOD the write end to no events; expect only the read end to report
    struct epoll_event nev = {.events = 0, .data.fd = fds[1]};
    epoll_ctl(ep, EPOLL_CTL_MOD, fds[1], &nev);
    int n = epoll_wait(ep, out, 4, 100);
    int only_read = n == 1 && out[0].data.fd == fds[0];
    // DEL the read end; drain it; nothing left ready
    epoll_ctl(ep, EPOLL_CTL_DEL, fds[0], NULL);
    char c; read(fds[0], &c, 1);
    int after_del = epoll_wait(ep, out, 4, 50) == 0;
    close(ep); close(fds[0]); close(fds[1]);
    printf("epoll_mod writable=%d only_read=%d after_del=%d\n", writable, only_read, after_del);
    return 0;
}
