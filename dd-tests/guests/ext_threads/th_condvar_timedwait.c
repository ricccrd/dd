// pthread_cond_timedwait that nobody signals must return ETIMEDOUT (not hang, not spurious-OK).
// Exercises the absolute-deadline timed wait against CLOCK_REALTIME. Portable -> all engines, golden.
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
int main(void) {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t c = PTHREAD_COND_INITIALIZER;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 50000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&m);
    int r = pthread_cond_timedwait(&c, &m, &ts);
    pthread_mutex_unlock(&m);
    printf("timedwait timeout=%d\n", r == ETIMEDOUT);
    return 0;
}
