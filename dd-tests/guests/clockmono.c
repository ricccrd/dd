// Time syscalls: CLOCK_MONOTONIC must be non-decreasing across a nanosleep, and the slept
// interval must be at least the requested ~30ms. Output is a deterministic verdict so it
// can be golden-checked (wall-clock values themselves are not reproducible).
#include <stdio.h>
#include <time.h>

int main(void) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    struct timespec req = {.tv_sec = 0, .tv_nsec = 30 * 1000 * 1000};
    nanosleep(&req, NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long long d = (long long)(t1.tv_sec - t0.tv_sec) * 1000000000LL + (t1.tv_nsec - t0.tv_nsec);
    // also confirm REALTIME is readable and non-zero
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    printf("clock mono_ok=%d slept_ge=%d realtime_ok=%d\n",
           d > 0, d >= 25 * 1000 * 1000, rt.tv_sec > 1000000000);
    return 0;
}
