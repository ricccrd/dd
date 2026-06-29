// Mach-O thread-local storage: _Thread_local var has independent per-thread instances. Apple's TLV
// (__thread_vars / __thread_data sections) model differs from ELF TLS — a darwin corner. darwin only.
#include <stdio.h>
#include <pthread.h>

static _Thread_local int tv = 7;

static void *fn(void *a) {
    (void)a;
    tv = 99;
    return (void *)(long)tv;
}

int main(void) {
    pthread_t t;
    pthread_create(&t, NULL, fn, NULL);
    void *r;
    pthread_join(t, &r);
    printf("tls main=%d thread=%ld\n", tv, (long)r); // 7 99
    return 0;
}
