// flock() advisory whole-file lock across a fork: child's LOCK_EX|LOCK_NB fails while the parent
// holds LOCK_EX, then succeeds after the parent unlocks (signaled via a pipe). Portable, golden.
#include <sys/file.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
int main(void) {
    char path[] = "/tmp/dd_flock_XXXXXX"; int fd = mkstemp(path);
    flock(fd, LOCK_EX);
    int p[2]; pipe(p);
    pid_t pid = fork();
    if (pid == 0) {
        int cfd = open(path, O_RDWR);
        int nb = flock(cfd, LOCK_EX | LOCK_NB);
        int blocked = (nb < 0 && (errno == EWOULDBLOCK || errno == EAGAIN));
        char r; read(p[0], &r, 1);
        int ok = flock(cfd, LOCK_EX | LOCK_NB) == 0;
        printf("flock child_blocked=%d child_acquired=%d\n", blocked, ok);
        fflush(stdout);
        close(cfd); _exit(0);
    }
    struct timespec ts = {0, 50000000}; nanosleep(&ts, 0);
    flock(fd, LOCK_UN);
    write(p[1], "g", 1);
    waitpid(pid, 0, 0); close(fd); unlink(path);
    return 0;
}
