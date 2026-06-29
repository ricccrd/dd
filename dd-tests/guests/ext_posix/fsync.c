// fsync/fdatasync on a written file return 0; data survives a reopen.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_fsync_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(fd, "durable", 7);
    int fs = fsync(fd) == 0;
    write(fd, "-more", 5);
    int fds = fdatasync(fd) == 0;
    close(fd);
    char buf[16] = {0};
    fd = open(path, O_RDONLY);
    int n = read(fd, buf, sizeof buf);
    close(fd);
    int survived = n == 12 && memcmp(buf, "durable-more", 12) == 0;
    unlink(path);
    printf("fsync fsync=%d fdatasync=%d survived=%d\n", fs, fds, survived);
    return 0;
}
