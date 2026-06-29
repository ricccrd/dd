// Best-effort scheduling APIs: set a thread attr's sched policy, query the priority range, create a
// thread under it, and call sched_yield. Verdict-only (priorities aren't enforced). Portable, golden.
#include <pthread.h>
#include <stdio.h>
#include <sched.h>
static void *w(void *_) { (void)_; return 0; }
int main(void) {
    pthread_attr_t a; pthread_attr_init(&a);
    int r1 = pthread_attr_setschedpolicy(&a, SCHED_OTHER);
    int mn = sched_get_priority_min(SCHED_OTHER);
    int mx = sched_get_priority_max(SCHED_OTHER);
    pthread_t t; int r2 = pthread_create(&t, &a, w, 0); pthread_join(t, 0);
    int r3 = sched_yield();
    printf("sched setpol=%d created=%d yield=%d range_ok=%d\n", r1 == 0, r2 == 0, r3 == 0, mn <= mx);
    return 0;
}
