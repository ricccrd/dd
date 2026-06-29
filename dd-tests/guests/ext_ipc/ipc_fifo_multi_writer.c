// One FIFO, two concurrent writers: two forked children each stream 1000 ints, the parent sums all
// 2000 values from the single read end. Verifies interleaved writers on a FIFO. Portable, golden.
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    const char *path = "/tmp/dd_fifo_mw";
    unlink(path); mkfifo(path, 0644);
    for (int k = 0; k < 2; k++) {
        if (fork() == 0) { int w = open(path, O_WRONLY); for (int i = 1; i <= 1000; i++) write(w, &i, sizeof i); close(w); _exit(0); }
    }
    int r = open(path, O_RDONLY);
    long sum = 0; int v; int got = 0;
    while (got < 2000 && read(r, &v, sizeof v) == sizeof v) { sum += v; got++; }
    close(r);
    for (int k = 0; k < 2; k++) wait(0);
    unlink(path);
    printf("fifo_mw sum=%ld got=%d\n", sum, got); // 2*(1..1000) = 1001000, 2000
    return 0;
}
