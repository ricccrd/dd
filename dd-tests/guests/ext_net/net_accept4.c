// accept4 with SOCK_NONBLOCK|SOCK_CLOEXEC sets those flags on the accepted fd atomically. Verified
// via F_GETFL/F_GETFD. (accept4 is Linux-only.) Diffed against the native oracle.
#define _GNU_SOURCE
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (struct sockaddr *)&a, sizeof a); listen(ls, 4);
    socklen_t al = sizeof a; getsockname(ls, (struct sockaddr *)&a, &al);
    pid_t pid = fork();
    if (pid == 0) { int cs = socket(AF_INET, SOCK_STREAM, 0); connect(cs, (struct sockaddr *)&a, sizeof a); send(cs, "hi", 2, 0); char b[4]; recv(cs, b, 4, 0); close(cs); _exit(0); }
    int cs = accept4(ls, 0, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
    int nb = (fcntl(cs, F_GETFL) & O_NONBLOCK) != 0;
    int ce = (fcntl(cs, F_GETFD) & FD_CLOEXEC) != 0;
    char buf[4] = {0};
    for (int i = 0; i < 2000; i++) { ssize_t n = recv(cs, buf, 4, 0); if (n > 0) break; struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
    send(cs, "ok", 2, 0);
    close(cs); close(ls); waitpid(pid, 0, 0);
    printf("accept4 nonblock=%d cloexec=%d got=%s\n", nb, ce, buf); // 1 1 hi
    return 0;
}
