// SOAK: deep call-graph churn. Three mutually-recursive functions form a tribonacci-style binary/ternary
// call tree; evaluating it makes tens of millions of nested CALL/RET pairs through a deep, ever-changing
// stack of frames. This hammers the JIT's call/return handling, return-address prediction, prologue/
// epilogue codegen and stack-slot management continuously -- a regime where a botched frame spill or a
// stale return target (harmless on a shallow call) eventually corrupts a result. Deterministic value
// -> golden, runs on every engine.
#include <stdint.h>
#include <stdio.h>

static uint64_t tb(int n);
static uint64_t tc(int n);
// noinline: keep every edge a real CALL/RET so the optimizer can't flatten the tree into a loop.
__attribute__((noinline)) static uint64_t ta(int n) { if (n < 2) return 1; return tb(n - 1) + tc(n - 2) + 1; }
__attribute__((noinline)) static uint64_t tb(int n) { if (n < 2) return 1; return tc(n - 1) + ta(n - 2) + 2; }
__attribute__((noinline)) static uint64_t tc(int n) { if (n < 2) return 1; return ta(n - 1) + tb(n - 2) + 3; }

int main(void) {
    volatile int n = 38;          // volatile so the tree isn't constant-folded away
    uint64_t r = ta(n) + tb(n) + tc(n);
    printf("soak callgraph val=%llu\n", (unsigned long long)r);
    return 0;
}
