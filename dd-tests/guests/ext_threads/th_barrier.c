// pthread_barrier across 3 rounds: 8 threads rendezvous at the barrier each round, exactly one is
// elected SERIAL_THREAD per round to bump a counter (==3). (macOS lacks pthread_barrier -> Linux only.)
#include <pthread.h>
#include <stdio.h>
#define N 8
#define ROUNDS 3
static pthread_barrier_t bar;
static long serial_count, arrivals;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void *w(void *_) {
    (void)_;
    for (int r = 0; r < ROUNDS; r++) {
        pthread_mutex_lock(&m); arrivals++; pthread_mutex_unlock(&m);
        int rc = pthread_barrier_wait(&bar);
        if (rc == PTHREAD_BARRIER_SERIAL_THREAD) { pthread_mutex_lock(&m); serial_count++; pthread_mutex_unlock(&m); }
    }
    return 0;
}
int main(void) {
    pthread_barrier_init(&bar, 0, N);
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    printf("barrier serial=%ld arrivals=%ld\n", serial_count, arrivals); // 3 24
    return 0;
}
