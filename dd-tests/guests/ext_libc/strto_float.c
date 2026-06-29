// strtod/strtof/strtold: sci notation, inf/nan, hex float, signed zero. Portable verdicts.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(void) {
    char *end;
    double d = strtod("3.14159e2 xx", &end);
    int d1 = fabs(d - 314.159) < 1e-9 && *end == ' ';
    int d2 = strtof("2.5", NULL) == 2.5f;
    int d3 = strtold("1.5", NULL) == 1.5L;
    double inf = strtod("INFINITY", NULL); int d4 = isinf(inf) && inf > 0;
    double nan = strtod("NAN", NULL); int d5 = isnan(nan);
    int d6 = strtod("0x1p4", NULL) == 16.0;
    double neg = strtod("-0.0", NULL); int d7 = neg == 0.0 && signbit(neg);
    printf("strto_float d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d\n",
           d1, d2, d3, d4, d5, d6, d7);
    return 0;
}
