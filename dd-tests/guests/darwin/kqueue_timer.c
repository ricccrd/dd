// macOS-native kqueue EVFILT_TIMER: a oneshot timer that fires after 10ms. No Linux equivalent in
// this form (Linux uses timerfd). darwin engine only, golden-checked.
#include <stdio.h>
#include <sys/event.h>
#include <stdint.h>

int main(void) {
    int kq = kqueue();
    struct kevent ch;
    EV_SET(&ch, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_USECONDS, 10000, NULL); // 10ms
    kevent(kq, &ch, 1, NULL, 0, NULL);
    struct kevent ev;
    int n = kevent(kq, NULL, 0, &ev, 1, NULL); // block until it fires
    int ok = (n == 1) && (ev.filter == EVFILT_TIMER) && (ev.ident == 1);
    printf("kqueue timer fired=%d\n", ok); // 1
    return 0;
}
