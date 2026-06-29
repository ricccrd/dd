// macOS-native GCD counting semaphore: a sem with initial value 3 grants exactly 3 non-blocking
// waits, then the 4th times out. Exercises dispatch_semaphore accounting. darwin engine only.
#include <stdio.h>
#include <dispatch/dispatch.h>

int main(void) {
    dispatch_semaphore_t s = dispatch_semaphore_create(3);
    int a = dispatch_semaphore_wait(s, DISPATCH_TIME_NOW);
    int b = dispatch_semaphore_wait(s, DISPATCH_TIME_NOW);
    int c = dispatch_semaphore_wait(s, DISPATCH_TIME_NOW);
    int d = dispatch_semaphore_wait(s, DISPATCH_TIME_NOW);
    printf("dispatch sem %d%d%d%d\n", a == 0, b == 0, c == 0, d != 0); // 1111
    return 0;
}
