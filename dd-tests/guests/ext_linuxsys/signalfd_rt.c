// signalfd reading queued realtime signals: block SIGRTMIN, raise it 3x, read 3 siginfo records.
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/signalfd.h>
#include <unistd.h>

int main(void) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGRTMIN);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, 0);
    for (int i = 0; i < 3; i++) raise(SIGRTMIN);
    int reads = 0, signo_ok = 0;
    struct signalfd_siginfo si;
    for (int i = 0; i < 3; i++) {
        if (read(sfd, &si, sizeof si) == sizeof si) {
            reads++;
            if ((int)si.ssi_signo == SIGRTMIN) signo_ok++;
        }
    }
    close(sfd);
    printf("signalfd_rt reads=%d signo_ok=%d\n", reads, signo_ok); // 3 3
    return 0;
}
