// snprintf return value + truncation semantics (would-be length, n=0). Portable verdicts.
#include <stdio.h>
#include <string.h>

int main(void) {
    char b[8];
    int r1 = snprintf(b, sizeof b, "%d", 12345);
    int ok1 = r1 == 5 && strcmp(b, "12345") == 0;
    int r2 = snprintf(b, 4, "%d", 12345); // truncated to "123", returns 5
    int ok2 = r2 == 5 && strcmp(b, "123") == 0;
    int r3 = snprintf(NULL, 0, "%s-%d", "ab", 7); // measure only
    int ok3 = r3 == 4;
    char c[16];
    int r4 = snprintf(c, sizeof c, "%5.2f", 3.14159);
    int ok4 = r4 == 5 && strcmp(c, " 3.14") == 0;
    printf("snprintf r1=%d r2=%d r3=%d r4=%d\n", ok1, ok2, ok3, ok4);
    return 0;
}
