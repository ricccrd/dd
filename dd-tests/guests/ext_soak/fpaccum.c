// SOAK: long-running floating-point accumulation determinism. Two dependent FP recurrences run for
// ~30M iterations: `y = sqrt(y*y + 1)` (a never-folding, slowly growing chain that rounds every step)
// and `x = (x*0.5 + 1)/3` (mul/add/div converging to a fixed point through a rounded transient). The
// chains are data-dependent so they can't be vectorized or reassociated; the final IEEE-754 bit patterns
// are printed (not a decimal that could format-differ), so any drift in the JIT's FP lowering over a long
// run is caught exactly. FP contraction is forced OFF so add+mul never fuses to an FMA on one arch only,
// keeping the result byte-identical across x86-64 / aarch64 / darwin. Golden, runs on every engine.
#pragma STDC FP_CONTRACT OFF
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

int main(void) {
    double x = 7.0, y = 0.0;
    for (uint64_t i = 0; i < 30000000ULL; i++) {
        y = sqrt(y * y + 1.0);     // grows ~ sqrt(i): keeps changing, rounds every step
        x = (x * 0.5 + 1.0) / 3.0; // mul + add + div, converges through a rounded transient
    }
    uint64_t xb, yb;
    memcpy(&xb, &x, 8);
    memcpy(&yb, &y, 8);
    printf("soak fpaccum xb=%llu yb=%llu\n", (unsigned long long)xb, (unsigned long long)yb);
    return 0;
}
