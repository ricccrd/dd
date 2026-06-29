// EDGE: times(2) / CPU accounting. After burning CPU in a tight loop, times() must report non-zero
// user time (tms_utime), and clock() must advance. A runtime that no-ops times() reports all zeros, so
// profilers/benchmarks/the shell `times` builtin see nothing. Verdict -> golden (Linux engines).
#include <stdio.h>
#include <sys/times.h>
#include <time.h>

int main(void) {
    clock_t c0 = clock();
    struct tms a;
    times(&a);
    volatile unsigned long x = 0;
    for (unsigned long i = 0; i < 800000000UL; i++) x += i * 2654435761UL; // burn enough for >=1 CPU tick
    struct tms b;
    clock_t t = times(&b);
    clock_t c1 = clock();
    int utime_ok = (b.tms_utime - a.tms_utime) > 0; // user CPU advanced
    int clock_ok = (c1 - c0) > 0;                    // clock() advanced
    int ret_ok = (t != (clock_t)-1);
    printf("times utime_ok=%d clock_ok=%d ret_ok=%d sink=%lu\n", utime_ok, clock_ok, ret_ok, (unsigned long)(x & 1));
    return 0;
}
