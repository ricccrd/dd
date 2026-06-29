// Pipe EOF and EPIPE: writing to a pipe whose read end is closed yields EPIPE (SIGPIPE ignored);
// reading a pipe whose write end closed yields 0 (EOF) after the buffered data. Portable, golden.
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <errno.h>
int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int p[2]; pipe(p);
    close(p[0]);
    ssize_t w = write(p[1], "x", 1);
    int epipe = (w < 0 && errno == EPIPE);
    close(p[1]);
    int q[2]; pipe(q);
    write(q[1], "hi", 2); close(q[1]);
    char buf[8]; ssize_t n1 = read(q[0], buf, 8); ssize_t n2 = read(q[0], buf, 8);
    close(q[0]);
    printf("pipe_eof epipe=%d first=%ld eof=%ld\n", epipe, (long)n1, (long)n2); // 1 2 0
    return 0;
}
