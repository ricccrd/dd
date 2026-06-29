// macOS-native GCD dispatch_group: fan 10 async adds out to a concurrent queue, group_wait for all,
// deterministic sum. Exercises GCD group barriers across worker threads. darwin engine only.
#include <stdio.h>
#include <dispatch/dispatch.h>

int main(void) {
    __block long sum = 0;
    dispatch_group_t g = dispatch_group_create();
    dispatch_queue_t q = dispatch_get_global_queue(0, 0);
    for (int i = 1; i <= 10; i++) {
        int v = i;
        dispatch_group_async(g, q, ^{ __sync_fetch_and_add(&sum, v); });
    }
    dispatch_group_wait(g, DISPATCH_TIME_FOREVER);
    printf("dispatch group sum=%ld\n", sum); // 55
    return 0;
}
