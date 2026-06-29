// macOS-native pthread condition variable: a producer signals a value, the main thread waits on the
// cond and reads it. Exercises Apple pthread cond/mutex wakeup. darwin engine only.
#include <stdio.h>
#include <pthread.h>

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t c = PTHREAD_COND_INITIALIZER;
static int ready = 0, val = 0;

static void *prod(void *a) {
    (void)a;
    pthread_mutex_lock(&m);
    val = 42; ready = 1;
    pthread_cond_signal(&c);
    pthread_mutex_unlock(&m);
    return NULL;
}

int main(void) {
    pthread_t t;
    pthread_create(&t, NULL, prod, NULL);
    pthread_mutex_lock(&m);
    while (!ready) pthread_cond_wait(&c, &m);
    int v = val;
    pthread_mutex_unlock(&m);
    pthread_join(t, NULL);
    printf("pthread cond val=%d\n", v); // 42
    return 0;
}
