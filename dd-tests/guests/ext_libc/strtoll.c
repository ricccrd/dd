// strtoll / strtoull: 64-bit limits, hex, ERANGE. Portable verdicts.
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

int main(void) {
    int d1 = strtoll("-9223372036854775808", NULL, 10) == LLONG_MIN;
    int d2 = strtoll("9223372036854775807", NULL, 10) == LLONG_MAX;
    int d3 = strtoull("18446744073709551615", NULL, 10) == ULLONG_MAX;
    errno = 0;
    long long d = strtoll("99999999999999999999999999", NULL, 10);
    int d4 = d == LLONG_MAX && errno == ERANGE;
    int d5 = strtoll("7fffffffffffffff", NULL, 16) == LLONG_MAX;
    printf("strtoll d1=%d d2=%d d3=%d d4=%d d5=%d\n", d1, d2, d3, d4, d5);
    return 0;
}
