// macOS-native setprogname/getprogname: BSD program-name storage (used by err/warn). darwin only.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    setprogname("ddtest");
    const char *p = getprogname();
    printf("progname=%s\n", p ? p : "?"); // ddtest
    return 0;
}
