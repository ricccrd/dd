// tgamma/lgamma/erf/erfc at fixed inputs. Oracle vs native Linux.
#include <stdio.h>
#include <math.h>

int main(void) {
    printf("tgamma=%.8f\n", tgamma(5.0));
    printf("lgamma=%.8f\n", lgamma(5.0));
    printf("erf=%.10f\n", erf(1.0));
    printf("erfc=%.10f\n", erfc(1.0));
    return 0;
}
