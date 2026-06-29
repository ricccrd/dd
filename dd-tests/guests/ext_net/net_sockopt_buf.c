// Send/receive buffer sizing: set SO_SNDBUF and SO_RCVBUF, read them back. Linux doubles the value,
// macOS returns it verbatim, so the check is "got >= requested" (verdict-only). Portable, golden.
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int snd = 65536; int r1 = setsockopt(s, SOL_SOCKET, SO_SNDBUF, &snd, sizeof snd);
    int got = 0; socklen_t l = sizeof got; int r2 = getsockopt(s, SOL_SOCKET, SO_SNDBUF, &got, &l);
    int rcv = 65536; int r3 = setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    int gotr = 0; l = sizeof gotr; int r4 = getsockopt(s, SOL_SOCKET, SO_RCVBUF, &gotr, &l);
    close(s);
    printf("sockopt_buf set_ok=%d snd_ge=%d rcv_ge=%d\n", (r1 | r2 | r3 | r4) == 0, got >= snd, gotr >= rcv);
    return 0;
}
