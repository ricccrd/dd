// One TCP server, three sequential clients over loopback: each client sends "req-N", the server
// upper-cases nothing but echoes it back; the parent sums all echoed bytes (1266). Portable, golden.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (struct sockaddr *)&a, sizeof a); listen(ls, 8);
    socklen_t al = sizeof a; getsockname(ls, (struct sockaddr *)&a, &al);
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < 3; i++) { int cs = accept(ls, 0, 0); char buf[64]; ssize_t n = recv(cs, buf, 64, 0); send(cs, buf, n, 0); close(cs); }
        _exit(0);
    }
    close(ls);
    long total = 0;
    for (int i = 0; i < 3; i++) {
        int cs = socket(AF_INET, SOCK_STREAM, 0);
        connect(cs, (struct sockaddr *)&a, sizeof a);
        char msg[16]; int m = snprintf(msg, 16, "req-%d", i); send(cs, msg, m, 0);
        char buf[64] = {0}; ssize_t n = recv(cs, buf, 63, 0);
        for (ssize_t k = 0; k < n; k++) total += (unsigned char)buf[k];
        close(cs);
    }
    waitpid(pid, 0, 0);
    printf("tcp_multi total=%ld\n", total); // 1266
    return 0;
}
