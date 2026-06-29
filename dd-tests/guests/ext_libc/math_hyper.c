// sinh/cosh/tanh/asinh/acosh/atanh at fixed inputs. Oracle vs native Linux.
#include <stdio.h>
#include <math.h>

int main(void) {
    printf("sinh=%.10f\n", sinh(1.0));
    printf("cosh=%.10f\n", cosh(1.0));
    printf("tanh=%.10f\n", tanh(1.0));
    printf("asinh=%.10f\n", asinh(1.0));
    printf("acosh=%.10f\n", acosh(2.0));
    printf("atanh=%.10f\n", atanh(0.5));
    return 0;
}
