// Non-blocking FIFO open semantics: O_WRONLY|O_NONBLOCK with no reader gives ENXIO; O_RDONLY|O_NONBLOCK
// succeeds; once a reader exists O_WRONLY|O_NONBLOCK succeeds and data flows. Portable -> all, golden.
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
int main(void) {
    const char *path = "/tmp/dd_fifo_nb";
    unlink(path); mkfifo(path, 0644);
    int wr = open(path, O_WRONLY | O_NONBLOCK);
    int enxio = (wr < 0 && errno == ENXIO);
    int rd = open(path, O_RDONLY | O_NONBLOCK);
    int rd_ok = rd >= 0;
    int wr2 = open(path, O_WRONLY | O_NONBLOCK);
    int wr_ok = wr2 >= 0;
    write(wr2, "hello", 5);
    char buf[16] = {0}; ssize_t n = read(rd, buf, 16);
    close(rd); if (wr2 >= 0) close(wr2); unlink(path);
    printf("fifo_nb enxio=%d rd_ok=%d wr_ok=%d data=%.*s\n", enxio, rd_ok, wr_ok, (int)(n > 0 ? n : 0), buf);
    return 0;
}
