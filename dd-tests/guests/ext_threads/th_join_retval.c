// pthread_create/join carrying a return value: 16 threads each return id*id, the joiner sums them.
// Verifies the join handshake propagates the thread's exit pointer. Portable -> all engines, golden.
#include <pthread.h>
#include <stdio.h>
static void *w(void *a) { long id = (long)a; return (void *)(id * id); }
int main(void) {
    pthread_t t[16];
    long sum = 0;
    for (long i = 0; i < 16; i++) pthread_create(&t[i], 0, w, (void *)i);
    for (int i = 0; i < 16; i++) { void *r; pthread_join(t[i], &r); sum += (long)r; }
    printf("join sum=%ld\n", sum); // sum i^2, 0..15 = 1240
    return 0;
}
