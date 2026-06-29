// strtol: bases, sign, endptr, base-0 auto-detect, ERANGE overflow. Portable verdicts.
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include <string.h>

int main(void) {
    char *end;
    long a = strtol("  -123abc", &end, 10);
    int d1 = a == -123 && strcmp(end, "abc") == 0;
    int d2 = strtol("0xFF", NULL, 16) == 255;
    int d3 = strtol("777", NULL, 8) == 511;
    int d4 = strtol("1010", NULL, 2) == 10;
    int d5 = strtol("0x1A", NULL, 0) == 26; // base 0 hex
    int d6 = strtol("010", NULL, 0) == 8;   // base 0 octal
    errno = 0;
    long g = strtol("99999999999999999999999", NULL, 10);
    int d7 = g == LONG_MAX && errno == ERANGE;
    int d8 = strtol("+42", NULL, 10) == 42;
    printf("strtol d1=%d d2=%d d3=%d d4=%d d5=%d d6=%d d7=%d d8=%d\n",
           d1, d2, d3, d4, d5, d6, d7, d8);
    return 0;
}
