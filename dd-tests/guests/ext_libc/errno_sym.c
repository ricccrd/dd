// errno semantics via SYMBOLIC codes (numeric values differ across libc, so compare symbols).
// Portable verdicts.
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <math.h>

int main(void) {
    errno = 0;
    FILE *f = fopen("/nonexistent/path/xyz", "r");
    int d1 = f == NULL && errno == ENOENT;
    errno = 0; double s = sqrt(-1.0);
    int d2 = errno == EDOM || isnan(s); // EDOM optional; NaN result guaranteed
    errno = 0; strtol("999999999999999999999", NULL, 10);
    int d3 = errno == ERANGE;
    errno = EINVAL; int d4 = errno == EINVAL; // errno is a settable lvalue
    errno = 0; int d5 = errno == 0;
    printf("errno_sym d1=%d d2=%d d3=%d d4=%d d5=%d\n", d1, d2, d3, d4, d5);
    return 0;
}
