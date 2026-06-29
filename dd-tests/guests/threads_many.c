// Thread fan-out under contention: 64 threads each add 10000 to a shared counter guarded by a
// mutex, plus an atomic counter incremented the same way. Verifies the runtime scales past a
// handful of threads and that locking + C11 atomics agree. Portable -> all engines, golden.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

#define N 64
#define PER 10000

static long shared;
static atomic_long ashared;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *_) {
    (void)_;
    for (int i = 0; i < PER; i++) {
        pthread_mutex_lock(&m);
        shared++;
        pthread_mutex_unlock(&m);
        atomic_fetch_add(&ashared, 1);
    }
    return 0;
}

int main(void) {
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, worker, 0);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    printf("threads mutex=%ld atomic=%ld\n", shared, (long)ashared); // 640000 640000
    return 0;
}
