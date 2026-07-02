// #252 regression repro (finite timeout): epoll_wait(ep, out, n, 300) with a cross-thread epoll_ctl at
// t=100ms must still return 0 at ~300ms -- not early (the bug would return 0 at ~100ms on the peer wake)
// and not never. Prints FINITE_OK when it returns 0 within [250,400]ms, else FINITE_BAD with details.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

static int ep;

static void *threadB(void *arg) {
    (void)arg;
    usleep(100000); // t=100ms: fire a cross-thread ctl while A is blocked with 300ms budget
    int p[2];
    if (pipe(p) < 0) return NULL;
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.fd = p[0];
    epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev);
    return NULL;
}

int main(void) {
    int pa[2];
    if (pipe(pa) < 0) return 3;
    ep = epoll_create1(0);
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.fd = pa[0];
    epoll_ctl(ep, EPOLL_CTL_ADD, pa[0], &ev);
    pthread_t tb;
    pthread_create(&tb, NULL, threadB, NULL);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct epoll_event out[8];
    int r = epoll_wait(ep, out, 8, 300);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    if (r == 0 && ms >= 250 && ms <= 400)
        printf("FINITE_OK ms=%ld\n", ms);
    else
        printf("FINITE_BAD r=%d ms=%ld\n", r, ms);
    return 0;
}
