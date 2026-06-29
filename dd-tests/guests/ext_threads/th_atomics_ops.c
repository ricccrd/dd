// C11 atomic operations on one long, single-threaded and deterministic: fetch add/sub/or/and/xor,
// compare_exchange success+failure, and exchange. Verifies each RMW op's value semantics. Golden.
#include <stdatomic.h>
#include <stdio.h>
int main(void) {
    atomic_long v = 0;
    atomic_fetch_add(&v, 100);
    atomic_fetch_sub(&v, 30);
    atomic_fetch_or(&v, 0xF0);
    atomic_fetch_and(&v, 0xFF);
    atomic_fetch_xor(&v, 0x0F);
    long acc = atomic_load(&v); // 0->100->70->0xF6(246)->246->0xF9(249)
    long cur = acc;
    int ok = atomic_compare_exchange_strong(&v, &cur, 999);
    long cur2 = 12345;
    int fail = !atomic_compare_exchange_strong(&v, &cur2, 0);
    long old = atomic_exchange(&v, 7);
    printf("atomics v=%ld cas_ok=%d cas_fail=%d old=%ld final=%ld\n",
           acc, ok, fail, old, (long)atomic_load(&v)); // 249 1 1 999 7
    return 0;
}
