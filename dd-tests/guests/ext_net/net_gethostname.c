// gethostname returns a non-empty name (the value itself varies by host, so the check is verdict-only).
// Confirms the syscall is wired and writes a NUL-terminated string. Portable -> all engines, golden.
#include <unistd.h>
#include <stdio.h>
#include <string.h>
int main(void) {
    char h[256] = {0};
    int r = gethostname(h, sizeof h);
    printf("gethostname r=%d nonempty=%d\n", r, strlen(h) > 0); // 0 1
    return 0;
}
