// Darwin arm64 variadic ABI corner: on Apple arm64 *all* variadic args are passed on the stack
// (unlike AAPCS register passing) — va_arg must walk the stack. Exercises that. darwin engine only.
#include <stdio.h>
#include <stdarg.h>

static long vsum(int n, ...) {
    va_list ap; va_start(ap, n);
    long s = 0;
    for (int i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}

int main(void) {
    printf("varargs=%ld\n", vsum(5, 1, 2, 3, 4, 5)); // 15
    return 0;
}
