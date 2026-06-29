// sscanf assignment-suppression (%*d), %n byte count, delimiters, return codes. Portable verdicts.
#include <stdio.h>

int main(void) {
    int a, c; sscanf("1 2 3", "%d %*d %d", &a, &c);
    int d1 = a == 1 && c == 3;
    int x, n; sscanf("42rest", "%d%n", &x, &n); int d2 = x == 42 && n == 2;
    int y, mo, da; sscanf("2026-06-29", "%d-%d-%d", &y, &mo, &da);
    int d3 = y == 2026 && mo == 6 && da == 29;
    int got = sscanf("only", "%d", &a); int d4 = got == 0; // matching failure
    int e = sscanf("", "%d", &a); int d5 = e == EOF;       // input failure
    printf("sscanf_mix d1=%d d2=%d d3=%d d4=%d d5=%d\n", d1, d2, d3, d4, d5);
    return 0;
}
