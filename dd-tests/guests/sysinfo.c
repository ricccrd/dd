// uname + getpid (stable substrings; uid is host-passthrough in bare mode so not checked).
#include <stdio.h>
#include <unistd.h>
#include <sys/utsname.h>
int main(void) {
    struct utsname u; uname(&u);
    printf("sys=%s pid_ok=%d\n", u.sysname, getpid() > 0);
    return 0;
}
