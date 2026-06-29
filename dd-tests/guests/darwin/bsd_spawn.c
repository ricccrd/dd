// macOS-native posix_spawn: spawn /usr/bin/true and /usr/bin/false, waitpid each, check exit codes.
// posix_spawn is the *primary* process-creation path on darwin (not fork/exec). darwin engine only.
#include <stdio.h>
#include <spawn.h>
#include <sys/wait.h>

extern char **environ;

static int run(const char *path) {
    pid_t pid;
    char *av[] = { (char *)path, NULL };
    if (posix_spawn(&pid, path, NULL, NULL, av, environ) != 0) return -1;
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(void) {
    int t = run("/usr/bin/true");
    int f = run("/usr/bin/false");
    printf("spawn true=%d false=%d\n", t, f); // 0 1
    return 0;
}
