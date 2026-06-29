// connect() to a loopback port with no listener returns ECONNREFUSED (immediate RST), not a hang.
// The port is obtained by binding then closing a probe socket. Portable -> all engines, golden.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(probe, (struct sockaddr *)&a, sizeof a);
    socklen_t al = sizeof a; getsockname(probe, (struct sockaddr *)&a, &al);
    close(probe);
    int cs = socket(AF_INET, SOCK_STREAM, 0);
    int r = connect(cs, (struct sockaddr *)&a, sizeof a);
    int refused = (r < 0 && errno == ECONNREFUSED);
    close(cs);
    printf("connect_refused refused=%d\n", refused); // 1
    return 0;
}
