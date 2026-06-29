// EDGE: socket recv flags. MSG_PEEK must return data WITHOUT consuming it (a second recv sees the
// same bytes); MSG_DONTWAIT on an empty socket must return EAGAIN immediately (not block). Both are
// flag-translation corners. Diffed vs native -> oracle.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    write(sv[1], "peekme", 6);

    char a[16] = {0}, b[16] = {0};
    ssize_t pn = recv(sv[0], a, sizeof a - 1, MSG_PEEK);  // peek, don't consume
    ssize_t cn = recv(sv[0], b, sizeof b - 1, 0);          // now consume
    int peek_ok = (pn == 6) && !strcmp(a, "peekme") && (cn == 6) && !strcmp(b, "peekme");

    char c[8];
    ssize_t en = recv(sv[0], c, sizeof c, MSG_DONTWAIT);   // empty now -> EAGAIN
    int dontwait_ok = (en < 0) && (errno == EAGAIN || errno == EWOULDBLOCK);
    close(sv[0]);
    close(sv[1]);
    printf("msgflags peek=%d dontwait=%d\n", peek_ok, dontwait_ok); // 1 1
    return 0;
}
