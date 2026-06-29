// lseek SEEK_SET/CUR/END and seeking past EOF to create a sparse hole.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_lseek_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(fd, "0123456789", 10);
    off_t set = lseek(fd, 3, SEEK_SET);      // 3
    off_t cur = lseek(fd, 2, SEEK_CUR);      // 5
    off_t end = lseek(fd, 0, SEEK_END);      // 10
    // seek past EOF and write -> file grows with a hole
    lseek(fd, 1000, SEEK_SET);
    write(fd, "Z", 1);
    struct stat st;
    fstat(fd, &st);
    close(fd);
    unlink(path);
    printf("lseek set=%d cur=%d end=%d size=%ld\n", (int)set, (int)cur, (int)end, (long)st.st_size); // 3 5 10 1001
    return 0;
}
