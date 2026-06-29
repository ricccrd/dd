// macOS-native arc4random: arc4random_uniform(1) is always 0 (upper bound exclusive), so 100 draws
// sum to 0 — deterministic. Exercises the BSD CSPRNG entry point. darwin engine only.
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    int z = 0;
    for (int i = 0; i < 100; i++) z += arc4random_uniform(1);
    printf("arc4random ok=%d\n", z == 0); // 1
    return 0;
}
