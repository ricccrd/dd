// Lock-free counter via a compare_exchange_weak retry loop: 32 threads race to increment, final must
// be exactly 320000 (no lost updates). The classic CAS-loop contention stress. Portable -> all, golden.
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#define N 32
#define PER 10000
static atomic_long v;
static void *w(void *_) {
    (void)_;
    for (int i = 0; i < PER; i++) {
        long cur = atomic_load(&v);
        while (!atomic_compare_exchange_weak(&v, &cur, cur + 1)) {}
    }
    return 0;
}
int main(void) {
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    printf("cas v=%ld\n", (long)v); // 320000
    return 0;
}
