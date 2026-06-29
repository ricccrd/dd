// fork/wait status decoding: normal exit code, and a signal-terminated child.
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    // child 1: exit(7)
    pid_t a = fork();
    if (a == 0) _exit(7);
    int st;
    waitpid(a, &st, 0);
    int exited = WIFEXITED(st) && WEXITSTATUS(st) == 7;
    // child 2: killed by SIGKILL
    pid_t b = fork();
    if (b == 0) { raise(SIGKILL); _exit(0); }
    waitpid(b, &st, 0);
    int killed = WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL;
    // child 3: abort -> SIGABRT
    pid_t c = fork();
    if (c == 0) { abort(); }
    waitpid(c, &st, 0);
    int aborted = WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
    printf("waitstatus exit7=%d sigkill=%d sigabrt=%d\n", exited, killed, aborted);
    return 0;
}
