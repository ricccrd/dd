// open(2) flag matrix: O_CREAT|O_EXCL collision, O_TRUNC, O_APPEND, O_WRONLY/O_RDONLY.
// Verdicts only (errno compared with platform symbol) -> identical on Linux & macOS.
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_openflags_%d", (int)getpid());
    unlink(path);
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    int created = fd >= 0;
    write(fd, "hello", 5);
    close(fd);
    // O_EXCL on an existing file must fail with EEXIST.
    int again = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    int excl = again < 0 && errno == EEXIST;
    if (again >= 0) close(again);
    // O_APPEND keeps existing data and appends.
    fd = open(path, O_WRONLY | O_APPEND);
    write(fd, "WORLD", 5);
    close(fd);
    char buf[64] = {0};
    fd = open(path, O_RDONLY);
    int n = read(fd, buf, sizeof buf);
    close(fd);
    int appended = n == 10 && memcmp(buf, "helloWORLD", 10) == 0;
    // O_TRUNC empties it.
    fd = open(path, O_WRONLY | O_TRUNC);
    close(fd);
    fd = open(path, O_RDONLY);
    n = read(fd, buf, sizeof buf);
    close(fd);
    int truncated = n == 0;
    unlink(path);
    printf("openflags created=%d excl=%d appended=%d truncated=%d\n", created, excl, appended, truncated);
    return 0;
}
