// abs/labs/llabs and div/ldiv/lldiv (truncation toward zero). Portable verdicts.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int d1 = abs(-5) == 5 && abs(5) == 5;
    int d2 = labs(-100000L) == 100000L;
    int d3 = llabs(-9000000000LL) == 9000000000LL;
    div_t q = div(17, 5); int d4 = q.quot == 3 && q.rem == 2;
    div_t q2 = div(-17, 5); int d5 = q2.quot == -3 && q2.rem == -2;
    ldiv_t l = ldiv(100L, 7L); int d6 = l.quot == 14 && l.rem == 2;
    lldiv_t ll = lldiv(1000LL, 3LL); int d7 = ll.quot == 333 && ll.rem == 1;
    printf("absdiv d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d\n",
           d1, d2, d3, d4, d5, d6, d7);
    return 0;
}
