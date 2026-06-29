// fcntl flag manipulation: F_DUPFD (dup above a floor), F_GETFD/F_SETFD for FD_CLOEXEC, and
// F_GETFL/F_SETFL for O_NONBLOCK on a pipe. Exercises the non-locking fcntl commands every
// runtime/event-loop uses. Portable -> all engines, golden-checked.
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    pipe(fds);

    int dupd = fcntl(fds[0], F_DUPFD, 100); // lowest free fd >= 100
    int dup_ok = dupd >= 100;

    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    int cloexec = (fcntl(fds[1], F_GETFD) & FD_CLOEXEC) != 0;

    int fl = fcntl(fds[0], F_GETFL);
    fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    int nonblock = (fcntl(fds[0], F_GETFL) & O_NONBLOCK) != 0;

    close(dupd);
    close(fds[0]);
    close(fds[1]);
    printf("fcntl dupfd=%d cloexec=%d nonblock=%d\n", dup_ok, cloexec, nonblock); // 1 1 1
    return 0;
}
