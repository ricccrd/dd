// Calendar math: convert a fixed UTC epoch with gmtime_r and format it with strftime; also parse a
// broken-down time back with timegm. Uses gmtime (UTC) so the result is timezone-independent and
// reproducible across platforms. Exercises the libc time conversion tables. Portable -> all engines.
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void) {
    time_t t = 1700000000; // 2023-11-14T22:13:20Z
    struct tm tmv;
    gmtime_r(&t, &tmv);
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    int fmt_ok = strcmp(buf, "2023-11-14 22:13:20") == 0;

    struct tm again = tmv;
    time_t back = timegm(&again);
    int roundtrip = back == t;
    int wday = tmv.tm_wday; // Tuesday = 2
    printf("timefmt fmt=%d roundtrip=%d wday=%d\n", fmt_ok, roundtrip, wday); // 1 1 2
    return 0;
}
