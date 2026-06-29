// sin/cos/tan/asin/acos/atan/atan2/hypot at fixed inputs. Oracle vs native Linux.
#include <stdio.h>
#include <math.h>

int main(void) {
    double pi = acos(-1.0);
    printf("sin=%.10f\n", sin(pi / 6));
    printf("cos=%.10f\n", cos(pi / 3));
    printf("tan=%.10f\n", tan(pi / 4));
    printf("asin=%.10f\n", asin(0.5));
    printf("acos=%.10f\n", acos(0.5));
    printf("atan=%.10f\n", atan(1.0));
    printf("atan2=%.10f\n", atan2(1.0, 1.0));
    printf("hypot=%.10f\n", hypot(3.0, 4.0));
    return 0;
}
