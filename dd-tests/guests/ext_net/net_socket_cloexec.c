// fcntl flags on a socket: set FD_CLOEXEC (was clear) and O_NONBLOCK, read both back. Verifies
// F_GETFD/F_SETFD and F_GETFL/F_SETFL on a socket fd. Portable -> all engines, golden.
#include <sys/socket.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int before = (fcntl(s, F_GETFD) & FD_CLOEXEC) != 0;
    fcntl(s, F_SETFD, FD_CLOEXEC);
    int after = (fcntl(s, F_GETFD) & FD_CLOEXEC) != 0;
    int fl = fcntl(s, F_GETFL); fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int nb = (fcntl(s, F_GETFL) & O_NONBLOCK) != 0;
    close(s);
    printf("sock_cloexec before=%d after=%d nonblock=%d\n", before, after, nb); // 0 1 1
    return 0;
}
