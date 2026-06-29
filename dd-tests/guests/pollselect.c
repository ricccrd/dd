// poll() and select() readiness on a pipe. Writes data, then confirms both multiplexers
// report the read end readable and a timeout path returns 0. Deterministic -> oracle.
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    if (pipe(fds) < 0) { perror("pipe"); return 1; }
    write(fds[1], "data", 4);

    struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
    int pr = poll(&pfd, 1, 1000);
    int p_ready = (pr == 1) && (pfd.revents & POLLIN);

    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(fds[0], &rs);
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    int sr = select(fds[0] + 1, &rs, NULL, NULL, &tv);
    int s_ready = (sr == 1) && FD_ISSET(fds[0], &rs);

    // timeout path: nothing on the write end's readability within 0ms
    struct pollfd pfd2 = {.fd = fds[1], .events = POLLIN};
    int to = poll(&pfd2, 1, 0);

    char buf[8];
    read(fds[0], buf, sizeof buf);
    printf("poll=%d select=%d timeout=%d\n", p_ready, s_ready, to == 0); // 1 1 1
    return 0;
}
