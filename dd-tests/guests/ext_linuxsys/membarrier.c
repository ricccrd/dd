// membarrier(2) MEMBARRIER_CMD_QUERY returns a non-negative bitmask of supported commands.
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#define MEMBARRIER_CMD_QUERY 0
#define MEMBARRIER_CMD_GLOBAL 1

int main(void) {
    long mask = syscall(SYS_membarrier, MEMBARRIER_CMD_QUERY, 0, 0);
    int queried = mask >= 0;
    int has_global = mask > 0 && (mask & MEMBARRIER_CMD_GLOBAL);
    // issue a global barrier if supported
    int issued = has_global ? syscall(SYS_membarrier, MEMBARRIER_CMD_GLOBAL, 0, 0) == 0 : 1;
    printf("membarrier queried=%d has_global=%d issued=%d\n", queried, has_global, issued);
    return 0;
}
