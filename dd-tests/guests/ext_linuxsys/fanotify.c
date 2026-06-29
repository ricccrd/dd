// fanotify_init(2): requires CAP_SYS_ADMIN. As an unprivileged guest it must fail with EPERM
// (the real-Linux contract) rather than ENOSYS. Raw syscall to avoid header availability issues.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    int fd = syscall(SYS_fanotify_init, 0 /*FAN_CLASS_NOTIF*/, 0);
    int ok = fd >= 0;
    int eperm = fd < 0 && errno == EPERM;
    if (ok) close(fd);
    printf("fanotify ok=%d eperm=%d\n", ok, eperm);
    return 0;
}
