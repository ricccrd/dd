// SOAK: contended atomics endurance. 8 threads hammer a shared counter for 2,000,000 iterations each:
// every iteration does an atomic fetch-add AND a compare-and-swap retry loop on a second word. That is
// ~16M lock-free RMW operations under real contention, exercising the JIT's lowering of atomics
// (LDXR/STXR load-linked/store-conditional retry on aarch64; LOCK XADD / LOCK CMPXCHG on x86) and its
// memory ordering over a long, contended run -- where a dropped exclusive monitor, a missed barrier, or a
// CAS that doesn't actually retry produces a lost update only under sustained pressure. Both final
// totals are order-independent and exact -> golden, runs on every engine.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#define NITER 2000000
#define NTHREAD 8

static atomic_ullong counter;   // exact final value = NITER*NTHREAD
static atomic_ullong casacc;    // each successful CAS adds 1 -> also NITER*NTHREAD

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < NITER; i++) {
        atomic_fetch_add_explicit(&counter, 1, memory_order_relaxed);
        unsigned long long cur = atomic_load_explicit(&casacc, memory_order_relaxed);
        while (!atomic_compare_exchange_weak_explicit(&casacc, &cur, cur + 1,
                   memory_order_acq_rel, memory_order_relaxed)) { /* reloads cur, retry */ }
    }
    return 0;
}

int main(void) {
    pthread_t th[NTHREAD];
    for (int i = 0; i < NTHREAD; i++) pthread_create(&th[i], 0, worker, 0);
    for (int i = 0; i < NTHREAD; i++) pthread_join(th[i], 0);
    printf("soak atomicchurn counter=%llu cas=%llu\n",
           (unsigned long long)counter, (unsigned long long)casacc);
    return 0;
}
