// TCP loopback echo over 127.0.0.1: a forked server binds an ephemeral port, the
// parent connects and round-trips a payload. Exercises socket/bind/listen/accept/
// connect/getsockname/send/recv across a fork. Deterministic -> oracle-checked.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) { perror("socket"); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0; // ephemeral
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0) { perror("bind"); return 1; }
    if (listen(ls, 4) < 0) { perror("listen"); return 1; }
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);
    int port = ntohs(a.sin_port);

    pid_t pid = fork();
    if (pid == 0) { // server
        int cs = accept(ls, NULL, NULL);
        char buf[64];
        ssize_t n = recv(cs, buf, sizeof buf, 0);
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32; // upper-case it
        send(cs, buf, n, 0);
        close(cs);
        _exit(0);
    }
    close(ls);
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in s = {0};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    s.sin_port = htons(port);
    if (connect(cs, (struct sockaddr *)&s, sizeof s) < 0) { perror("connect"); return 1; }
    const char *msg = "hello-socket";
    send(cs, msg, strlen(msg), 0);
    char buf[64] = {0};
    ssize_t n = recv(cs, buf, sizeof buf - 1, 0);
    buf[n > 0 ? n : 0] = 0;
    int st = 0;
    waitpid(pid, &st, 0);
    printf("tcp echo=%s exit=%d\n", buf, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return 0;
}
