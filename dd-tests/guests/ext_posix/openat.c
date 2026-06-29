// openat(2): open a file relative to a directory fd, and AT_FDCWD behaves like open().
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/dd_openat_%d", (int)getpid());
    mkdir(dir, 0755);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    int relok = dfd >= 0;
    int fd = openat(dfd, "child.txt", O_CREAT | O_WRONLY, 0644);
    write(fd, "abcd", 4);
    close(fd);
    // read back relative to the dir fd
    char buf[16] = {0};
    fd = openat(dfd, "child.txt", O_RDONLY);
    int n = read(fd, buf, sizeof buf);
    close(fd);
    int readback = n == 4 && memcmp(buf, "abcd", 4) == 0;
    // AT_FDCWD form
    char full[160];
    snprintf(full, sizeof full, "%s/child.txt", dir);
    int fd2 = openat(AT_FDCWD, full, O_RDONLY);
    int atcwd = fd2 >= 0;
    if (fd2 >= 0) close(fd2);
    close(dfd);
    unlinkat(open(dir, O_RDONLY | O_DIRECTORY), "child.txt", 0);
    rmdir(dir);
    printf("openat dirfd=%d readback=%d atcwd=%d\n", relok, readback, atcwd);
    return 0;
}
