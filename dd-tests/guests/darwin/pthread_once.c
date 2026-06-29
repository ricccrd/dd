// macOS-native pthread_once: the initializer must run exactly once across 5 calls. darwin engine only.
#include <stdio.h>
#include <pthread.h>

static pthread_once_t once = PTHREAD_ONCE_INIT;
static int count = 0;
static void init(void) { count++; }

int main(void) {
    for (int i = 0; i < 5; i++) pthread_once(&once, init);
    printf("pthread once count=%d\n", count); // 1
    return 0;
}
