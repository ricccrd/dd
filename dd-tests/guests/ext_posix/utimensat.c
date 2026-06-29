// utimensat/futimens: set fixed atime/mtime, read them back via stat.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_utime_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    close(fd);
    struct timespec ts[2] = {{1000000000, 0}, {1234567890, 0}}; // atime, mtime
    int u = utimensat(AT_FDCWD, path, ts, 0) == 0;
    struct stat st; stat(path, &st);
    int mtime_ok = st.st_mtime == 1234567890;
    int atime_ok = st.st_atime == 1000000000;
    // futimens via an fd
    fd = open(path, O_RDONLY);
    struct timespec ts2[2] = {{1111111111, 0}, {2000000000, 0}};
    int fu = futimens(fd, ts2) == 0;
    fstat(fd, &st);
    int fmtime_ok = st.st_mtime == 2000000000;
    close(fd);
    unlink(path);
    printf("utimensat set=%d atime=%d mtime=%d futimens=%d fmtime=%d\n", u, atime_ok, mtime_ok, fu, fmtime_ok);
    return 0;
}
