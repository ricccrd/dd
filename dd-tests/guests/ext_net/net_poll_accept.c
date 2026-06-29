// Non-blocking listener + poll(POLLIN) before accept: the forked client connects after a short delay,
// poll reports the listener readable, accept succeeds and the request is read. The accepted fd is set
// back to blocking (macOS inherits the listener's O_NONBLOCK). Portable -> all engines, golden.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (struct sockaddr *)&a, sizeof a); listen(ls, 4);
    socklen_t al = sizeof a; getsockname(ls, (struct sockaddr *)&a, &al);
    fcntl(ls, F_SETFL, O_NONBLOCK);
    pid_t pid = fork();
    if (pid == 0) {
        struct timespec ts = {0, 30000000}; nanosleep(&ts, 0);
        int cs = socket(AF_INET, SOCK_STREAM, 0); connect(cs, (struct sockaddr *)&a, sizeof a);
        send(cs, "ping", 4, 0); char b[8]; recv(cs, b, 8, 0); close(cs); _exit(0);
    }
    struct pollfd pf = {.fd = ls, .events = POLLIN};
    int pr = poll(&pf, 1, 2000);
    int ready = (pr == 1) && (pf.revents & POLLIN);
    int cs = accept(ls, 0, 0);
    int fl = fcntl(cs, F_GETFL); fcntl(cs, F_SETFL, fl & ~O_NONBLOCK); // normalize to blocking
    char buf[8] = {0}; recv(cs, buf, 8, 0); send(cs, "pong", 4, 0);
    close(cs); close(ls); waitpid(pid, 0, 0);
    printf("poll_accept ready=%d got=%s\n", ready, buf); // 1 ping
    return 0;
}
