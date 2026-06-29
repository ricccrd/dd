// tee(2): duplicate data between two pipes without consuming it, then splice it out.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int a[2], b[2];
    // nonblocking pipes so an unimplemented tee() fails fast (read -> EAGAIN) instead of hanging.
    pipe2(a, O_NONBLOCK); pipe2(b, O_NONBLOCK);
    write(a[1], "teedata", 7);
    long teed = tee(a[0], b[1], 7, 0); // copy a -> b, leaving a intact
    // a still has the data
    char ba[8] = {0};
    int na = read(a[0], ba, 7);
    char bb[8] = {0};
    int nb = read(b[0], bb, 7);
    int ok = teed == 7 && na == 7 && nb == 7;
    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
    printf("tee teed=%ld a_kept=%d b_got=%d ok=%d\n", teed, na == 7, nb == 7, ok);
    return 0;
}
