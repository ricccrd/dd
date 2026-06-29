// static macOS/arm64 raw-syscall guest (no libc, _start): pipe() returns the two fds in x0/x1 (a
// darwin multi-return-register syscall — different from Linux which writes an array). Write a byte
// through the pipe and read it back, then exit 0 on success. Exercises the x0/x1 dual-return ABI.
static long sc(long n, long a, long b, long c) {
    register long x16 asm("x16") = n;
    register long x0 asm("x0") = a, x1 asm("x1") = b, x2 asm("x2") = c;
    asm volatile("svc #0x80" : "+r"(x0) : "r"(x16), "r"(x1), "r"(x2) : "memory");
    return x0;
}
// pipe() returns rd in x0 and wr in x1; capture both.
static void pipe2fds(int *rd, int *wr) {
    register long x16 asm("x16") = 42; // SYS_pipe
    register long x0 asm("x0");
    register long x1 asm("x1");
    asm volatile("svc #0x80" : "=r"(x0), "=r"(x1) : "r"(x16) : "memory");
    *rd = (int)x0; *wr = (int)x1;
}
void start(void) {
    int rd, wr;
    pipe2fds(&rd, &wr);
    char out = 'Z';
    sc(4, wr, (long)&out, 1);    // SYS_write(wr, "Z", 1)
    char in = 0;
    long n = sc(3, rd, (long)&in, 1); // SYS_read(rd, &in, 1)
    sc(1, (n == 1 && in == 'Z') ? 0 : 1, 0, 0); // SYS_exit
}
