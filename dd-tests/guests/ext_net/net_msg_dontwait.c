// recv(MSG_DONTWAIT) on an empty socket returns EAGAIN/EWOULDBLOCK instead of blocking; after a
// write it returns the bytes. The per-call non-blocking flag. Portable -> all engines, golden.
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    char buf[8];
    ssize_t n = recv(sv[1], buf, 8, MSG_DONTWAIT);
    int eagain = (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
    write(sv[0], "hi", 2);
    ssize_t m = recv(sv[1], buf, 8, MSG_DONTWAIT);
    close(sv[0]); close(sv[1]);
    printf("msg_dontwait eagain=%d then=%ld\n", eagain, (long)m); // 1 2
    return 0;
}
