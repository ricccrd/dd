// execve: re-exec our own binary with a sentinel argument; the second image prints the verdict.
// Exercises the loader's exec-replace-image path (no fork) for a static-PIE ELF / Mach-O.
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "child") == 0) {
        printf("execself child ok=1\n");
        return 0;
    }
    char *nargv[] = {argv[0], (char *)"child", NULL};
    execve(argv[0], nargv, environ);
    // only reached on failure
    printf("execself failed=1\n");
    return 1;
}
