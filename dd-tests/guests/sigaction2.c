// Signal handling: install an SA_SIGINFO handler for SIGUSR1 (reads siginfo->si_signo), raise it
// via kill(), and separately reap a child with an SA_SIGINFO SIGCHLD handler. Exercises
// sigaction/SA_SIGINFO + async handler delivery + SIGCHLD. Portable (sigqueue/si_value, which macOS
// lacks, is covered by the Linux-only `sigqueue` case). Golden-checked across engines.
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t got_usr1, usr1_signo, got_chld;

static void on_usr1(int sig, siginfo_t *si, void *u) {
    (void)sig;
    (void)u;
    got_usr1 = 1;
    usr1_signo = si->si_signo;
}
static void on_chld(int sig, siginfo_t *si, void *u) {
    (void)sig;
    (void)si;
    (void)u;
    got_chld = 1;
}

int main(void) {
    struct sigaction sa = {0};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_sigaction = on_chld;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);

    kill(getpid(), SIGUSR1);
    for (int i = 0; i < 1000000 && !got_usr1; i++) {} // spin until delivered

    pid_t pid = fork();
    if (pid == 0) _exit(0);
    int st;
    waitpid(pid, &st, 0);
    // SIGUSR1's number differs across platforms (10 on Linux, 30 on macOS) -> print a verdict.
    printf("sigaction usr1=%d signo_ok=%d chld=%d\n", got_usr1, usr1_signo == SIGUSR1, got_chld); // 1 1 1
    return 0;
}
