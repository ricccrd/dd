// Lock-free Treiber stack: 8 threads each push 1000 nodes onto a shared head via CAS on the pointer.
// Walking the final list must count exactly 8000 nodes. Verifies pointer-width atomic CAS. Golden.
#include <stdatomic.h>
#include <pthread.h>
#include <stdio.h>
struct node { struct node *next; long val; };
static _Atomic(struct node *) head;
#define N 8
#define PER 1000
static struct node pool[N * PER];
static atomic_int idx;
static void push(long v) {
    int i = atomic_fetch_add(&idx, 1);
    struct node *n = &pool[i]; n->val = v;
    struct node *old = atomic_load(&head);
    do { n->next = old; } while (!atomic_compare_exchange_weak(&head, &old, n));
}
static void *w(void *_) { (void)_; for (int i = 0; i < PER; i++) push(1); return 0; }
int main(void) {
    pthread_t t[N];
    for (int i = 0; i < N; i++) pthread_create(&t[i], 0, w, 0);
    for (int i = 0; i < N; i++) pthread_join(t[i], 0);
    long count = 0;
    for (struct node *p = atomic_load(&head); p; p = p->next) count++;
    printf("cas_ptr count=%ld\n", count); // 8000
    return 0;
}
