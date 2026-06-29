// stat/fstat/lstat consistency: same size & inode for a regular file; lstat sees the symlink itself.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char path[128], link[128];
    snprintf(path, sizeof path, "/tmp/dd_stat_%d", (int)getpid());
    snprintf(link, sizeof link, "/tmp/dd_stat_%d.lnk", (int)getpid());
    unlink(path); unlink(link);
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    write(fd, "1234567", 7);
    struct stat fs;
    fstat(fd, &fs);
    close(fd);
    struct stat ss;
    stat(path, &ss);
    symlink(path, link);
    struct stat ls;
    lstat(link, &ls);
    int reg = S_ISREG(ss.st_mode);
    int sizeok = ss.st_size == 7 && fs.st_size == 7;
    int sameino = ss.st_ino == fs.st_ino;
    int islink = S_ISLNK(ls.st_mode);
    int statfollow; // stat() on the symlink follows it -> regular, size 7
    struct stat fls;
    stat(link, &fls);
    statfollow = S_ISREG(fls.st_mode) && fls.st_size == 7;
    unlink(path); unlink(link);
    printf("statfam reg=%d size=%d ino=%d lnk=%d follow=%d\n", reg, sizeok, sameino, islink, statfollow);
    return 0;
}
