// AF_UNIX datagram sockets bound to filesystem paths: client sendto's the server's path, server
// echoes back to the client's bound path. Verifies named-unix dgram addressing. Portable, golden.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    const char *sp = "/tmp/dd_unix_dg_srv.sock", *cp = "/tmp/dd_unix_dg_cli.sock";
    unlink(sp); unlink(cp);
    int srv = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un sa = {0}; sa.sun_family = AF_UNIX; strcpy(sa.sun_path, sp);
    bind(srv, (struct sockaddr *)&sa, sizeof sa);
    pid_t pid = fork();
    if (pid == 0) {
        char buf[64]; struct sockaddr_un from; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(srv, buf, 64, 0, (struct sockaddr *)&from, &fl);
        sendto(srv, buf, n, 0, (struct sockaddr *)&from, fl);
        _exit(0);
    }
    int cl = socket(AF_UNIX, SOCK_DGRAM, 0);
    struct sockaddr_un ca = {0}; ca.sun_family = AF_UNIX; strcpy(ca.sun_path, cp);
    bind(cl, (struct sockaddr *)&ca, sizeof ca);
    sendto(cl, "dgram-unix", 10, 0, (struct sockaddr *)&sa, sizeof sa);
    char buf[64] = {0}; recvfrom(cl, buf, 63, 0, 0, 0);
    waitpid(pid, 0, 0); unlink(sp); unlink(cp);
    printf("unix_dgram reply=%s\n", buf); // dgram-unix
    return 0;
}
