// exp/log/log10/log2/pow/sqrt/cbrt/expm1/log1p at fixed inputs. Oracle vs native Linux.
#include <stdio.h>
#include <math.h>

int main(void) {
    printf("exp=%.10f\n", exp(1.0));
    printf("log=%.10f\n", log(10.0));
    printf("log10=%.10f\n", log10(1000.0));
    printf("log2=%.10f\n", log2(64.0));
    printf("pow=%.10f\n", pow(2.0, 10.0));
    printf("sqrt=%.10f\n", sqrt(2.0));
    printf("cbrt=%.10f\n", cbrt(27.0));
    printf("expm1=%.12f\n", expm1(1e-5));
    printf("log1p=%.12f\n", log1p(1e-5));
    return 0;
}
