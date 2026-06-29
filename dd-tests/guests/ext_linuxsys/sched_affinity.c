// sched_getaffinity / sched_setaffinity: read the cpu mask (>=1 cpu), pin to cpu 0, read it back.
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>

int main(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    int got = sched_getaffinity(0, sizeof set, &set) == 0;
    int ncpu = CPU_COUNT(&set);
    int has_cpus = ncpu >= 1;
    // pin to cpu 0
    cpu_set_t one;
    CPU_ZERO(&one);
    CPU_SET(0, &one);
    int pinned = sched_setaffinity(0, sizeof one, &one) == 0;
    cpu_set_t check;
    CPU_ZERO(&check);
    sched_getaffinity(0, sizeof check, &check);
    int only0 = CPU_ISSET(0, &check) && CPU_COUNT(&check) == 1;
    printf("sched_affinity got=%d has_cpus=%d pinned=%d only0=%d\n", got, has_cpus, pinned, only0);
    return 0;
}
