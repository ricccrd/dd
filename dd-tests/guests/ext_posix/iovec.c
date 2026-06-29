// readv/writev scatter-gather across three buffers.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_iovec_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    struct iovec wv[3] = {
        {(void *)"AAA", 3}, {(void *)"BBBB", 4}, {(void *)"CC", 2},
    };
    ssize_t w = writev(fd, wv, 3); // 9 bytes
    lseek(fd, 0, SEEK_SET);
    char a[3], b[4], c[2];
    struct iovec rv[3] = {{a, 3}, {b, 4}, {c, 2}};
    ssize_t r = readv(fd, rv, 3);
    int ok = w == 9 && r == 9 && memcmp(a, "AAA", 3) == 0 && memcmp(b, "BBBB", 4) == 0 && memcmp(c, "CC", 2) == 0;
    close(fd);
    unlink(path);
    printf("iovec wrote=%d read=%d ok=%d\n", (int)w, (int)r, ok);
    return 0;
}
