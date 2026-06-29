// pthread thread-specific data (pthread_key): 8 threads each store a per-thread value, mutate it
// 10000 times, and confirm no cross-thread bleed; the sum of finals is deterministic. Portable, golden.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
static pthread_key_t key;
#define N 8
static long total;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void *w(void *a) {
    long id = (long)a;
    long *v = malloc(sizeof(long)); *v = id * 100;
    pthread_setspecific(key, v);
    for (int i = 0; i < 10000; i++) { long *p = pthread_getspecific(key); *p += 1; }
    long final = *(long *)pthread_getspecific(key);
    pthread_mutex_lock(&m); total += final; pthread_mutex_unlock(&m);
    free(v);
    return 0;
}
int main(void) {
    pthread_key_create(&key, 0);
    pthread_t t[N];
    for (long i = 0; i < N; i++) pthread_create(&t[i], 0, w, (void *)i);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    // sum(id*100 + 10000) = 100*(0..7) + 8*10000 = 2800 + 80000 = 82800
    printf("key total=%ld\n", total);
    return 0;
}
