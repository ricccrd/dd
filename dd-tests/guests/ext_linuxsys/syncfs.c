// sync/syncfs/sync_file_range: all commit cleanly (return 0 / void) on a written file.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    char path[128];
    snprintf(path, sizeof path, "/tmp/dd_syncfs_%d", (int)getpid());
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    write(fd, "syncme", 6);
    int sfr = sync_file_range(fd, 0, 0, SYNC_FILE_RANGE_WRITE) == 0;
    int sf = syncfs(fd) == 0;
    sync(); // void
    close(fd);
    unlink(path);
    printf("syncfs sync_file_range=%d syncfs=%d\n", sfr, sf); // 1 1
    return 0;
}
