// SOAK: indirect-call / inline-cache endurance. A table of 64 distinct functions is called through a
// function pointer chosen by the running state, ~80M times. Every call is an indirect branch to a
// constantly-shifting target -- the worst case for an inline cache / IBTC, which is correct on a short
// run but can drift (megamorphic eviction, stale entry) only after sustained churn. Deterministic
// checksum -> golden, runs on every engine.
#include <stdint.h>
#include <stdio.h>

typedef uint64_t (*fn)(uint64_t);
#define F(n) static uint64_t f##n(uint64_t a) { return a * (2u * n + 1u) + (n + 0x100u); }
F(0) F(1) F(2) F(3) F(4) F(5) F(6) F(7) F(8) F(9) F(10) F(11) F(12) F(13) F(14) F(15)
F(16) F(17) F(18) F(19) F(20) F(21) F(22) F(23) F(24) F(25) F(26) F(27) F(28) F(29) F(30) F(31)
F(32) F(33) F(34) F(35) F(36) F(37) F(38) F(39) F(40) F(41) F(42) F(43) F(44) F(45) F(46) F(47)
F(48) F(49) F(50) F(51) F(52) F(53) F(54) F(55) F(56) F(57) F(58) F(59) F(60) F(61) F(62) F(63)
#define R(n) f##n,
static fn table[64] = {
    R(0) R(1) R(2) R(3) R(4) R(5) R(6) R(7) R(8) R(9) R(10) R(11) R(12) R(13) R(14) R(15)
    R(16) R(17) R(18) R(19) R(20) R(21) R(22) R(23) R(24) R(25) R(26) R(27) R(28) R(29) R(30) R(31)
    R(32) R(33) R(34) R(35) R(36) R(37) R(38) R(39) R(40) R(41) R(42) R(43) R(44) R(45) R(46) R(47)
    R(48) R(49) R(50) R(51) R(52) R(53) R(54) R(55) R(56) R(57) R(58) R(59) R(60) R(61) R(62) R(63)
};

int main(void) {
    uint64_t a = 0xdeadbeefULL;
    for (uint64_t i = 0; i < 80000000ULL; i++)
        a = table[(a ^ (a >> 7)) & 63](a); // target = f(state): a different one almost every call
    printf("soak indirect acc=%llu\n", (unsigned long long)a);
    return 0;
}
