// integer + float math; deterministic output, checked against the native oracle.
#include <stdio.h>
#include <math.h>
int main(void) {
    long s = 0; for (int i = 1; i <= 100; i++) s += i;
    double h = 0; for (int i = 1; i <= 1000; i++) h += 1.0 / i;
    printf("sum=%ld sqrt=%.0f harm=%.4f\n", s, sqrt(16.0), h);
    return 0;
}
