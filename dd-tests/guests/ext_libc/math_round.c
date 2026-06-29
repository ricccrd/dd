// round/floor/ceil/trunc/nearbyint/rint/lround/llround (exact integers). Portable verdicts.
#include <stdio.h>
#include <math.h>

int main(void) {
    int d1 = round(2.5) == 3.0 && round(-2.5) == -3.0 && round(2.4) == 2.0;
    int d2 = floor(2.7) == 2.0 && floor(-2.1) == -3.0;
    int d3 = ceil(2.1) == 3.0 && ceil(-2.9) == -2.0;
    int d4 = trunc(2.9) == 2.0 && trunc(-2.9) == -2.0;
    int d5 = nearbyint(2.5) == 2.0 && nearbyint(3.5) == 4.0; // round-to-even default
    int d6 = rint(0.5) == 0.0 && rint(1.5) == 2.0;           // round-to-even default
    int d7 = lround(2.5) == 3 && lround(-2.5) == -3;
    int d8 = llround(2.5) == 3LL;
    printf("math_round d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d d8=%d\n",
           d1, d2, d3, d4, d5, d6, d7, d8);
    return 0;
}
