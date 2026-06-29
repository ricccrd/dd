// pthread_spinlock: 16 threads each spin-lock around a shared increment 20000 times. Final ==
// 320000. (macOS lacks pthread_spin_* -> Linux only, diffed against the native oracle.)
#include <pthread.h>
#include <stdio.h>
#define N 16
#define PER 20000
static pthread_spinlock_t sp;
static long shared;
static void *w(void *_) {
    (void)_;
    for (int i = 0; i < PER; i++) { pthread_spin_lock(&sp); shared++; pthread_spin_unlock(&sp); }
    return 0;
}
int main(void) {
    pthread_spin_init(&sp, 0);
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    printf("spinlock shared=%ld\n", shared); // 320000
    return 0;
}
