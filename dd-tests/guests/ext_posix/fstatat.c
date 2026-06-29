// fstatat(2): stat relative to a dir fd, and AT_SYMLINK_NOFOLLOW sees the link itself.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char dir[128];
    snprintf(dir, sizeof dir, "/tmp/dd_fstatat_%d", (int)getpid());
    mkdir(dir, 0755);
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    int fd = openat(dfd, "f", O_CREAT | O_WRONLY, 0644);
    write(fd, "ab", 2);
    close(fd);
    struct stat st;
    int rel = fstatat(dfd, "f", &st, 0) == 0 && st.st_size == 2;
    symlinkat("f", dfd, "l");
    struct stat ls;
    int nofollow = fstatat(dfd, "l", &ls, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(ls.st_mode);
    struct stat fs;
    int follow = fstatat(dfd, "l", &fs, 0) == 0 && S_ISREG(fs.st_mode) && fs.st_size == 2;
    unlinkat(dfd, "f", 0);
    unlinkat(dfd, "l", 0);
    close(dfd);
    rmdir(dir);
    printf("fstatat rel=%d nofollow=%d follow=%d\n", rel, nofollow, follow);
    return 0;
}
