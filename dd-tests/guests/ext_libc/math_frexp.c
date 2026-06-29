// frexp/ldexp/modf/scalbn/scalbln/ilogb/logb (exact values). Portable verdicts.
#include <stdio.h>
#include <math.h>

int main(void) {
    int e; double m = frexp(8.0, &e); int d1 = m == 0.5 && e == 4;
    int d2 = ldexp(0.75, 3) == 6.0;
    double ip; double fp = modf(3.75, &ip); int d3 = ip == 3.0 && fp == 0.75;
    int d4 = scalbn(1.5, 4) == 24.0;
    int d5 = scalbln(2.0, 3L) == 16.0;
    int d6 = ilogb(8.0) == 3;
    int d7 = logb(16.0) == 4.0;
    printf("math_frexp d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d\n",
           d1, d2, d3, d4, d5, d6, d7);
    return 0;
}
