// POSIX advisory file locking (fcntl F_SETLK/F_GETLK) across a fork. Parent takes a write
// lock, child probes it with F_GETLK and reports the holder, then parent releases and the
// child acquires. Exercises fcntl lock structs + cross-process visibility. Deterministic.
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    const char *path = "/tmp/dd_filelock.bin";
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    ftruncate(fd, 16);

    struct flock fl = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 16};
    fcntl(fd, F_SETLK, &fl); // parent holds the lock

    int pp[2];
    pipe(pp);
    pid_t pid = fork();
    if (pid == 0) {
        close(pp[0]);
        struct flock q = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 16};
        fcntl(fd, F_GETLK, &q); // who holds it?
        int blocked = (q.l_type != F_UNLCK);
        write(pp[1], &blocked, sizeof blocked);
        _exit(0);
    }
    close(pp[1]);
    int blocked = 0;
    read(pp[0], &blocked, sizeof blocked);
    waitpid(pid, NULL, 0);

    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl); // release
    struct flock q2 = {.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 16};
    fcntl(fd, F_GETLK, &q2);
    int free_now = (q2.l_type == F_UNLCK);
    close(fd);
    unlink(path);
    printf("filelock blocked=%d free_after=%d\n", blocked, free_now); // 1 1
    return 0;
}
