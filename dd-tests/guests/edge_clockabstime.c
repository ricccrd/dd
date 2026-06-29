// EDGE: clock_nanosleep(TIMER_ABSTIME). Sleep UNTIL an absolute deadline (now + 40ms). If the runtime
// ignores TIMER_ABSTIME and treats the value as relative, it would try to sleep for the absolute
// timespec (thousands of seconds since boot) and hang. Correct behaviour returns in ~40ms. Verdict ->
// golden across engines (the 25s harness timeout catches the relative-interpretation hang).
#include <stdio.h>
#include <time.h>

int main(void) {
    struct timespec t0, t1, deadline;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    deadline = t0;
    deadline.tv_nsec += 40 * 1000 * 1000;
    if (deadline.tv_nsec >= 1000000000L) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000L; }
    int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long ms = (long long)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    int ok = (rc == 0) && ms >= 30 && ms < 2000; // woke near the absolute deadline, not way early/late
    printf("clockabstime abstime_ok=%d\n", ok); // 1
    return 0;
}
