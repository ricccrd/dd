// truncate/ftruncate grow (zero-filled) and shrink.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_trunc_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(fd, "0123456789", 10);
    ftruncate(fd, 4);            // shrink to 4
    struct stat st; fstat(fd, &st);
    int shrunk = st.st_size == 4;
    ftruncate(fd, 20);           // grow to 20, zero-filled
    fstat(fd, &st);
    int grew = st.st_size == 20;
    // the grown region reads as zeros
    char buf[20];
    lseek(fd, 0, SEEK_SET);
    read(fd, buf, 20);
    int zeros = buf[10] == 0 && buf[19] == 0;
    close(fd);
    truncate(path, 7);           // path-based
    stat(path, &st);
    int pathtrunc = st.st_size == 7;
    unlink(path);
    printf("truncate shrunk=%d grew=%d zeros=%d pathtrunc=%d\n", shrunk, grew, zeros, pathtrunc);
    return 0;
}
