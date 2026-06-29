// macOS-native pthread thread-specific data: pthread_key_create + set/getspecific in a child thread.
// Apple's TSD slots are a known darwin corner (fast %gs-like TSD via __pthread). darwin engine only.
#include <stdio.h>
#include <pthread.h>

static pthread_key_t key;

static void *fn(void *a) {
    pthread_setspecific(key, a);
    return pthread_getspecific(key);
}

int main(void) {
    pthread_key_create(&key, NULL);
    pthread_t t;
    pthread_create(&t, NULL, fn, (void *)99);
    void *r;
    pthread_join(t, &r);
    printf("pthread tsd=%ld\n", (long)r); // 99
    return 0;
}
