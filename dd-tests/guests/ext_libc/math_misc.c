// hypot/copysign/signbit/nextafter/fmax/fmin/fma/fabs. Portable verdicts.
#include <stdio.h>
#include <math.h>

int main(void) {
    int d1 = hypot(3.0, 4.0) == 5.0;
    int d2 = copysign(3.0, -1.0) == -3.0 && copysign(-3.0, 1.0) == 3.0;
    int d3 = signbit(-0.0) != 0 && signbit(1.0) == 0;
    int d4 = nextafter(1.0, 2.0) > 1.0 && nextafter(1.0, 0.0) < 1.0;
    int d5 = fmax(2.0, 3.0) == 3.0 && fmin(2.0, 3.0) == 2.0;
    int d6 = fma(2.0, 3.0, 4.0) == 10.0;
    int d7 = fabs(-7.0) == 7.0;
    printf("math_misc d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d\n",
           d1, d2, d3, d4, d5, d6, d7);
    return 0;
}
