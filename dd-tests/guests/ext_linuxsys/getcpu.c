// getcpu(2): the reported cpu index is below the online cpu count.
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void) {
    unsigned cpu = 9999, node = 9999;
    int rc = syscall(SYS_getcpu, &cpu, &node, NULL);
    long nproc = sysconf(_SC_NPROCESSORS_CONF);
    int cpu_ok = rc == 0 && cpu < (unsigned)nproc;
    printf("getcpu rc=%d cpu_in_range=%d\n", rc, cpu_ok); // 0 1
    return 0;
}
