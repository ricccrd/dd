// pidfd_send_signal(2): open a pidfd for a child and signal it via the fd; child dies of SIGTERM.
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int sync[2];
    pipe(sync);
    pid_t c = fork();
    if (c == 0) {
        char go; read(sync[0], &go, 1); // wait until parent has a pidfd
        pause();                         // wait to be killed
        _exit(0);
    }
    int pidfd = syscall(SYS_pidfd_open, c, 0);
    int opened = pidfd >= 0;
    write(sync[1], "g", 1);
    int sent = syscall(SYS_pidfd_send_signal, pidfd, SIGTERM, NULL, 0) == 0;
    // cleanup fallback so the child can never hang the test if pidfd signalling is unsupported;
    // a zombie already killed by SIGTERM is unaffected, so WTERMSIG still distinguishes the two.
    kill(c, SIGKILL);
    int st;
    waitpid(c, &st, 0);
    int killed = WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM;
    if (opened) close(pidfd);
    printf("pidfd_signal opened=%d sent=%d killed=%d\n", opened, sent, killed);
    return 0;
}
