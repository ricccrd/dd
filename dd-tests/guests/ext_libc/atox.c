// atoi/atol/atoll/atof leading-space + trailing-junk + bad-input behaviour. Portable verdicts.
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(void) {
    int d1 = atoi("42") == 42 && atoi("-17") == -17 && atoi("  9xyz") == 9;
    int d2 = atol("1000000") == 1000000L;
    int d3 = atoll("9000000000") == 9000000000LL;
    int d4 = fabs(atof("3.14") - 3.14) < 1e-9;
    int d5 = atoi("notanumber") == 0;
    printf("atox d1=%d d2=%d d3=%d d4=%d d5=%d\n", d1, d2, d3, d4, d5);
    return 0;
}
