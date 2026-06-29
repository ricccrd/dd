// unshare(CLONE_FILES): detach the fd table; an already-open fd remains valid afterward.
#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_unshare_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(fd, "abc", 3);
    int un = unshare(CLONE_FILES) == 0;
    // the fd survives the fd-table unshare
    lseek(fd, 0, SEEK_SET);
    char buf[4] = {0};
    int n = read(fd, buf, 3);
    int survived = n == 3 && memcmp(buf, "abc", 3) == 0;
    close(fd);
    unlink(path);
    printf("unshare_files unshare=%d fd_survived=%d\n", un, survived);
    return 0;
}
