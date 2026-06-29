// SOAK: long-lived thread pool / condvar+mutex endurance. 8 worker threads consume a bounded
// producer-consumer queue while the main thread enqueues 2,000,000 tasks; each task carries a fixed
// value that a worker folds into a shared total under the lock. Every enqueue/dequeue is a mutex +
// condvar signal/wait, so the run drives millions of futex/condvar operations and lock hand-offs through
// the JIT -- the regime where a lost wakeup, a mishandled futex requeue, or a TLS/lock-state leak only
// manifests after sustained contention. The total is the sum of all task values and is independent of
// scheduling order, so it is deterministic -> golden, runs on every engine.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#define CAP 256
#define NTASKS 1000000
#define NWORK 8

static long q[CAP];
static int head, tail, count, done;
static long total;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t not_full = PTHREAD_COND_INITIALIZER;

static void *worker(void *arg) {
    (void)arg;
    long local = 0;
    for (;;) {
        pthread_mutex_lock(&mu);
        while (count == 0 && !done) pthread_cond_wait(&not_empty, &mu);
        if (count == 0 && done) { pthread_mutex_unlock(&mu); break; }
        long v = q[head]; head = (head + 1) % CAP; count--;
        pthread_cond_signal(&not_full);
        pthread_mutex_unlock(&mu);
        local += v;
    }
    pthread_mutex_lock(&mu); total += local; pthread_mutex_unlock(&mu);
    return 0;
}

int main(void) {
    pthread_t th[NWORK];
    for (int i = 0; i < NWORK; i++) pthread_create(&th[i], 0, worker, 0);
    for (long i = 0; i < NTASKS; i++) {
        pthread_mutex_lock(&mu);
        while (count == CAP) pthread_cond_wait(&not_full, &mu);
        q[tail] = (i % 101) + 1; tail = (tail + 1) % CAP; count++;
        pthread_cond_signal(&not_empty);
        pthread_mutex_unlock(&mu);
    }
    pthread_mutex_lock(&mu); done = 1; pthread_cond_broadcast(&not_empty); pthread_mutex_unlock(&mu);
    for (int i = 0; i < NWORK; i++) pthread_join(th[i], 0);
    printf("soak threadpool total=%ld\n", total); // sum_{i=0..NTASKS-1} (i%101)+1, order-independent
    return 0;
}
