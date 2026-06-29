// SOAK: thread create/teardown endurance. Spawn-and-join 4000 short-lived threads in sequence; each
// does a little work into a shared atomic. The bug class this catches: per-thread JIT/runtime state
// (TLS slots, thread-context structs, stacks, host pthreads) that leaks or isn't recycled, so a runtime
// that's fine for a few threads exhausts a table / FDs / memory after thousands over a long run.
// Deterministic total -> golden, runs on every engine.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

static atomic_long total;

static void *worker(void *arg) {
    long id = (long)arg;
    long s = 0;
    for (int i = 0; i < 1000; i++) s += (id + i) & 7;
    atomic_fetch_add(&total, s);
    return 0;
}

int main(void) {
    for (long i = 0; i < 4000; i++) {
        pthread_t t;
        if (pthread_create(&t, 0, worker, (void *)i) != 0) { printf("soak threadchurn create_fail@%ld\n", i); return 1; }
        pthread_join(t, 0);
    }
    printf("soak threadchurn total=%ld\n", (long)total);
    return 0;
}
