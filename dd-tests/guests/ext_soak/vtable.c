// SOAK: megamorphic virtual dispatch (double-indirect) endurance. An array of 256 "objects", each
// holding a method pointer + private state, is walked in a data-dependent order for ~60M iterations;
// every step LOADS the object, LOADS its method pointer from the struct, and calls through it. The
// single call site therefore sees a constantly-shifting target reached via a memory load -- the worst
// case for an inline cache / IBTC that caches on the call site, which can drift (stale entry,
// megamorphic eviction) only after sustained churn. Deterministic checksum -> golden, every engine.
#include <stdint.h>
#include <stdio.h>

struct obj;
typedef uint64_t (*method)(struct obj *, uint64_t);
struct obj { method m; uint64_t s; };

#define M(n) static uint64_t m##n(struct obj *o, uint64_t a) { return (a + o->s) * (2u * n + 1u) ^ (o->s >> (n & 7)); }
M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7)
static method methods[8] = { m0, m1, m2, m3, m4, m5, m6, m7 };

int main(void) {
    struct obj objs[256];
    for (int i = 0; i < 256; i++) { objs[i].m = methods[i & 7]; objs[i].s = 0x1000003ULL * (uint64_t)(i + 1); }
    uint64_t a = 0xdeadbeefULL;
    for (uint64_t i = 0; i < 80000000ULL; i++) {
        struct obj *o = &objs[(a ^ (a >> 9)) & 255]; // object = f(state)
        a = o->m(o, a);                               // call through a loaded pointer (megamorphic)
    }
    printf("soak vtable acc=%llu\n", (unsigned long long)a);
    return 0;
}
