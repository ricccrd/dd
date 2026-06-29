// sendmsg/recvmsg with a multi-segment iovec over a UNIX socketpair. Exercises the msghdr +
// scatter/gather socket path (used by many RPC libraries). Deterministic -> oracle.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

int main(void) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);

    struct iovec sio[2] = {{"AB", 2}, {"CDEF", 4}};
    struct msghdr smh = {0};
    smh.msg_iov = sio;
    smh.msg_iovlen = 2;
    ssize_t sent = sendmsg(sv[0], &smh, 0);

    char b1[3] = {0}, b2[8] = {0};
    struct iovec rio[2] = {{b1, 3}, {b2, 3}};
    struct msghdr rmh = {0};
    rmh.msg_iov = rio;
    rmh.msg_iovlen = 2;
    ssize_t got = recvmsg(sv[1], &rmh, 0);
    close(sv[0]);
    close(sv[1]);
    printf("sendmsg sent=%ld got=%ld data=%.3s%.3s\n", (long)sent, (long)got, b1, b2);
    return 0;
}
