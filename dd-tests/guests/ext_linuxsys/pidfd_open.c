// pidfd_open(2): obtain a pidfd for our own pid; it is a valid fd and fstat succeeds.
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    int pidfd = syscall(SYS_pidfd_open, getpid(), 0);
    int opened = pidfd >= 0;
    struct stat st;
    int statok = opened && fstat(pidfd, &st) == 0;
    if (opened) close(pidfd);
    printf("pidfd_open opened=%d statok=%d\n", opened, statok); // 1 1
    return 0;
}
