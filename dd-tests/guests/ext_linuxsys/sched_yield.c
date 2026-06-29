// sched_yield(2): returns 0; sched_getscheduler reports a valid policy for the calling thread.
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>

int main(void) {
    int y = sched_yield() == 0;
    int pol = sched_getscheduler(0);
    int valid = pol == SCHED_OTHER || pol == SCHED_BATCH || pol == SCHED_IDLE || pol >= 0;
    int pmax = sched_get_priority_max(SCHED_OTHER);
    int pmin = sched_get_priority_min(SCHED_OTHER);
    int prio_ok = pmax >= 0 && pmin >= 0 && pmax >= pmin;
    printf("sched_yield yield=%d policy_valid=%d prio_ok=%d\n", y, valid, prio_ok);
    return 0;
}
