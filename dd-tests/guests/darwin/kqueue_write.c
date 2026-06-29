// macOS-native kqueue EVFILT_WRITE: a fresh pipe's write end is immediately writable (buffer space).
// darwin engine only, golden-checked.
#include <stdio.h>
#include <sys/event.h>
#include <unistd.h>
#include <stdint.h>

int main(void) {
    int fds[2];
    if (pipe(fds) < 0) return 1;
    int kq = kqueue();
    struct kevent ch;
    EV_SET(&ch, fds[1], EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, NULL);
    kevent(kq, &ch, 1, NULL, 0, NULL);
    struct kevent ev;
    struct timespec to = {1, 0};
    int n = kevent(kq, NULL, 0, &ev, 1, &to);
    int ok = (n == 1) && (ev.filter == EVFILT_WRITE) && (ev.ident == (uintptr_t)fds[1]);
    printf("kqueue writable=%d\n", ok); // 1
    return 0;
}
