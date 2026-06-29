// statx(2): Linux extended stat. Diffed vs native -> raw fields are fine (same binary runs natively).
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_statx_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    write(fd, "1234567890", 10);
    close(fd);
    struct statx stx;
    int rc = statx(AT_FDCWD, path, 0, STATX_BASIC_STATS, &stx);
    int sizeok = (stx.stx_mask & STATX_SIZE) && stx.stx_size == 10;
    int reg = (stx.stx_mode & S_IFMT) == S_IFREG;
    int nlink = stx.stx_nlink == 1;
    unlink(path);
    printf("statx rc=%d size=%d reg=%d nlink=%d\n", rc, sizeok, reg, nlink);
    return 0;
}
