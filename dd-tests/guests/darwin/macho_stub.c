// Mach-O dyld stub binding + indirect calls: call a libSystem function through a function pointer
// (lazy-bound stub) and use snprintf. Exercises __stubs/__got resolution. darwin engine only.
#include <stdio.h>
#include <string.h>

int main(void) {
    size_t (*f)(const char *) = strlen;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d-%s", 42, "xyz");
    printf("stub len=%zu snp=%d buf=%s\n", f("hello"), n, buf); // 5 6 42-xyz
    return 0;
}
