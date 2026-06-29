// macOS-native GCD: dispatch_async onto a global concurrent queue, synchronized back via a dispatch
// semaphore. Exercises GCD worker threads + semaphore wakeup. darwin engine only, golden-checked.
#include <stdio.h>
#include <dispatch/dispatch.h>

int main(void) {
    __block int x = 0;
    dispatch_semaphore_t s = dispatch_semaphore_create(0);
    dispatch_queue_t q = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    dispatch_async(q, ^{ x = 42; dispatch_semaphore_signal(s); });
    dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER);
    printf("dispatch async x=%d\n", x); // 42
    return 0;
}
