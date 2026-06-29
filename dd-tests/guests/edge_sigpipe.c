// EDGE: MSG_NOSIGNAL. Writing to a peer-closed socket normally raises SIGPIPE; with MSG_NOSIGNAL the
// send must instead return -1/EPIPE and deliver NO signal. We leave SIGPIPE at its default (fatal) so
// that if the flag is ignored, the process dies — the test then never prints its line. Correct
// behaviour prints the verdict. Deterministic -> golden across engines.
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    close(sv[0]); // peer gone -> writes to sv[1] would SIGPIPE
    signal(SIGPIPE, SIG_DFL); // ensure it's fatal if delivered (so a flag-ignore is visible as death)

    ssize_t n = send(sv[1], "data", 4, MSG_NOSIGNAL);
    int epipe = (n < 0) && (errno == EPIPE);
    close(sv[1]);
    printf("sigpipe survived=1 epipe=%d\n", epipe); // 1 1   (no line at all if SIGPIPE killed us)
    return 0;
}
