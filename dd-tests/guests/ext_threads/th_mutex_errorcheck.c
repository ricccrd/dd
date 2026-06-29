// Errorcheck mutex: a self re-lock returns EDEADLK (not a hang) and unlocking an unowned mutex
// returns EPERM. Exercises the diagnostic mutex contract. Portable -> all engines, golden.
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
int main(void) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_t m; pthread_mutex_init(&m, &a);
    pthread_mutex_lock(&m);
    int dl = pthread_mutex_lock(&m) == EDEADLK;
    pthread_mutex_unlock(&m);
    int pe = pthread_mutex_unlock(&m) == EPERM;
    printf("errorcheck edeadlk=%d eperm=%d\n", dl, pe);
    return 0;
}
