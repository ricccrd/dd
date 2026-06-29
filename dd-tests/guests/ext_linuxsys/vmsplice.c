// vmsplice(2): gather a user buffer into a pipe, then read it back.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
    int p[2];
    // nonblocking so an unimplemented vmsplice() fails fast instead of blocking the reader.
    pipe2(p, O_NONBLOCK);
    char data[] = "vmspliced-payload";
    struct iovec iov = {data, sizeof data - 1};
    long w = vmsplice(p[1], &iov, 1, 0);
    char buf[64] = {0};
    int r = read(p[0], buf, sizeof buf);
    int ok = w == (long)(sizeof data - 1) && r == w && memcmp(buf, data, w) == 0;
    close(p[0]); close(p[1]);
    printf("vmsplice wrote=%ld read=%d ok=%d\n", w, r, ok);
    return 0;
}
