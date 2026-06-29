// select() across two ready socketpairs: both read ends become readable, select returns 2 and both
// FD_ISSET. Verifies the fd_set readiness path with a finite timeout. Portable -> all, golden.
#include <sys/select.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int a[2], b[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, a);
    socketpair(AF_UNIX, SOCK_STREAM, 0, b);
    write(a[0], "x", 1); write(b[0], "y", 1);
    fd_set rf; FD_ZERO(&rf); FD_SET(a[1], &rf); FD_SET(b[1], &rf);
    int mx = (a[1] > b[1] ? a[1] : b[1]) + 1;
    struct timeval tv = {1, 0};
    int r = select(mx, &rf, 0, 0, &tv);
    int both = FD_ISSET(a[1], &rf) && FD_ISSET(b[1], &rf);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
    printf("select_multi ready=%d both=%d\n", r, both); // 2 1
    return 0;
}
