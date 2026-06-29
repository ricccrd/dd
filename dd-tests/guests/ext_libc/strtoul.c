// strtoul: large values, hex, negative wraparound, ERANGE. Portable verdicts.
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>

int main(void) {
    int d1 = strtoul("4294967295", NULL, 10) == 4294967295UL;
    int d2 = strtoul("ffffffff", NULL, 16) == 0xffffffffUL;
    errno = 0;
    unsigned long c = strtoul("999999999999999999999999", NULL, 10);
    int d3 = c == ULONG_MAX && errno == ERANGE;
    int d4 = strtoul("-1", NULL, 10) == ULONG_MAX; // negative wraps
    printf("strtoul d1=%d d2=%d d3=%d d4=%d\n", d1, d2, d3, d4);
    return 0;
}
