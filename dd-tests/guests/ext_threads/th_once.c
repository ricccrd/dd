// pthread_once: 32 threads each call pthread_once 1000 times on the same control; the init runs
// exactly once. The canonical thread-safe lazy-initialization primitive. Portable -> all, golden.
#include <pthread.h>
#include <stdio.h>
static pthread_once_t once = PTHREAD_ONCE_INIT;
static long init_count;
static void init(void) { init_count++; }
static void *w(void *_) { (void)_; for (int i = 0; i < 1000; i++) pthread_once(&once, init); return 0; }
int main(void) {
    pthread_t t[32];
    for (int i = 0; i < 32; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < 32; i++) pthread_join(t[i], 0);
    printf("once init_count=%ld\n", init_count); // 1
    return 0;
}
