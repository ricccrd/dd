// getrandom(2): two 32-byte draws must succeed, be non-zero, and differ from each other.
// Output is a deterministic verdict (not the bytes) so it can be golden-checked.
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

int main(void) {
    unsigned char a[32], b[32];
    ssize_t n1 = getrandom(a, sizeof a, 0);
    ssize_t n2 = getrandom(b, sizeof b, 0);
    int allzero = 1;
    for (int i = 0; i < 32; i++)
        if (a[i]) allzero = 0;
    int differ = memcmp(a, b, 32) != 0;
    printf("getrandom n=%ld,%ld nonzero=%d differ=%d\n", (long)n1, (long)n2, !allzero, differ);
    return 0;
}
