// rand()/srand() determinism for a fixed seed. Oracle vs native Linux (glibc PRNG sequence).
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    srand(12345);
    for (int i = 0; i < 5; i++) printf("%d\n", rand() % 1000);
    srand(12345); int first = rand();
    srand(12345);
    printf("reproducible=%d\n", rand() == first);
    return 0;
}
