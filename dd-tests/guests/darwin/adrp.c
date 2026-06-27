// static macOS/arm64 guest: write a __cstring string LITERAL then exit(42), via raw BSD syscalls.
// The literal's address is materialized with adrp+add -- so this exercises the segment-slide relocation
// jitdarwin must apply to adrp (hello.c dodges it by building its string on the stack). If adrp isn't
// relocated, the guest reads the wrong page and prints zeros / faults.
static long sc(long n, long a, long b, long c) {
    register long x16 asm("x16") = n;
    register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c;
    asm volatile("svc #0x80" : "+r"(x0) : "r"(x16), "r"(x1), "r"(x2) : "memory");
    return x0;
}
void start(void) {
    sc(4, 1, (long)"ADRP-OK\n", 8);   // SYS_write(1, <cstring literal via adrp>, 8)
    sc(1, 42, 0, 0);                   // SYS_exit(42)
}
