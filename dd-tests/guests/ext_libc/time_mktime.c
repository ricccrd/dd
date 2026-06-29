// mktime (TZ=UTC0) + timegm + field normalisation. Portable verdicts.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(void) {
    setenv("TZ", "UTC0", 1); tzset();
    struct tm tmv = {0};
    tmv.tm_year = 123; tmv.tm_mon = 10; tmv.tm_mday = 14;
    tmv.tm_hour = 22; tmv.tm_min = 13; tmv.tm_sec = 20; tmv.tm_isdst = 0;
    int d1 = mktime(&tmv) == 1700000000;

    struct tm g = {0};
    g.tm_year = 70; g.tm_mon = 0; g.tm_mday = 1; g.tm_isdst = 0;
    int d2 = timegm(&g) == 0; // epoch

    struct tm n = {0};
    n.tm_year = 123; n.tm_mon = 13; n.tm_mday = 1; n.tm_isdst = 0; // month 13 -> next Feb
    mktime(&n);
    int d3 = n.tm_year == 124 && n.tm_mon == 1;
    printf("time_mktime d1=%d d2=%d d3=%d\n", d1, d2, d3);
    return 0;
}
