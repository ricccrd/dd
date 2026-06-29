// uname(2): on Linux sysname is "Linux" and fields are non-empty. Diffed vs native Linux.
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int main(void) {
    struct utsname u;
    int rc = uname(&u);
    int is_linux = strcmp(u.sysname, "Linux") == 0;
    int has_release = strlen(u.release) > 0;
    int has_machine = strlen(u.machine) > 0;
    printf("uname rc=%d linux=%d release=%d machine=%d\n", rc, is_linux, has_release, has_machine);
    return 0;
}
