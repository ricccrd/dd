// Floating-point parsing/formatting: round-trip doubles through strtod and snprintf, plus a few
// tricky inputs (scientific notation, hex float, inf). Exercises the libc number conversion paths
// (which lean on long-double + locale internals). Portable -> all engines, golden-checked.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    double a = strtod("3.141592653589793", NULL);
    double b = strtod("1.5e-3", NULL);
    double c = strtod("0x1.8p3", NULL); // 12.0
    double inf = strtod("inf", NULL);

    char buf[64];
    snprintf(buf, sizeof buf, "%.6f", a);
    int pi_ok = strcmp(buf, "3.141593") == 0;
    snprintf(buf, sizeof buf, "%.4e", b);
    int sci_ok = strcmp(buf, "1.5000e-03") == 0;
    int hex_ok = c == 12.0;
    int inf_ok = isinf(inf) && inf > 0;

    long acc = 0;
    for (int i = 0; i < 1000; i++) acc += (long)(strtod("2.5", NULL) * 4); // 10 each
    printf("strtod pi=%d sci=%d hex=%d inf=%d acc=%ld\n", pi_ok, sci_ok, hex_ok, inf_ok, acc);
    return 0;
}
