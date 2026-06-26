// static macOS/arm64 guest: write "hi\n" then exit(42) via raw BSD syscalls (svc #0x80).
// Build the string on the stack (SP-relative) rather than a __cstring literal (adrp), which the
// minimal jitdarwin engine doesn't yet relocate.
static long sc(long n, long a, long b, long c) {
    register long x16 asm("x16") = n;
    register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c;
    asm volatile("svc #0x80" : "+r"(x0) : "r"(x16), "r"(x1), "r"(x2) : "memory");
    return x0;
}
void start(void) {
    volatile char buf[4];
    buf[0] = 'h'; buf[1] = 'i'; buf[2] = '\n';
    sc(4, 1, (long)buf, 3);   // SYS_write(1, buf, 3)
    sc(1, 42, 0, 0);          // SYS_exit(42)
}
