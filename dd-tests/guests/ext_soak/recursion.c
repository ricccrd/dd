// SOAK: recursion-depth stress. A non-tail recursive function descends 6000 frames deep (the add after
// the call defeats tail-call elimination, so the frames really stack), and that deep descent is repeated
// 6000 times -> ~36M genuine nested CALL/RET over a deep guest stack. This stresses the JIT's guest-stack
// growth/limit handling, frame setup, and return-address fidelity at depth -- where an off-by-one in
// stack accounting or a clobbered saved register only bites after the stack is genuinely deep.
// Deterministic sum -> golden, runs on every engine.
#include <stdint.h>
#include <stdio.h>

// noinline: prevent gcc's accumulator-recursion->loop rewrite, so the frames really stack to depth.
__attribute__((noinline)) static uint64_t rec(int n) { if (n == 0) return 0; return (uint64_t)n + rec(n - 1); }

int main(void) {
    volatile int depth = 6000;            // volatile -> not folded to the closed form
    uint64_t total = 0;
    for (int i = 0; i < 6000; i++) total += rec(depth); // each descent returns depth*(depth+1)/2
    printf("soak recursion total=%llu\n", (unsigned long long)total);
    return 0;
}
