// macOS-native GCD dispatch_apply: a parallel for-loop over 100 iterations on a concurrent queue;
// the atomic sum is deterministic (0+1+...+99). Exercises GCD parallel-apply. darwin engine only.
#include <stdio.h>
#include <dispatch/dispatch.h>

int main(void) {
    __block long sum = 0;
    dispatch_apply(100, dispatch_get_global_queue(0, 0), ^(size_t i) {
        __sync_fetch_and_add(&sum, (long)i);
    });
    printf("dispatch apply sum=%ld\n", sum); // 4950
    return 0;
}
