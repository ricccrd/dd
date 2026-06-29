// A signal arriving before the deadline makes pthread_cond_timedwait return 0 (the wakeup wins the
// race against the timeout). Verifies signal delivery to a timed waiter. Portable -> all, golden.
#include <pthread.h>
#include <stdio.h>
#include <time.h>
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static int ready;
static void *signaler(void *_) {
    (void)_;
    struct timespec ts = {0, 30000000}; nanosleep(&ts, 0);
    pthread_mutex_lock(&m); ready = 1; pthread_cond_signal(&c); pthread_mutex_unlock(&m);
    return 0;
}
int main(void) {
    pthread_t t; pthread_create(&t, 0, signaler, 0);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); ts.tv_sec += 5;
    pthread_mutex_lock(&m);
    int r = 0;
    while (!ready) r = pthread_cond_timedwait(&c, &m, &ts);
    pthread_mutex_unlock(&m);
    pthread_join(t, 0);
    printf("signal_wins rc=%d ready=%d\n", r, ready); // 0 1
    return 0;
}
