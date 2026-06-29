// macOS-native pthread mutex contention: 4 threads each bump a shared counter 10k times under a
// mutex; the join'd total is deterministic. Exercises Apple pthread mutex + thread scheduling.
#include <stdio.h>
#include <pthread.h>

#define N 4
#define ITER 10000
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static long counter = 0;

static void *fn(void *a) {
    (void)a;
    for (int i = 0; i < ITER; i++) { pthread_mutex_lock(&m); counter++; pthread_mutex_unlock(&m); }
    return NULL;
}

int main(void) {
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], NULL, fn, NULL);
    for (int i = 0; i < N; i++) pthread_join(t[i], NULL);
    printf("pthread mutex counter=%ld\n", counter); // 40000
    return 0;
}
