// Thread-local storage: each of 8 threads writes its index into a __thread variable, spins, and
// confirms the value is still its own (no cross-thread bleed). Exercises TLS setup per thread.
// Portable -> all engines, golden-checked.
#include <pthread.h>
#include <stdio.h>

static __thread long slot;
static long ok_count;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *arg) {
    long id = (long)arg;
    slot = id * 7 + 1;
    int good = 1;
    for (int i = 0; i < 100000; i++) {
        if (slot != id * 7 + 1) good = 0;
        slot += 0; // keep it live
    }
    pthread_mutex_lock(&m);
    if (good) ok_count++;
    pthread_mutex_unlock(&m);
    return 0;
}

int main(void) {
    pthread_t t[8];
    for (long i = 0; i < 8; i++) pthread_create(&t[i], 0, worker, (void *)i);
    for (int i = 0; i < 8; i++) pthread_join(t[i], 0);
    printf("tls ok=%ld\n", ok_count); // 8
    return 0;
}
