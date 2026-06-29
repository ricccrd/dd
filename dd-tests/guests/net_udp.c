// UDP loopback datagram round-trip on 127.0.0.1. Exercises SOCK_DGRAM, sendto/recvfrom
// with addresses, getsockname for the ephemeral port. Deterministic -> oracle-checked.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int srv = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    bind(srv, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a;
    getsockname(srv, (struct sockaddr *)&a, &al);
    int port = ntohs(a.sin_port);

    pid_t pid = fork();
    if (pid == 0) {
        char buf[64];
        struct sockaddr_in from = {0};
        socklen_t fl = sizeof from;
        ssize_t n = recvfrom(srv, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
        sendto(srv, buf, n, 0, (struct sockaddr *)&from, fl); // echo back
        _exit(0);
    }
    close(srv);
    int cl = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in s = {0};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    s.sin_port = htons(port);
    const char *msg = "datagram-42";
    sendto(cl, msg, strlen(msg), 0, (struct sockaddr *)&s, sizeof s);
    char buf[64] = {0};
    ssize_t n = recvfrom(cl, buf, sizeof buf - 1, 0, NULL, NULL);
    buf[n > 0 ? n : 0] = 0;
    waitpid(pid, NULL, 0);
    printf("udp echo=%s\n", buf);
    return 0;
}
