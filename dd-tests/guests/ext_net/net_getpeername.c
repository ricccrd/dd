// getsockname/getpeername on a connected TCP pair: the client's peer == the server's listen port,
// and the server's local port matches too. Verifies socket name introspection. Portable, golden.
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
    bind(ls, (struct sockaddr *)&a, sizeof a); listen(ls, 4);
    socklen_t al = sizeof a; getsockname(ls, (struct sockaddr *)&a, &al);
    int port = ntohs(a.sin_port);
    pid_t pid = fork();
    if (pid == 0) {
        int cs = accept(ls, 0, 0);
        struct sockaddr_in sn; socklen_t sl = sizeof sn; getsockname(cs, (struct sockaddr *)&sn, &sl);
        int srvport_ok = (ntohs(sn.sin_port) == port);
        char out[32]; int m = snprintf(out, 32, "srvport=%d", srvport_ok); send(cs, out, m, 0); close(cs); _exit(0);
    }
    close(ls);
    int cs = socket(AF_INET, SOCK_STREAM, 0); connect(cs, (struct sockaddr *)&a, sizeof a);
    struct sockaddr_in pn; socklen_t pl = sizeof pn; getpeername(cs, (struct sockaddr *)&pn, &pl);
    int peer_ok = (ntohs(pn.sin_port) == port) && (pn.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    char buf[32] = {0}; recv(cs, buf, 31, 0);
    close(cs); waitpid(pid, 0, 0);
    printf("getpeername peer_ok=%d %s\n", peer_ok, buf); // 1 srvport=1
    return 0;
}
