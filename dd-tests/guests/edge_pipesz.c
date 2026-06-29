// EDGE: pipe capacity control + dup3 self-dup. F_SETPIPE_SZ resizes a pipe's buffer and F_GETPIPE_SZ
// reads it back (>= requested, rounded up); dup3(fd, fd, 0) with equal fds must fail EINVAL (unlike
// dup2, which returns fd). Both are Linux-specific fcntl/dup corners. Diffed vs native -> oracle.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#define F_GETPIPE_SZ 1032
#endif

int main(void) {
    int fds[2];
    pipe(fds);
    int set = fcntl(fds[1], F_SETPIPE_SZ, 256 * 1024);
    int got = fcntl(fds[1], F_GETPIPE_SZ);
    int size_ok = (set >= 0) && (got >= 256 * 1024);

    int r = dup3(fds[0], fds[0], 0); // equal oldfd==newfd -> EINVAL on Linux
    int dup3_ok = (r < 0) && (errno == EINVAL);
    close(fds[0]);
    close(fds[1]);
    printf("pipesz size_ok=%d dup3_einval=%d\n", size_ok, dup3_ok); // 1 1
    return 0;
}
