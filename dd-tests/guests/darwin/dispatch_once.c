// macOS-native GCD dispatch_once: the block runs exactly once across 5 calls (the Objective-C/Swift
// lazy-init primitive). darwin engine only, golden-checked.
#include <stdio.h>
#include <dispatch/dispatch.h>

static dispatch_once_t once;
static int count = 0;

int main(void) {
    for (int i = 0; i < 5; i++) dispatch_once(&once, ^{ count++; });
    printf("dispatch once count=%d\n", count); // 1
    return 0;
}
