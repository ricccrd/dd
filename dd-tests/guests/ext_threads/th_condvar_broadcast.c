// Condition variable broadcast: 16 threads block on a gate predicate; main raises the gate and
// broadcasts; all 16 wake and count. Verifies cond_broadcast wakes every waiter. Portable, golden.
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#define N 16
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static int gate;
static long awake;
static void *w(void *_) {
    (void)_;
    pthread_mutex_lock(&m);
    while (!gate) pthread_cond_wait(&c, &m);
    awake++;
    pthread_mutex_unlock(&m);
    return 0;
}
int main(void) {
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, w, 0);
    struct timespec ts = {0, 50000000}; nanosleep(&ts, 0);
    pthread_mutex_lock(&m); gate = 1; pthread_cond_broadcast(&c); pthread_mutex_unlock(&m);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    printf("broadcast awake=%ld\n", awake); // 16
    return 0;
}
