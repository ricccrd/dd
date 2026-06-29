// atomic_flag spinlock (the most primitive portable lock): 16 threads each take/clear the flag 20000
// times around a shared increment. Final == 16*20000. Verifies test-and-set + acquire/release. Golden.
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#define N 16
#define PER 20000
static atomic_flag lock = ATOMIC_FLAG_INIT;
static long shared;
static void *w(void *_) {
    (void)_;
    for (int i = 0; i < PER; i++) {
        while (atomic_flag_test_and_set_explicit(&lock, memory_order_acquire)) {}
        shared++;
        atomic_flag_clear_explicit(&lock, memory_order_release);
    }
    return 0;
}
int main(void) {
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    printf("atomic_flag shared=%ld\n", shared); // 320000
    return 0;
}
