// read/write basic roundtrip + partial reads. Byte-sum verdict (deterministic).
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_rw_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    unsigned char data[256];
    for (int i = 0; i < 256; i++) data[i] = (unsigned char)i;
    ssize_t w = write(fd, data, sizeof data);
    lseek(fd, 0, SEEK_SET);
    long sum = 0;
    int reads = 0;
    char buf[37]; // odd size -> several partial reads
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i < r; i++) sum += (unsigned char)buf[i];
        reads++;
    }
    close(fd);
    unlink(path);
    printf("rw wrote=%d sum=%ld reads=%d\n", (int)w, sum, reads); // 256, 32640, 8
    return 0;
}
