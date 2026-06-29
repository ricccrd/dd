// difftime signed second deltas. Portable verdicts.
#include <stdio.h>
#include <time.h>

int main(void) {
    time_t a = 1700000000, b = 1700003600;
    int d1 = difftime(b, a) == 3600.0;
    int d2 = difftime(a, b) == -3600.0;
    int d3 = difftime(a, a) == 0.0;
    printf("time_diff d1=%d d2=%d d3=%d\n", d1, d2, d3);
    return 0;
}
