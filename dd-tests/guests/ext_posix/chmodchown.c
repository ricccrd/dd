// chmod sets permission bits; chown to our own uid/gid is a no-op that returns 0.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_chmod_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    close(fd);
    int c1 = chmod(path, 0600) == 0;
    struct stat st; stat(path, &st);
    int is600 = (st.st_mode & 0777) == 0600;
    int c2 = fchmod(open(path, O_RDONLY), 0755) == 0;
    stat(path, &st);
    int is755 = (st.st_mode & 0777) == 0755;
    int ch = chown(path, getuid(), getgid()) == 0; // self -> allowed
    unlink(path);
    printf("chmodchown chmod=%d m600=%d fchmod=%d m755=%d chown=%d\n", c1, is600, c2, is755, ch);
    return 0;
}
