// Reader/writer lock: 4 writers each increment a shared counter under the write lock while 8 readers
// repeatedly read it under the read lock and check consistency. Final == total writes. Portable, golden.
#include <pthread.h>
#include <stdio.h>
#define WR 4
#define RD 8
#define PER 10000
static pthread_rwlock_t lk = PTHREAD_RWLOCK_INITIALIZER;
static long shared, read_errors;
static void *writer(void *_) {
    (void)_;
    for (int i = 0; i < PER; i++) { pthread_rwlock_wrlock(&lk); shared++; pthread_rwlock_unlock(&lk); }
    return 0;
}
static void *reader(void *_) {
    (void)_;
    for (int i = 0; i < PER; i++) { pthread_rwlock_rdlock(&lk); long a = shared, b = shared; if (a != b) read_errors++; pthread_rwlock_unlock(&lk); }
    return 0;
}
int main(void) {
    pthread_t w[WR], r[RD];
    for (int i = 0; i < RD; i++) pthread_create(&r[i], 0, reader, 0);
    for (int i = 0; i < WR; i++) pthread_create(&w[i], 0, writer, 0);
    for (int i = 0; i < WR; i++) pthread_join(w[i], 0);
    for (int i = 0; i < RD; i++) pthread_join(r[i], 0);
    printf("rwlock shared=%ld read_errors=%ld\n", shared, read_errors); // 40000 0
    return 0;
}
