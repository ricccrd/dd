// sendmsg/recvmsg over UDP with a multi-iovec payload and msg_name addressing: the server fills
// msg_name with the client's loopback address and reports the total length back. Portable, golden.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    int srv = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(srv, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a; getsockname(srv, (struct sockaddr *)&a, &al);
    pid_t pid = fork();
    if (pid == 0) {
        char b1[4], b2[8]; struct iovec io[2] = {{b1, 3}, {b2, 5}};
        struct sockaddr_in from; struct msghdr mh = {0};
        mh.msg_iov = io; mh.msg_iovlen = 2; mh.msg_name = &from; mh.msg_namelen = sizeof from;
        ssize_t n = recvmsg(srv, &mh, 0);
        int from_lo = (from.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
        char out[32]; int m = snprintf(out, 32, "n=%ld lo=%d", (long)n, from_lo);
        sendto(srv, out, m, 0, (struct sockaddr *)&from, mh.msg_namelen);
        _exit(0);
    }
    close(srv);
    int cl = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in ca = {0}; ca.sin_family = AF_INET; ca.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(cl, (struct sockaddr *)&ca, sizeof ca);
    struct iovec sio[2] = {{"ABC", 3}, {"DEFGH", 5}};
    struct msghdr smh = {0}; smh.msg_iov = sio; smh.msg_iovlen = 2; smh.msg_name = &a; smh.msg_namelen = sizeof a;
    sendmsg(cl, &smh, 0);
    char buf[32] = {0}; recvfrom(cl, buf, 31, 0, 0, 0);
    waitpid(pid, 0, 0);
    printf("sendmsg_addr %s\n", buf); // n=8 lo=1
    return 0;
}
