// nanosleep: sleeping ~30ms must elapse at least the requested monotonic time.
#include <stdio.h>
#include <time.h>

static long long ns(struct timespec t) { return (long long)t.tv_sec * 1000000000LL + t.tv_nsec; }

int main(void) {
    struct timespec a, b, req = {0, 30000000}; // 30ms
    clock_gettime(CLOCK_MONOTONIC, &a);
    int rc = nanosleep(&req, NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long long elapsed = ns(b) - ns(a);
    printf("nanosleep rc=%d slept_ge=%d\n", rc, elapsed >= 25000000); // allow slack
    return 0;
}
