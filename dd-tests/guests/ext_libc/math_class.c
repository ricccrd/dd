// Classification: isnan/isinf/isfinite/isnormal/fpclassify/signbit on special values. Portable verdicts.
#include <stdio.h>
#include <math.h>

int main(void) {
    double inf = INFINITY, nan = NAN, zero = 0.0, norm = 1.5;
    int d1 = isnan(nan) && !isnan(norm);
    int d2 = isinf(inf) && !isinf(norm);
    int d3 = isfinite(norm) && !isfinite(inf) && !isfinite(nan);
    int d4 = isnormal(norm) && !isnormal(zero) && !isnormal(inf);
    int d5 = fpclassify(inf) == FP_INFINITE && fpclassify(nan) == FP_NAN;
    int d6 = fpclassify(zero) == FP_ZERO && fpclassify(norm) == FP_NORMAL;
    int d7 = signbit(-1.0) && !signbit(1.0);
    printf("math_class d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d\n",
           d1, d2, d3, d4, d5, d6, d7);
    return 0;
}
