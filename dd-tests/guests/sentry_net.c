// sentry_net -- a single-process AF_INET/UDP loopback echo used to validate the untrusted-guest SENTRY
// split's SOCKET family. socket/bind/getsockname/sendto/recvfrom all live in the sentry's forwarded set,
// so DDJIT_UNTRUSTED=1 forces the socket lifecycle + the sockaddr/data marshaling across the ring (the
// real socket fd is sentry-owned and never visible to the worker). No fork: the socket sends a datagram
// to its OWN bound loopback address and reads it back, isolating the socket-forwarding path from the
// fork/clone lane. Registered TWICE (trusted baseline + .untrusted()) against the SAME golden line, so
// the forwarded result must equal the trusted one byte-for-byte. Deterministic -> golden-checked.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(void) {
    int s = socket(AF_INET, SOCK_DGRAM, 0); // sentry-owned fd, virtual to the worker
    if (s < 0) { perror("socket"); return 1; }

    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0; // ephemeral: the kernel picks the port
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0) { perror("bind"); return 1; }

    // getsockname: learn the bound port (out-sockaddr + in/out socklen copied back from the ring).
    socklen_t al = sizeof a;
    if (getsockname(s, (struct sockaddr *)&a, &al) != 0) { perror("getsockname"); return 1; }

    // sendto OUR OWN address (in-data + in-destaddr marshaled to the ring); loopback queues it locally.
    const char *msg = "datagram-echo-42";
    ssize_t sent = sendto(s, msg, strlen(msg), 0, (struct sockaddr *)&a, sizeof a);
    if (sent != (ssize_t)strlen(msg)) { perror("sendto"); return 1; }

    // recvfrom (out-data + out-srcaddr + in/out socklen): the queued datagram comes straight back.
    char rb[128] = {0};
    struct sockaddr_in from = {0};
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(s, rb, sizeof rb - 1, 0, (struct sockaddr *)&from, &fl);
    if (n < 0) { perror("recvfrom"); return 1; }
    rb[n] = 0;

    close(s);
    printf("sentry_net echo=%s len=%zd\n", rb, (ssize_t)n);
    return 0;
}
