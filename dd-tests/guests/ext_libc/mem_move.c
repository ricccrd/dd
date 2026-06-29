// Overlapping memmove (both directions), memset patterns, unsigned memcmp — portable verdicts.
#include <stdio.h>
#include <string.h>

int main(void) {
    char a[16] = "0123456789";
    memmove(a + 2, a, 5); // forward overlap -> "0101234789"
    int fwd = strncmp(a, "0101234789", 10) == 0;
    char b[16] = "0123456789";
    memmove(b, b + 3, 5); // backward overlap -> "3456756789"
    int bwd = strncmp(b, "3456756789", 10) == 0;
    char p[8]; memset(p, 0, 8); memset(p, 'A', 3);
    int set = p[0] == 'A' && p[2] == 'A' && p[3] == 0;
    int c1 = memcmp("\x01", "\x02", 1) < 0;
    int c2 = memcmp("\xff", "\x01", 1) > 0; // unsigned: 0xff > 0x01
    printf("mem_move fwd=%d bwd=%d set=%d cmp=%d\n", fwd, bwd, set, c1 && c2);
    return 0;
}
