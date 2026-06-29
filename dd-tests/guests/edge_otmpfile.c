// EDGE: O_TMPFILE — open a directory with O_TMPFILE to get an unnamed, auto-cleaned regular file,
// write+read it, and confirm it has no name (link count 0, not present by any path). Linux-only flag;
// macOS has no equivalent, so open(O_TMPFILE) typically fails (EISDIR/EINVAL). Diffed vs native ->
// oracle.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    int fd = open("/tmp", O_TMPFILE | O_RDWR, 0600);
    if (fd < 0) { printf("otmpfile open_failed errno_path\n"); return 0; } // native success vs JIT failure diverge here
    write(fd, "anon-tmpfile", 12);
    char buf[16] = {0};
    pread(fd, buf, 12, 0);
    struct stat st;
    fstat(fd, &st);
    close(fd);
    printf("otmpfile data=%s nlink=%ld\n", buf, (long)st.st_nlink); // anon-tmpfile 0
    return 0;
}
