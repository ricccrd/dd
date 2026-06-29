// gettid(2): in a single-threaded process the thread id equals the pid and is positive.
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    pid_t tid = syscall(SYS_gettid);
    pid_t pid = getpid();
    printf("gettid positive=%d eq_pid=%d\n", tid > 0, tid == pid); // 1 1
    return 0;
}
