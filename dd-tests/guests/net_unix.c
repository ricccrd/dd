// AF_UNIX socketpair full-duplex IPC across a fork. Exercises socketpair + bidirectional
// send/recv on a stream socket. Deterministic -> oracle-checked.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) { perror("socketpair"); return 1; }
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        char buf[64];
        ssize_t n = read(sv[1], buf, sizeof buf);
        long sum = 0;
        for (ssize_t i = 0; i < n; i++) sum += (unsigned char)buf[i];
        char out[32];
        int m = snprintf(out, sizeof out, "sum=%ld", sum);
        write(sv[1], out, m);
        _exit(0);
    }
    close(sv[1]);
    const char *msg = "ABCDE"; // 65+66+67+68+69 = 335
    write(sv[0], msg, strlen(msg));
    char buf[64] = {0};
    ssize_t n = read(sv[0], buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = 0;
    waitpid(pid, NULL, 0);
    printf("unix reply=%s\n", buf);
    return 0;
}
