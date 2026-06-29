// Mach-O function-local static with guard variable: lazy one-time init of a function static, then
// incremented across calls. Exercises the __cxa guard / static-init path. darwin engine only.
#include <stdio.h>

static int next(void) { static int n = 0; n++; return n; }

int main(void) {
    int a = next(), b = next(), c = next();
    printf("static %d%d%d\n", a, b, c); // 123
    return 0;
}
