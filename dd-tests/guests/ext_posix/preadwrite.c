// pread/pwrite: positioned IO must NOT move the file offset.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_prw_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    char zeros[100] = {0};
    write(fd, zeros, sizeof zeros);
    lseek(fd, 0, SEEK_SET);
    // pwrite at offset 50 without touching the offset
    pwrite(fd, "XYZ", 3, 50);
    off_t after_pwrite = lseek(fd, 0, SEEK_CUR); // still 0
    char buf[3] = {0};
    pread(fd, buf, 3, 50);
    off_t after_pread = lseek(fd, 0, SEEK_CUR); // still 0
    int match = memcmp(buf, "XYZ", 3) == 0;
    close(fd);
    unlink(path);
    printf("preadwrite match=%d off_kept=%d\n", match, after_pwrite == 0 && after_pread == 0);
    return 0;
}
