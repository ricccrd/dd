// Bulk TCP transfer over loopback: client streams 400 cycles of bytes 0..255 (102400 bytes); the
// server sums every byte and returns the 64-bit total (13056000). Verifies large streamed IO. Golden.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#define CYCLES 400
#define TOTAL (CYCLES * 256)
int main(void) {
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(ls, (struct sockaddr *)&a, sizeof a); listen(ls, 4);
    socklen_t al = sizeof a; getsockname(ls, (struct sockaddr *)&a, &al);
    pid_t pid = fork();
    if (pid == 0) {
        int cs = accept(ls, 0, 0);
        uint64_t sum = 0; long cnt = 0; unsigned char buf[4096]; ssize_t n;
        while (cnt < TOTAL && (n = recv(cs, buf, sizeof buf, 0)) > 0) { for (ssize_t i = 0; i < n; i++) sum += buf[i]; cnt += n; }
        write(cs, &sum, 8); close(cs); _exit(0);
    }
    close(ls);
    int cs = socket(AF_INET, SOCK_STREAM, 0); connect(cs, (struct sockaddr *)&a, sizeof a);
    unsigned char buf[256]; for (int i = 0; i < 256; i++) buf[i] = i;
    for (int c = 0; c < CYCLES; c++) { long off = 0; while (off < 256) { ssize_t w = send(cs, buf + off, 256 - off, 0); if (w <= 0) break; off += w; } }
    uint64_t sum = 0; recv(cs, &sum, 8, MSG_WAITALL);
    close(cs); waitpid(pid, 0, 0);
    printf("tcp_bulk sum=%lu\n", (unsigned long)sum); // 13056000
    return 0;
}
