// strftime broad specifier coverage for a fixed UTC time. Oracle vs native Linux.
#include <stdio.h>
#include <time.h>

int main(void) {
    time_t t = 1700000000; // 2023-11-14T22:13:20Z (Tuesday)
    struct tm tmv; gmtime_r(&t, &tmv);
    char b[128];
    strftime(b, sizeof b, "%A %a %B %b %d %e %j %m %y %Y", &tmv); printf("[%s]\n", b);
    strftime(b, sizeof b, "%H %I %M %S %p %P", &tmv); printf("[%s]\n", b);
    strftime(b, sizeof b, "%w %u %U %W", &tmv); printf("[%s]\n", b);
    strftime(b, sizeof b, "%F %T %D %R", &tmv); printf("[%s]\n", b);
    return 0;
}
