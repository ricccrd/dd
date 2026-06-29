// fcntl: F_GETFL reflects open mode, F_SETFL toggles O_NONBLOCK, F_DUPFD>=N, F_GETFD/F_SETFD FD_CLOEXEC.
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_fcntl_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    int fl = fcntl(fd, F_GETFL);
    int isrw = (fl & O_ACCMODE) == O_RDWR;
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int nb = (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0;
    int d = fcntl(fd, F_DUPFD, 100);
    int dupfd = d >= 100;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int cloexec = (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
    if (d >= 0) close(d);
    close(fd);
    unlink(path);
    printf("fcntlmisc rw=%d nonblock=%d dupfd=%d cloexec=%d\n", isrw, nb, dupfd, cloexec);
    return 0;
}
