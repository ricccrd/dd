// TCP half-close: client sends 5x "hello" then shutdown(SHUT_WR); the server reads until EOF (recv
// returns 0), reports the byte count + eof flag back, client reads it. Portable -> all, golden.
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
    pid_t pid = fork();
    if (pid == 0) {
        int cs = accept(ls, 0, 0);
        char buf[128]; long tot = 0; ssize_t n;
        while ((n = recv(cs, buf, sizeof buf, 0)) > 0) tot += n;
        int eof = (n == 0);
        char out[32]; int m = snprintf(out, 32, "got=%ld eof=%d", tot, eof);
        send(cs, out, m, 0); close(cs); _exit(0);
    }
    close(ls);
    int cs = socket(AF_INET, SOCK_STREAM, 0); connect(cs, (struct sockaddr *)&a, sizeof a);
    for (int i = 0; i < 5; i++) send(cs, "hello", 5, 0);
    shutdown(cs, SHUT_WR);
    char buf[64] = {0}; ssize_t n = recv(cs, buf, 63, 0); buf[n > 0 ? n : 0] = 0;
    close(cs); waitpid(pid, 0, 0);
    printf("tcp_shutdown %s\n", buf); // got=25 eof=1
    return 0;
}
