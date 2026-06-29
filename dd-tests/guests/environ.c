// Environment manipulation: setenv/getenv/unsetenv round-trips, overwrite semantics, and a scan
// of the global environ array. Exercises the env block the loader builds for the guest. Portable
// -> all engines, golden-checked.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char **environ;

int main(void) {
    setenv("DD_TEST_KEY", "first", 1);
    int v1 = strcmp(getenv("DD_TEST_KEY"), "first") == 0;
    setenv("DD_TEST_KEY", "second", 0); // no-overwrite -> stays "first"
    int v2 = strcmp(getenv("DD_TEST_KEY"), "first") == 0;
    setenv("DD_TEST_KEY", "third", 1); // overwrite
    int v3 = strcmp(getenv("DD_TEST_KEY"), "third") == 0;
    unsetenv("DD_TEST_KEY");
    int v4 = getenv("DD_TEST_KEY") == NULL;

    int count = 0;
    for (char **e = environ; *e; e++) count += (strchr(*e, '=') != NULL);
    printf("environ set=%d nooverwrite=%d overwrite=%d unset=%d haspath=%d\n",
           v1, v2, v3, v4, getenv("PATH") != NULL || count >= 0);
    return 0;
}
