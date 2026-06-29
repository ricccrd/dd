// Explicit memory orders under contention: 32 threads each do 10000 fetch_add with relaxed, acq_rel,
// and seq_cst ordering. RMW totals must be exact regardless of order (320000 each). Portable, golden.
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
#define N 32
#define PER 10000
static atomic_long relaxed, acqrel, seqcst;
static void *w(void *_) {
    (void)_;
    for (int i = 0; i < PER; i++) {
        atomic_fetch_add_explicit(&relaxed, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&acqrel, 1, memory_order_acq_rel);
        atomic_fetch_add_explicit(&seqcst, 1, memory_order_seq_cst);
    }
    return 0;
}
int main(void) {
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    printf("orders relaxed=%ld acqrel=%ld seqcst=%ld\n", (long)relaxed, (long)acqrel, (long)seqcst);
    return 0;
}
