// Connected UDP: the client calls connect() on a SOCK_DGRAM socket then uses send/recv (no address);
// the server echoes three datagrams; the parent sums echoed bytes (756). Portable -> all, golden.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    int srv = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(srv, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a; getsockname(srv, (struct sockaddr *)&a, &al);
    pid_t pid = fork();
    if (pid == 0) {
        char buf[64]; struct sockaddr_in from; socklen_t fl = sizeof from;
        for (int i = 0; i < 3; i++) { ssize_t n = recvfrom(srv, buf, 64, 0, (struct sockaddr *)&from, &fl); sendto(srv, buf, n, 0, (struct sockaddr *)&from, fl); }
        _exit(0);
    }
    close(srv);
    int cl = socket(AF_INET, SOCK_DGRAM, 0);
    connect(cl, (struct sockaddr *)&a, sizeof a);
    long total = 0;
    for (int i = 0; i < 3; i++) { char m[16]; int len = snprintf(m, 16, "dg%d", i); send(cl, m, len, 0); char buf[64] = {0}; ssize_t n = recv(cl, buf, 63, 0); for (ssize_t k = 0; k < n; k++) total += (unsigned char)buf[k]; }
    waitpid(pid, 0, 0);
    printf("udp_connected total=%ld\n", total); // 756
    return 0;
}
