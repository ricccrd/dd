// POSIX named semaphore (sem_open) used as a mutex across 8 threads, each doing 10000 wait/post
// around a shared increment. Final == 80000. Named sems work on Linux and macOS. Portable, golden.
#include <semaphore.h>
#include <pthread.h>
#include <stdio.h>
#include <fcntl.h>
#define N 8
#define PER 10000
static sem_t *s;
static long total;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void *w(void *_) {
    (void)_;
    for (int i = 0; i < PER; i++) { sem_wait(s); pthread_mutex_lock(&m); total++; pthread_mutex_unlock(&m); sem_post(s); }
    return 0;
}
int main(void) {
    sem_unlink("/dd_sem_pc");
    s = sem_open("/dd_sem_pc", O_CREAT, 0600, 1);
    if (s == SEM_FAILED) { perror("sem_open"); return 1; }
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    sem_close(s); sem_unlink("/dd_sem_pc");
    printf("sem_named total=%ld\n", total); // 80000
    return 0;
}
