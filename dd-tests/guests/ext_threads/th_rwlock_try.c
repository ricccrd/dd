// rwlock try-variants on a read-held lock: another tryrdlock succeeds (shared reads stack) while
// trywrlock is denied with EBUSY. Both are well-defined cross-platform. Portable -> all, golden.
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
int main(void) {
    pthread_rwlock_t lk = PTHREAD_RWLOCK_INITIALIZER;
    pthread_rwlock_rdlock(&lk);
    int rd_ok = pthread_rwlock_tryrdlock(&lk) == 0;     // shared read allowed
    if (rd_ok) pthread_rwlock_unlock(&lk);
    int wr_busy = pthread_rwlock_trywrlock(&lk) == EBUSY; // writer must wait
    pthread_rwlock_unlock(&lk);
    printf("rwlock_try rd_ok=%d wr_busy=%d\n", rd_ok, wr_busy);
    return 0;
}
