// Anonymous pipe across a fork: child writes ints 1..2000, parent sums them. Verifies pipe() + the
// blocking byte stream survives a fork with the right ends closed. Portable -> all engines, golden.
#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
int main(void) {
    int p[2]; pipe(p);
    pid_t pid = fork();
    if (pid == 0) { close(p[0]); for (int i = 1; i <= 2000; i++) write(p[1], &i, sizeof i); close(p[1]); _exit(0); }
    close(p[1]);
    long sum = 0; int v;
    while (read(p[0], &v, sizeof v) == sizeof v) sum += v;
    close(p[0]); waitpid(pid, 0, 0);
    printf("pipe sum=%ld\n", sum); // 1..2000 = 2001000
    return 0;
}
