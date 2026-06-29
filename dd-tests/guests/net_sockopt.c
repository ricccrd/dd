// Socket options: set/get SO_REUSEADDR, SO_RCVBUF, TCP_NODELAY, and read SO_ERROR. Exercises
// getsockopt/setsockopt across SOL_SOCKET and IPPROTO_TCP. Deterministic verdict -> oracle.
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    int r1 = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    int got = 0;
    socklen_t gl = sizeof got;
    int r2 = getsockopt(s, SOL_SOCKET, SO_REUSEADDR, &got, &gl);

    int nd = 1;
    int r3 = setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &nd, sizeof nd);
    int ndg = 0;
    gl = sizeof ndg;
    int r4 = getsockopt(s, IPPROTO_TCP, TCP_NODELAY, &ndg, &gl);

    int err = -1;
    gl = sizeof err;
    int r5 = getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &gl); // freshly opened -> 0
    close(s);
    printf("sockopt reuse=%d nodelay=%d soerr=%d ok=%d\n", got != 0, ndg != 0, err,
           (r1 | r2 | r3 | r4 | r5) == 0);
    return 0;
}
