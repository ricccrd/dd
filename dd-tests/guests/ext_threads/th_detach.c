// Detached threads: 32 threads run un-joined, each bumps an atomic; main polls until all reported.
// Verifies detach (no join) still runs the thread to completion and releases it. Portable, golden.
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
static atomic_int done;
static void *w(void *_) { (void)_; atomic_fetch_add(&done, 1); return 0; }
int main(void) {
    for (int i = 0; i < 32; i++) { pthread_t t; pthread_create(&t, 0, w, 0); pthread_detach(t); }
    while (atomic_load(&done) < 32) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
    printf("detach done=%d\n", atomic_load(&done)); // 32
    return 0;
}
