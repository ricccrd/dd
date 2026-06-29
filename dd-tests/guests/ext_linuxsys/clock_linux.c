// Linux-specific clock ids: MONOTONIC_RAW and BOOTTIME are readable and monotonic-ish.
#define _GNU_SOURCE
#include <stdio.h>
#include <time.h>

static long long ns(struct timespec t) { return (long long)t.tv_sec * 1000000000LL + t.tv_nsec; }

int main(void) {
    struct timespec raw1, raw2, boot, coarse;
    int r1 = clock_gettime(CLOCK_MONOTONIC_RAW, &raw1) == 0;
    for (volatile long i = 0; i < 1000000; i++) {}
    clock_gettime(CLOCK_MONOTONIC_RAW, &raw2);
    int raw_mono = ns(raw2) >= ns(raw1);
    int b = clock_gettime(CLOCK_BOOTTIME, &boot) == 0;
    int c = clock_gettime(CLOCK_MONOTONIC_COARSE, &coarse) == 0;
    int boot_pos = ns(boot) > 0;
    printf("clock_linux raw=%d raw_mono=%d boottime=%d coarse=%d boot_pos=%d\n", r1, raw_mono, b, c, boot_pos);
    return 0;
}
