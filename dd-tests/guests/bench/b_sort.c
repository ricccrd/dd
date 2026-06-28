// qsort of millions of ints — branchy, memory-bound, and an indirect call per comparison.
#include <stdio.h>
#include <stdlib.h>
static int cmp(const void *a, const void *b) {
    unsigned x = *(const unsigned*)a, y = *(const unsigned*)b;
    return (x > y) - (x < y);
}
int main(void) {
    const int N = 2 * 1000 * 1000;
    unsigned *v = malloc(sizeof(unsigned) * N);
    unsigned long sum = 0, seed;
    for (int rep = 0; rep < 6; rep++) {
        seed = 88172645463325252UL + rep;
        for (int i = 0; i < N; i++) { seed = seed * 6364136223846793005UL + 1; v[i] = (unsigned)(seed >> 33); }
        qsort(v, N, sizeof(unsigned), cmp);
        for (int i = 0; i < N; i += 997) sum += v[i];
    }
    printf("%lu\n", sum);
    free(v);
    return 0;
}
