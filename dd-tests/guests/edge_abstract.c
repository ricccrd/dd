// EDGE: Linux abstract-namespace AF_UNIX sockets (sun_path[0] == '\0' — no filesystem entry). Bind a
// server to an abstract name, connect a client, round-trip a byte. This is a Linux-only feature
// (systemd, X11, D-Bus session bus use it); macOS has no abstract namespace, so bind/connect there
// behave differently. Diffed vs native -> oracle.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    struct sockaddr_un a = {0};
    a.sun_family = AF_UNIX;
    a.sun_path[0] = '\0';                     // abstract namespace
    memcpy(a.sun_path + 1, "dd_abstract", 11);
    socklen_t alen = (socklen_t)(sizeof(a.sun_family) + 1 + 11);

    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(ls, (struct sockaddr *)&a, alen) < 0) { printf("abstract bind_failed\n"); return 0; }
    listen(ls, 1);
    pid_t pid = fork();
    if (pid == 0) {
        int cs = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(cs, (struct sockaddr *)&a, alen) == 0) write(cs, "Z", 1);
        close(cs);
        _exit(0);
    }
    int cs = accept(ls, NULL, NULL);
    char b = 0;
    ssize_t n = (cs >= 0) ? read(cs, &b, 1) : -1;
    if (cs >= 0) close(cs);
    close(ls);
    waitpid(pid, NULL, 0);
    printf("abstract got=%ld byte=%c\n", (long)n, b ? b : '?'); // 1 Z
    return 0;
}
