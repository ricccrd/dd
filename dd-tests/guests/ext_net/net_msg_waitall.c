// recv(MSG_WAITALL) blocks until the full buffer is filled even when the forked sender dribbles one
// byte at a time. Verifies the wait-for-all-bytes contract. Portable -> all engines, golden.
#include <stdio.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    pid_t pid = fork();
    if (pid == 0) { close(sv[0]); for (int i = 0; i < 10; i++) { char c = 'A' + i; struct timespec ts = {0, 5000000}; nanosleep(&ts, 0); write(sv[1], &c, 1); } _exit(0); }
    close(sv[1]);
    char buf[10] = {0}; ssize_t n = recv(sv[0], buf, 10, MSG_WAITALL);
    close(sv[0]); waitpid(pid, 0, 0);
    printf("msg_waitall n=%ld data=%.10s\n", (long)n, buf); // 10 ABCDEFGHIJ
    return 0;
}
