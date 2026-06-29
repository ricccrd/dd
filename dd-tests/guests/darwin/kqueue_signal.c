// macOS-native kqueue EVFILT_SIGNAL: ignore SIGUSR1 then raise it; kevent reports the signal pending.
// (EVFILT_SIGNAL observes signals independently of disposition once ignored.) darwin engine only.
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <sys/event.h>

int main(void) {
    signal(SIGUSR1, SIG_IGN);
    int kq = kqueue();
    struct kevent ch;
    EV_SET(&ch, SIGUSR1, EVFILT_SIGNAL, EV_ADD | EV_ENABLE, 0, 0, NULL);
    kevent(kq, &ch, 1, NULL, 0, NULL);
    raise(SIGUSR1);
    struct kevent ev;
    struct timespec to = {1, 0};
    int n = kevent(kq, NULL, 0, &ev, 1, &to);
    int ok = (n == 1) && (ev.filter == EVFILT_SIGNAL) && (ev.ident == SIGUSR1);
    printf("kqueue signal=%d\n", ok); // 1
    return 0;
}
