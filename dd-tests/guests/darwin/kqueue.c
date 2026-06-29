// macOS-native kqueue: register a pipe's read end with EVFILT_READ, write to it, and confirm kevent
// reports the fd readable with the right byte count. The BSD event-notification primitive (the
// counterpart to Linux epoll). darwin engine only, golden-checked.
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    if (pipe(fds) < 0) { perror("pipe"); return 1; }
    int kq = kqueue();
    struct kevent ch;
    EV_SET(&ch, fds[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, NULL);
    kevent(kq, &ch, 1, NULL, 0, NULL);

    write(fds[1], "hello", 5);
    struct kevent ev;
    struct timespec to = {1, 0};
    int n = kevent(kq, NULL, 0, &ev, 1, &to);
    int readable = (n == 1) && (ev.filter == EVFILT_READ) && (ev.ident == (uintptr_t)fds[0]);
    long bytes = (n == 1) ? (long)ev.data : -1;
    close(kq);
    close(fds[0]);
    close(fds[1]);
    printf("kqueue readable=%d bytes=%ld\n", readable, bytes); // 1 5
    return 0;
}
