// lockf() POSIX record lock across a fork: child's F_TLOCK fails while the parent holds F_LOCK on
// the whole file, then succeeds after the parent F_ULOCKs (signaled via a pipe). Portable, golden.
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
int main(void) {
    char path[] = "/tmp/dd_lockf_XXXXXX"; int fd = mkstemp(path);
    ftruncate(fd, 100);
    lockf(fd, F_LOCK, 100);
    int p[2]; pipe(p);
    pid_t pid = fork();
    if (pid == 0) {
        int cfd = open(path, O_RDWR);
        int blocked = (lockf(cfd, F_TLOCK, 100) < 0 && (errno == EACCES || errno == EAGAIN));
        char r; read(p[0], &r, 1);
        int ok = lockf(cfd, F_TLOCK, 100) == 0;
        printf("lockf blocked=%d acquired=%d\n", blocked, ok);
        fflush(stdout);
        close(cfd); _exit(0);
    }
    struct timespec ts = {0, 50000000}; nanosleep(&ts, 0);
    lockf(fd, F_ULOCK, 100);
    write(p[1], "g", 1);
    waitpid(pid, 0, 0); close(fd); unlink(path);
    return 0;
}
