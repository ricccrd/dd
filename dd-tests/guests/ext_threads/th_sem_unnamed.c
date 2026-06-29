// Unnamed POSIX semaphore (sem_init, pshared=0) as a mutex across 8 threads, 10000 wait/post each.
// Final == 80000. (macOS sem_init is unsupported -> Linux only, diffed against the native oracle.)
#include <semaphore.h>
#include <pthread.h>
#include <stdio.h>
#define N 8
#define PER 10000
static sem_t s;
static long total;
static void *w(void *_) {
    (void)_;
    for (int i = 0; i < PER; i++) { sem_wait(&s); total++; sem_post(&s); }
    return 0;
}
int main(void) {
    sem_init(&s, 0, 1);
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    sem_destroy(&s);
    printf("sem_unnamed total=%ld\n", total); // 80000
    return 0;
}
