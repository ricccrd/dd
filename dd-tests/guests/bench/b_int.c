// Integer-heavy: a sieve of Eratosthenes + summation, repeated. Branches + memory + integer ALU.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    const int N = 5 * 1000 * 1000;
    unsigned char *sieve = malloc(N);
    long total = 0;
    for (int rep = 0; rep < 70; rep++) {
        memset(sieve, 1, N);
        sieve[0] = sieve[1] = 0;
        for (int i = 2; (long)i * i < N; i++)
            if (sieve[i])
                for (long j = (long)i * i; j < N; j += i) sieve[j] = 0;
        for (int i = 2; i < N; i++) if (sieve[i]) total += i;
    }
    printf("%ld\n", total);
    free(sieve);
    return 0;
}
