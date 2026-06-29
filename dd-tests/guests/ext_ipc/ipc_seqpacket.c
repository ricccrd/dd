// AF_UNIX SOCK_SEQPACKET socketpair: reliable, ordered, message-boundary-preserving datagrams. Three
// records of length 3/6/3 arrive as three reads. (SEQPACKET is Linux-only here.) Diffed vs oracle.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) < 0) { perror("socketpair"); return 1; }
    const char *m[3] = {"abc", "defghi", "jkl"};
    for (int i = 0; i < 3; i++) write(sv[0], m[i], strlen(m[i]));
    char buf[64]; long lens = 0;
    for (int i = 0; i < 3; i++) { ssize_t n = read(sv[1], buf, sizeof buf); lens = lens * 10 + n; }
    close(sv[0]); close(sv[1]);
    printf("seqpacket lens=%ld\n", lens); // 363
    return 0;
}
