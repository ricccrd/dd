// macOS-native GCD: dispatch_sync onto a serial queue runs the block (a clang Block) and returns.
// Grand Central Dispatch + blocks runtime — no Linux equivalent. darwin engine only, golden-checked.
#include <stdio.h>
#include <dispatch/dispatch.h>

int main(void) {
    __block int x = 0;
    dispatch_queue_t q = dispatch_queue_create("dd.test", NULL);
    dispatch_sync(q, ^{ x = 42; });
    printf("dispatch sync x=%d\n", x); // 42
    return 0;
}
