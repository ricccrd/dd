// pthread_key destructors: 10 threads each set a key to a malloc'd value and exit; the registered
// destructor must fire once per thread (10 total). Verifies TSD teardown on thread exit. Golden.
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
static pthread_key_t key;
static long dtor_calls;
static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static void dtor(void *p) { pthread_mutex_lock(&m); dtor_calls++; pthread_mutex_unlock(&m); free(p); }
static void *w(void *_) { (void)_; pthread_setspecific(key, malloc(8)); return 0; }
int main(void) {
    pthread_key_create(&key, dtor);
    pthread_t t[10];
    for (int i = 0; i < 10; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < 10; i++) pthread_join(t[i], 0);
    printf("key_dtor calls=%ld\n", dtor_calls); // 10
    return 0;
}
