// AF_UNIX SOCK_DGRAM socketpair preserves message boundaries: three writes of lengths 2/4/2 are
// received as three distinct datagrams (no coalescing). Verifies dgram framing. Portable, golden.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int sv[2]; socketpair(AF_UNIX, SOCK_DGRAM, 0, sv);
    const char *msgs[3] = {"aa", "bbbb", "cc"};
    for (int i = 0; i < 3; i++) write(sv[0], msgs[i], strlen(msgs[i]));
    long lens = 0; char buf[64];
    for (int i = 0; i < 3; i++) { ssize_t n = read(sv[1], buf, sizeof buf); lens = lens * 10 + n; }
    close(sv[0]); close(sv[1]);
    printf("sockpair_dgram lens=%ld\n", lens); // 242
    return 0;
}
