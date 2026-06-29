// dup3(2) Linux: like dup2 but takes a flags arg (O_CLOEXEC) and rejects oldfd==newfd.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_dup3_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    int nf = dup3(fd, 50, O_CLOEXEC);
    int dup3ok = nf == 50;
    int cloexec = (fcntl(50, F_GETFD) & FD_CLOEXEC) != 0;
    // dup3(x,x,..) must fail EINVAL (unlike dup2 which would no-op)
    int sameerr = dup3(fd, fd, 0) < 0 && errno == EINVAL;
    close(fd); close(50);
    unlink(path);
    printf("dup3 ok=%d cloexec=%d sameerr=%d\n", dup3ok, cloexec, sameerr);
    return 0;
}
