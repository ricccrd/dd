// dup/dup2: duplicated fds share the same open-file offset; dup2 onto a target closes it first.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_dup_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(fd, "0123456789", 10);
    int d = dup(fd);
    int dupped = d >= 0 && d != fd;
    // advancing one fd advances the shared offset
    lseek(fd, 4, SEEK_SET);
    off_t shared = lseek(d, 0, SEEK_CUR);
    int sharedoff = shared == 4;
    // dup2 to a chosen number
    int target = 20;
    int d2 = dup2(fd, target);
    int dup2ok = d2 == target;
    char buf[4] = {0};
    lseek(d2, 0, SEEK_SET);
    read(d2, buf, 4);
    int readok = memcmp(buf, "0123", 4) == 0;
    close(fd); close(d); close(d2);
    unlink(path);
    printf("dup dupped=%d sharedoff=%d dup2=%d read=%d\n", dupped, sharedoff, dup2ok, readok);
    return 0;
}
