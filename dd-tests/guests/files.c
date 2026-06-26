// open/write/read/unlink a temp file (exercises the fs syscalls in bare mode).
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
int main(void) {
    const char *p = "/tmp/ddtest_files";
    int fd = open(p, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); return 1; }
    write(fd, "payload", 7); close(fd);
    fd = open(p, O_RDONLY); char b[16] = {0}; int n = (int)read(fd, b, 15); close(fd); unlink(p);
    printf("files n=%d data=%s\n", n, b);
    return 0;
}
