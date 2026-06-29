// macOS-native kqueue EVFILT_USER: a purely user-triggered event (NOTE_TRIGGER) — the BSD way to
// wake a kevent loop from another context. No Linux equivalent (Linux uses eventfd). darwin only.
#include <stdio.h>
#include <time.h>
#include <sys/event.h>

int main(void) {
    int kq = kqueue();
    struct kevent ch;
    EV_SET(&ch, 0x1234, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    kevent(kq, &ch, 1, NULL, 0, NULL);
    struct kevent tr;
    EV_SET(&tr, 0x1234, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
    kevent(kq, &tr, 1, NULL, 0, NULL);
    struct kevent ev;
    struct timespec to = {1, 0};
    int n = kevent(kq, NULL, 0, &ev, 1, &to);
    int ok = (n == 1) && (ev.filter == EVFILT_USER) && (ev.ident == 0x1234);
    printf("kqueue user=%d\n", ok); // 1
    return 0;
}
