// Memory bandwidth: large memcpy back and forth. On x86 this exercises `rep movs`, which dd can
// lower to a host memcpy — a case where the translator can beat a naive native loop.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    const int N = 8 * 1024 * 1024;
    unsigned char *a = malloc(N), *b = malloc(N);
    for (int i = 0; i < N; i++) a[i] = (unsigned char)(i * 131 + 7);
    unsigned long sum = 0;
    for (int rep = 0; rep < 200; rep++) {
        memcpy(b, a, N);
        b[rep % N] ^= (unsigned char)rep;       // defeat dead-store elimination
        memcpy(a, b, N);
        sum += a[(unsigned)(rep * 1315423911u) % N];
    }
    printf("%lu\n", sum);
    free(a); free(b);
    return 0;
}
