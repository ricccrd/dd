// static macOS/arm64 raw-syscall guest (no libc, _start entry): getpid() via svc, then exit with a
// fixed code. Exercises the bare BSD syscall ABI (x16=number, svc #0x80) on the no-runtime path.
static long sc(long n, long a, long b, long c) {
    register long x16 asm("x16") = n;
    register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c;
    asm volatile("svc #0x80" : "+r"(x0) : "r"(x16), "r"(x1), "r"(x2) : "memory");
    return x0;
}
void start(void) {
    long pid = sc(20, 0, 0, 0);   // SYS_getpid
    sc(1, (pid > 0) ? 7 : 1, 0, 0); // SYS_exit(7 if pid sane)
}
