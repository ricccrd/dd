// #252 regression repro (infinite timeout): epoll_wait(ep, out, n, -1) must NEVER spuriously return 0
// because a PEER thread did an epoll_ctl on the same instance while we were blocked -- it must keep
// blocking until a REAL event. Thread A registers pipe A's read-end then loops in epoll_wait(-1).
// Thread B first registers ANOTHER never-ready fd (fires the engine's internal cross-thread wake) and
// only LATER writes to pipe A (the genuine event). Correct: A ignores the spurious wake (no 0-return)
// and wakes exactly on the real write -> DELIVERED_OK. The bug returned 0 at the wake -> SPURIOUS_ZERO.
// (Arch-independent: does not rely on a signal interrupting a blocked kevent.) A 6s alarm guards a hang.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

static int ep;
static int pa[2];

static void on_alarm(int s) {
    (void)s;
    const char *m = "HANG_TIMEOUT\n";
    if (write(1, m, strlen(m)) < 0) {}
    _exit(2);
}

static void *threadB(void *arg) {
    (void)arg;
    usleep(300000); // A is now blocked in epoll_wait(-1)
    int pb[2];
    if (pipe(pb) == 0) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof ev);
        ev.events = EPOLLIN;
        ev.data.fd = pb[0];
        epoll_ctl(ep, EPOLL_CTL_ADD, pb[0], &ev); // never-ready fd -> internal cross-thread wake, NO real event
    }
    usleep(300000); // still blocked? then deliver the genuine event
    char c = 'x';
    if (write(pa[1], &c, 1) < 0) {}
    return NULL;
}

int main(void) {
    signal(SIGALRM, on_alarm);
    if (pipe(pa) < 0) return 3;
    ep = epoll_create1(0);
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.events = EPOLLIN;
    ev.data.fd = pa[0];
    epoll_ctl(ep, EPOLL_CTL_ADD, pa[0], &ev);
    pthread_t tb;
    pthread_create(&tb, NULL, threadB, NULL); // makes the guest multi-threaded (g_threaded=1)
    alarm(6);
    struct epoll_event out[8];
    for (;;) {
        int r = epoll_wait(ep, out, 8, -1);
        if (r == 0) { // infinite timeout must never yield 0
            const char *m = "SPURIOUS_ZERO\n";
            if (write(1, m, strlen(m)) < 0) {}
            _exit(1);
        }
        if (out[0].data.fd == pa[0]) { // the genuine event finally arrived
            const char *m = "DELIVERED_OK\n";
            if (write(1, m, strlen(m)) < 0) {}
            _exit(0);
        }
        // any other fd: keep waiting (shouldn't happen in this repro)
    }
}
