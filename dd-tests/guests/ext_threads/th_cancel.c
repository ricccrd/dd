// pthread_cancel with a cleanup handler: 8 threads spin at a cancellation point; main cancels each,
// and every thread's pushed cleanup handler must run exactly once (8 total). Portable -> all, golden.
#include <pthread.h>
#include <stdio.h>
#include <time.h>
static long cleaned;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void cleanup(void *_) { (void)_; pthread_mutex_lock(&m); cleaned++; pthread_mutex_unlock(&m); }
static void *w(void *_) {
    (void)_;
    pthread_cleanup_push(cleanup, 0);
    for (;;) { pthread_testcancel(); struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
    pthread_cleanup_pop(0);
    return 0;
}
int main(void) {
    pthread_t t[8];
    for (int i = 0; i < 8; i++) pthread_create(&t[i], 0, w, 0);
    struct timespec ts = {0, 50000000}; nanosleep(&ts, 0);
    for (int i = 0; i < 8; i++) pthread_cancel(t[i]);
    for (int i = 0; i < 8; i++) { void *r; pthread_join(t[i], &r); }
    printf("cancel cleaned=%ld\n", cleaned); // 8
    return 0;
}
