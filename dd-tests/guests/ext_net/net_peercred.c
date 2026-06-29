// SO_PEERCRED on an AF_UNIX socketpair returns the peer's credentials; here both ends are this
// process, so uid==getuid() and pid>0. (SO_PEERCRED is Linux-only.) Diffed vs native oracle.
#define _GNU_SOURCE
#include <sys/socket.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    struct ucred cr; socklen_t l = sizeof cr;
    int r = getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &cr, &l);
    close(sv[0]); close(sv[1]);
    printf("peercred r=%d uid_ok=%d pid_ok=%d\n", r, cr.uid == getuid(), cr.pid > 0); // 0 1 1
    return 0;
}
