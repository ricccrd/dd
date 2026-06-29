// access/faccessat: F_OK/R_OK/W_OK on an existing file, ENOENT on a missing one.
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_access_%d", (int)getpid());
    unlink(path);
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    close(fd);
    int f = access(path, F_OK) == 0;
    int r = access(path, R_OK) == 0;
    int w = access(path, W_OK) == 0;
    int missing = access("/tmp/dd_no_such_file_zzz", F_OK) < 0 && errno == ENOENT;
    int at = faccessat(AT_FDCWD, path, F_OK, 0) == 0;
    unlink(path);
    printf("access f=%d r=%d w=%d missing=%d at=%d\n", f, r, w, missing, at);
    return 0;
}
