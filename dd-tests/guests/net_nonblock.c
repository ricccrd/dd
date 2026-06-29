// Non-blocking connect: O_NONBLOCK client connect() returns EINPROGRESS, then poll(POLLOUT)
// reports writable and SO_ERROR confirms success. The async-connect pattern every event-loop
// based server/client uses. Deterministic verdict -> oracle.
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (struct sockaddr *)&a, sizeof a);
    listen(ls, 4);
    socklen_t al = sizeof a;
    getsockname(ls, (struct sockaddr *)&a, &al);

    int cs = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(cs, F_SETFL, O_NONBLOCK);
    int rc = connect(cs, (struct sockaddr *)&a, sizeof a);
    int inprogress = (rc < 0 && errno == EINPROGRESS) || rc == 0;

    struct pollfd p = {.fd = cs, .events = POLLOUT};
    int pr = poll(&p, 1, 1000);
    int writable = (pr == 1) && (p.revents & POLLOUT);

    int err = -1;
    socklen_t el = sizeof err;
    getsockopt(cs, SOL_SOCKET, SO_ERROR, &err, &el);

    int acc = accept(ls, NULL, NULL);
    close(acc);
    close(cs);
    close(ls);
    printf("nonblock inprogress=%d writable=%d soerr=%d\n", inprogress, writable, err);
    return 0;
}
