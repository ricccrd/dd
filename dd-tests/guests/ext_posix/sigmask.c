// sigprocmask + sigpending: block SIGUSR1, raise it (stays pending), unblock -> handler fires once.
#include <signal.h>
#include <stdio.h>

static volatile sig_atomic_t got = 0;
static void h(int s) { (void)s; got++; }

int main(void) {
    signal(SIGUSR1, h);
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigprocmask(SIG_BLOCK, &block, &old);
    raise(SIGUSR1);
    int not_yet = got == 0; // blocked -> handler hasn't run
    sigset_t pend;
    sigpending(&pend);
    int pending = sigismember(&pend, SIGUSR1) == 1;
    sigprocmask(SIG_UNBLOCK, &block, NULL); // delivers now
    int delivered = got == 1;
    printf("sigmask not_yet=%d pending=%d delivered=%d\n", not_yet, pending, delivered);
    return 0;
}
