// prctl(PR_SET_NAME / PR_GET_NAME): set the thread/process name and read it back. Linux-specific
// (used by runtimes, profilers, and `ps`). Diffed against a native Linux oracle.
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>

int main(void) {
    int rc = prctl(PR_SET_NAME, "dd-guestproc", 0, 0, 0);
    char name[32] = {0};
    prctl(PR_GET_NAME, name, 0, 0, 0);
    // PR_SET_NAME truncates to 15 chars + NUL
    printf("prctl set=%d name=%.15s\n", rc == 0, name); // 1 dd-guestproc
    return 0;
}
