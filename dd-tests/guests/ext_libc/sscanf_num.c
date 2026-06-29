// sscanf numeric conversions: d x o u f and %i auto-base. Portable verdicts.
#include <stdio.h>

int main(void) {
    int a, b, c;
    int got = sscanf("10 20 30", "%d %d %d", &a, &b, &c);
    int d1 = got == 3 && a == 10 && b == 20 && c == 30;
    int h; sscanf("0xff", "%x", &h); int d2 = h == 255;
    int o; sscanf("17", "%o", &o); int d3 = o == 15;
    unsigned u; sscanf("4000000000", "%u", &u); int d4 = u == 4000000000u;
    double f; sscanf("3.14", "%lf", &f); int d5 = f > 3.13 && f < 3.15;
    int i1, i2; sscanf("0x10 010", "%i %i", &i1, &i2); int d6 = i1 == 16 && i2 == 8;
    printf("sscanf_num d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d\n", d1, d2, d3, d4, d5, d6);
    return 0;
}
