// strftime numeric specifiers (locale-independent) for a fixed UTC time. Portable verdicts.
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(void) {
    time_t t = 1700000000; // 2023-11-14T22:13:20Z
    struct tm tmv; gmtime_r(&t, &tmv);
    char b[64];
    strftime(b, sizeof b, "%Y-%m-%d", &tmv);   int d1 = strcmp(b, "2023-11-14") == 0;
    strftime(b, sizeof b, "%H:%M:%S", &tmv);   int d2 = strcmp(b, "22:13:20") == 0;
    strftime(b, sizeof b, "%j", &tmv);         int d3 = strcmp(b, "318") == 0; // 1-based day of year
    strftime(b, sizeof b, "%w", &tmv);         int d4 = strcmp(b, "2") == 0;   // Tue
    strftime(b, sizeof b, "%F %T", &tmv);      int d5 = strcmp(b, "2023-11-14 22:13:20") == 0;
    printf("time_strftime_num d1=%d d2=%d d3=%d d4=%d d5=%d\n", d1, d2, d3, d4, d5);
    return 0;
}
