// AF_UNIX stream socket bound to a filesystem path: server accepts, upper-cases the request, echoes
// it; client connects, sends "unix-stream", reads "UNIX-STREAM". Portable -> all engines, golden.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
    const char *path = "/tmp/dd_unix_stream.sock";
    unlink(path);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {0}; a.sun_family = AF_UNIX; strcpy(a.sun_path, path);
    bind(ls, (struct sockaddr *)&a, sizeof a); listen(ls, 4);
    pid_t pid = fork();
    if (pid == 0) {
        int cs = accept(ls, 0, 0); char buf[64]; ssize_t n = recv(cs, buf, 64, 0);
        for (ssize_t i = 0; i < n; i++) if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;
        send(cs, buf, n, 0); close(cs); _exit(0);
    }
    close(ls);
    int cs = socket(AF_UNIX, SOCK_STREAM, 0);
    connect(cs, (struct sockaddr *)&a, sizeof a);
    send(cs, "unix-stream", 11, 0);
    char buf[64] = {0}; ssize_t n = recv(cs, buf, 63, 0); buf[n > 0 ? n : 0] = 0;
    close(cs); waitpid(pid, 0, 0); unlink(path);
    printf("unix_stream reply=%s\n", buf); // UNIX-STREAM
    return 0;
}
