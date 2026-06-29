// SOAK: setjmp/longjmp control-flow endurance. 4,000,000 times: arm a jmp_buf with setjmp, do work,
// then longjmp back into it with a nonzero value that the second return folds into a checksum. Each
// longjmp forces a non-local transfer that restores the saved register/stack state -- exercising the
// JIT's setjmp/longjmp support (saved-context capture and restore, callee-saved register reload, stack
// unwind to the jmp_buf) millions of times. A restore that drops a saved register or mis-rolls the stack
// only diverges the running checksum after many transfers. Deterministic checksum -> golden, every engine.
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>

#define N 4000000

int main(void) {
    uint64_t sum = 0, work = 1;
    for (long i = 0; i < N; i++) {
        jmp_buf b;
        int v = setjmp(b);
        if (v == 0) {
            work = work * 6364136223846793005ULL + (uint64_t)i; // callee-saved live across the jump
            longjmp(b, (int)((i % 250) + 1));                   // non-local return with value 1..250
        }
        sum += (uint64_t)v + (work & 0xff); // observe both the longjmp value and the preserved work
    }
    printf("soak longjmp sum=%llu work=%llu\n", (unsigned long long)sum, (unsigned long long)work);
    return 0;
}
