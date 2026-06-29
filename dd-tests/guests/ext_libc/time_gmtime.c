// gmtime_r broken-down fields for a fixed UTC epoch (timezone-independent). Portable verdicts.
#include <stdio.h>
#include <time.h>

int main(void) {
    time_t t = 1700000000; // 2023-11-14T22:13:20Z, Tuesday
    struct tm tmv; gmtime_r(&t, &tmv);
    int d1 = tmv.tm_year == 123;          // 2023 - 1900
    int d2 = tmv.tm_mon == 10;            // November (0-based)
    int d3 = tmv.tm_mday == 14;
    int d4 = tmv.tm_hour == 22 && tmv.tm_min == 13 && tmv.tm_sec == 20;
    int d5 = tmv.tm_wday == 2;            // Tuesday
    int d6 = tmv.tm_yday == 317;          // 0-based day of year
    printf("time_gmtime d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d\n",
           d1, d2, d3, d4, d5, d6);
    return 0;
}
