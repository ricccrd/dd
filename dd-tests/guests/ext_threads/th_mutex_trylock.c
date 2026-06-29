// pthread_mutex_trylock: succeeds on a free mutex, returns EBUSY when already held by this thread,
// succeeds again after unlock. Single-threaded so the outcome is deterministic. Portable, golden.
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
int main(void) {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    int ok = pthread_mutex_trylock(&m) == 0;
    int busy = pthread_mutex_trylock(&m) == EBUSY;
    pthread_mutex_unlock(&m);
    int again = pthread_mutex_trylock(&m) == 0;
    pthread_mutex_unlock(&m);
    printf("trylock ok=%d busy=%d again=%d\n", ok, busy, again);
    return 0;
}
