// asctime/ctime canonical 26-char format for a fixed epoch (TZ=UTC0). Portable verdicts.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int main(void) {
    setenv("TZ", "UTC0", 1); tzset();
    time_t t = 1700000000;
    struct tm tmv; gmtime_r(&t, &tmv);
    int d1 = strcmp(asctime(&tmv), "Tue Nov 14 22:13:20 2023\n") == 0;
    char buf[32]; strcpy(buf, ctime(&t)); // local == UTC under TZ=UTC0
    int d2 = strcmp(buf, "Tue Nov 14 22:13:20 2023\n") == 0;
    printf("time_asctime d1=%d d2=%d\n", d1, d2);
    return 0;
}
