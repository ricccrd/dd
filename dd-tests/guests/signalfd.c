// signalfd: block SIGUSR1, raise it, then read the siginfo from the signalfd instead of a
// handler. Exercises sigprocmask + signalfd4 + the ssi_signo read. Deterministic -> oracle.
#include <signal.h>
#include <stdio.h>
#include <sys/signalfd.h>
#include <unistd.h>

int main(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int fd = signalfd(-1, &mask, 0);
    if (fd < 0) { perror("signalfd"); return 1; }
    raise(SIGUSR1);

    struct signalfd_siginfo si;
    ssize_t n = read(fd, &si, sizeof si);
    close(fd);
    printf("signalfd n=%ld signo=%d is_usr1=%d\n", (long)n, si.ssi_signo, si.ssi_signo == SIGUSR1);
    return 0;
}
