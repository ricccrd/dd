// Scatter/gather over a socket: writev of three 3-byte iovecs sends "foobarbaz", readv reassembles
// it into three buffers. Verifies vectored socket IO. Portable -> all engines, golden.
#include <sys/uio.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct iovec wv[3] = {{"foo", 3}, {"bar", 3}, {"baz", 3}};
    ssize_t w = writev(sv[0], wv, 3);
    char a[4] = {0}, b[4] = {0}, c[4] = {0};
    struct iovec rv[3] = {{a, 3}, {b, 3}, {c, 3}};
    ssize_t r = readv(sv[1], rv, 3);
    close(sv[0]); close(sv[1]);
    printf("writev w=%ld r=%ld data=%s%s%s\n", (long)w, (long)r, a, b, c); // 9 9 foobarbaz
    return 0;
}
