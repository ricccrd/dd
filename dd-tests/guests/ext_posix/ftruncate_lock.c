// flock(2) advisory whole-file lock: a child's non-blocking flock fails while the parent holds it.
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_flock_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    int locked = flock(fd, LOCK_EX) == 0;
    pid_t c = fork();
    if (c == 0) {
        int cfd = open(path, O_RDWR);
        int rc = flock(cfd, LOCK_EX | LOCK_NB); // should fail: EWOULDBLOCK
        _exit(rc < 0 ? 0 : 1);
    }
    int st; waitpid(c, &st, 0);
    int child_blocked = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    flock(fd, LOCK_UN);
    close(fd);
    unlink(path);
    printf("flock locked=%d child_blocked=%d\n", locked, child_blocked);
    return 0;
}
