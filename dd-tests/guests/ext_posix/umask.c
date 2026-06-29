// umask: setting 022 makes a 0666 create become 0644 on disk; old mask returned.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_umask_%d", (int)getpid());
    unlink(path);
    mode_t old = umask(022);
    int prev_set = umask(022) == 022; // second call returns the 022 we just set
    umask(022);
    int fd = open(path, O_CREAT | O_WRONLY, 0666);
    close(fd);
    struct stat st;
    stat(path, &st);
    int masked = (st.st_mode & 0777) == 0644;
    unlink(path);
    umask(old);
    printf("umask prev_set=%d masked=%d\n", prev_set, masked);
    return 0;
}
