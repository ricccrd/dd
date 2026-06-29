// Recursive mutex: the same thread locks the mutex 100 levels deep through recursion, then relocks
// it 50 times in a flat loop. A non-recursive mutex would deadlock. Portable -> all engines, golden.
#include <pthread.h>
#include <stdio.h>
static pthread_mutex_t m;
static void rec(int d) { if (!d) return; pthread_mutex_lock(&m); rec(d - 1); pthread_mutex_unlock(&m); }
int main(void) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m, &a);
    rec(100);
    long c = 0;
    for (int i = 0; i < 50; i++) { pthread_mutex_lock(&m); c++; }
    for (int i = 0; i < 50; i++) pthread_mutex_unlock(&m);
    printf("recursive depth=%d relock=%ld\n", 100, c);
    return 0;
}
