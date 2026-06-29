// setpgid/getpgid: a forked child puts itself in its own process group; parent observes it.
// Two pipes synchronise: child sets pgid + signals on c2p, parent checks then releases via p2c
// (avoids the ESRCH race of reaping before observing, and any single-pipe self-read).
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int c2p[2], p2c[2];
    pipe(c2p); pipe(p2c);
    pid_t c = fork();
    if (c == 0) {
        setpgid(0, 0); // own pgid = pid
        char ok = (getpgid(0) == getpid()) ? 1 : 0;
        write(c2p[1], &ok, 1);
        char go; read(p2c[0], &go, 1); // wait for parent
        _exit(0);
    }
    char child_ok = 0;
    read(c2p[0], &child_ok, 1);     // child has set its pgid
    int parent_sees = getpgid(c) == c;
    write(p2c[1], "g", 1);          // release child
    int st;
    waitpid(c, &st, 0);
    printf("setpgid child_own=%d parent_sees=%d\n", child_ok, parent_sees);
    return 0;
}
