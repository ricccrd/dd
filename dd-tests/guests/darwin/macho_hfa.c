// AArch64 AAPCS64 ABI corner: a homogeneous float aggregate (3 doubles) passed/returned by value
// uses the SIMD/FP registers v0-v2. Exercises the darwin arm64 calling convention. darwin engine only.
#include <stdio.h>

typedef struct { double x, y, z; } V3;
static double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

int main(void) {
    V3 a = {1, 2, 3}, b = {4, 5, 6};
    printf("hfa dot=%d\n", (int)dot(a, b)); // 4+10+18 = 32
    return 0;
}
