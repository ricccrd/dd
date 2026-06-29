// clock_gettime across REALTIME/MONOTONIC/PROCESS_CPUTIME; monotonic must not go backwards.
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static long long ns(struct timespec t) { return (long long)t.tv_sec * 1000000000LL + t.tv_nsec; }

int main(void) {
    struct timespec rt, m1, m2, cpu;
    int r = clock_gettime(CLOCK_REALTIME, &rt) == 0;
    clock_gettime(CLOCK_MONOTONIC, &m1);
    // busy a touch
    volatile long x = 0;
    for (int i = 0; i < 2000000; i++) x += i;
    clock_gettime(CLOCK_MONOTONIC, &m2);
    int mono = ns(m2) >= ns(m1);
    int cp = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) == 0;
    int realpos = ns(rt) > 1000000000000LL; // well past 2001
    printf("clockid real=%d mono=%d cputime=%d realpos=%d\n", r, mono, cp, realpos);
    return 0;
}
