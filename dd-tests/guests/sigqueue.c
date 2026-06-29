// Realtime signal payload: sigqueue() delivers an integer value alongside SIGUSR1, read out of
// siginfo->si_value in an SA_SIGINFO handler. Linux-specific (macOS has no sigqueue). Diffed
// against a native Linux oracle.
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t got, val;

static void on_usr1(int sig, siginfo_t *si, void *u) {
    (void)sig;
    (void)u;
    got = 1;
    val = si->si_value.sival_int;
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);

    union sigval sv = {.sival_int = 77};
    sigqueue(getpid(), SIGUSR1, sv);
    for (int i = 0; i < 1000000 && !got; i++) {}
    printf("sigqueue got=%d val=%d\n", got, val); // 1 77
    return 0;
}
