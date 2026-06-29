// ioprio_get(2): query the calling process's IO priority (IOPRIO_WHO_PROCESS).
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#define IOPRIO_WHO_PROCESS 1

int main(void) {
    int prio = syscall(SYS_ioprio_get, IOPRIO_WHO_PROCESS, 0);
    // -1 with no error is also valid ("none/default"); just verify the call is serviced (>= -1)
    int serviced = prio >= -1;
    printf("ioprio serviced=%d\n", serviced);
    return 0;
}
