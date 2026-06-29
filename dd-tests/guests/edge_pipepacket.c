// EDGE: O_DIRECT "packet mode" pipes (pipe2(O_DIRECT)). Each write becomes a distinct packet, so two
// writes are read back as two separate reads of the original sizes — NOT coalesced into one stream.
// A runtime that ignores O_DIRECT coalesces them. Diffed vs native -> oracle.
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    if (pipe2(fds, O_DIRECT) < 0) { printf("pipepacket pipe2_failed\n"); return 0; }
    fcntl(fds[0], F_SETFL, O_NONBLOCK); // non-blocking so a coalescing bug can't hang the 2nd read
    write(fds[1], "AAA", 3);
    write(fds[1], "BBBBB", 5);
    char b1[64] = {0}, b2[64] = {0};
    ssize_t n1 = read(fds[0], b1, sizeof b1); // packet mode: returns exactly 3 (not 8 coalesced)
    ssize_t n2 = read(fds[0], b2, sizeof b2); // returns exactly 5
    close(fds[0]);
    close(fds[1]);
    printf("pipepacket n1=%ld n2=%ld a=%s b=%s\n", (long)n1, (long)n2, b1, b2); // 3 5 AAA BBBBB
    return 0;
}
