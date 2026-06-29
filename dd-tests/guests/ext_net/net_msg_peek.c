// recv(MSG_PEEK) leaves the data in the socket buffer so a subsequent normal recv returns the same
// bytes. Verifies non-destructive peeks over an AF_UNIX stream pair. Portable -> all, golden.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    write(sv[0], "peekdata", 8);
    char p[16] = {0}; ssize_t pn = recv(sv[1], p, 8, MSG_PEEK);
    char r[16] = {0}; ssize_t rn = recv(sv[1], r, 8, 0);
    int same = (strcmp(p, r) == 0);
    close(sv[0]); close(sv[1]);
    printf("msg_peek peeked=%.*s read=%.*s same=%d\n", (int)pn, p, (int)rn, r, same);
    return 0;
}
