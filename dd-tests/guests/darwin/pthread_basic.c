// macOS-native pthread create/join with a return value through libSystem's pthread (Apple's own,
// Mach-thread backed). darwin engine only, golden-checked.
#include <stdio.h>
#include <pthread.h>

static void *fn(void *a) { return (void *)(((long)a) * 2); }

int main(void) {
    pthread_t t;
    pthread_create(&t, NULL, fn, (void *)21);
    void *r;
    pthread_join(t, &r);
    printf("pthread ret=%ld\n", (long)r); // 42
    return 0;
}
