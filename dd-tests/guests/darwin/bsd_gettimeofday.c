// macOS-native gettimeofday monotonicity (commpage-backed on darwin — read without a real syscall).
// Exercises the darwin commpage time path. darwin engine only, golden-checked.
#include <stdio.h>
#include <sys/time.h>

int main(void) {
    struct timeval a, b;
    gettimeofday(&a, NULL);
    gettimeofday(&b, NULL);
    long ta = (long)a.tv_sec * 1000000L + a.tv_usec;
    long tb = (long)b.tv_sec * 1000000L + b.tv_usec;
    printf("gettimeofday mono=%d\n", tb >= ta); // 1
    return 0;
}
