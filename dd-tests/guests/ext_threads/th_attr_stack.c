// Thread attributes: set a 1 MiB stack via pthread_attr_setstacksize, read it back, then run a
// recursion that consumes real stack to prove the custom stack works. Portable -> all, golden.
#include <pthread.h>
#include <stdio.h>
static long sum_recurse(long n) {
    if (n == 0) return 0;
    volatile char pad[256]; pad[0] = 1;
    return pad[0] + n + sum_recurse(n - 1);
}
static void *w(void *_) { (void)_; return (void *)sum_recurse(1000); }
int main(void) {
    pthread_attr_t a; pthread_attr_init(&a);
    size_t sz = 1 << 20; pthread_attr_setstacksize(&a, sz);
    size_t got = 0; pthread_attr_getstacksize(&a, &got);
    pthread_t t; pthread_create(&t, &a, w, 0);
    void *r; pthread_join(t, &r);
    // sum_{1..1000}(1 + n) = 1000 + 500500 = 501500
    printf("attr_stack size_ok=%d result=%ld\n", got == sz, (long)r);
    return 0;
}
