// N threads contending on a mutex-guarded counter (exercises clone/futex + §B under threads).
#include <stdio.h>
#include <pthread.h>
#define N 8
static long c = 0;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void *w(void *a) { (void)a; for (int i = 0; i < 100000; i++) { pthread_mutex_lock(&m); c++; pthread_mutex_unlock(&m); } return 0; }
int main(void) {
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    printf("threads sum=%ld\n", c);
    return 0;
}
