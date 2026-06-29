// Mutex contention stress (the futex fast/slow path under load): 16 threads each take a single shared
// mutex 100000 times around a tiny critical section. Final == 1600000. Portable -> all, golden.
#include <pthread.h>
#include <stdio.h>
#define N 16
#define PER 100000
static long shared;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void *w(void *_) {
    (void)_;
    for (int i = 0; i < PER; i++) { pthread_mutex_lock(&m); shared++; pthread_mutex_unlock(&m); }
    return 0;
}
int main(void) {
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    printf("mutex_stress shared=%ld\n", shared); // 1600000
    return 0;
}
