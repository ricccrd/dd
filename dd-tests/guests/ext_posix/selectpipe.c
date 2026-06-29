// select(2) + pselect(2): readiness on a pipe read end with timeouts.
#include <stdio.h>
#include <sys/select.h>
#include <unistd.h>

int main(void) {
    int fds[2];
    pipe(fds);
    fd_set rf;
    FD_ZERO(&rf); FD_SET(fds[0], &rf);
    struct timeval tv = {0, 50000};
    int timed_out = select(fds[0] + 1, &rf, NULL, NULL, &tv) == 0;
    write(fds[1], "y", 1);
    FD_ZERO(&rf); FD_SET(fds[0], &rf);
    tv.tv_sec = 0; tv.tv_usec = 50000;
    int ready = select(fds[0] + 1, &rf, NULL, NULL, &tv) == 1 && FD_ISSET(fds[0], &rf);
    // pselect with the same readiness
    char c; read(fds[0], &c, 1);
    write(fds[1], "z", 1);
    FD_ZERO(&rf); FD_SET(fds[0], &rf);
    struct timespec ts = {0, 50000000};
    int pready = pselect(fds[0] + 1, &rf, NULL, NULL, &ts, NULL) == 1;
    close(fds[0]); close(fds[1]);
    printf("selectpipe timeout=%d ready=%d pselect=%d\n", timed_out, ready, pready);
    return 0;
}
