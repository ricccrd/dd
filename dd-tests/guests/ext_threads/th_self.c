// pthread_self / pthread_equal identity: 16 threads each confirm self==self and self!=main-thread.
// Verifies thread handles are stable and distinct. Portable -> all engines, golden.
#include <pthread.h>
#include <stdio.h>
static pthread_t main_id;
static long not_equal_main, self_consistent;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void *w(void *_) {
    (void)_;
    pthread_t s = pthread_self();
    int sc = pthread_equal(s, pthread_self());
    int ne = !pthread_equal(s, main_id);
    pthread_mutex_lock(&m); self_consistent += sc; not_equal_main += ne; pthread_mutex_unlock(&m);
    return 0;
}
int main(void) {
    main_id = pthread_self();
    pthread_t t[16];
    for (int i = 0; i < 16; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < 16; i++) pthread_join(t[i], 0);
    printf("self consistent=%ld ne_main=%ld\n", self_consistent, not_equal_main); // 16 16
    return 0;
}
