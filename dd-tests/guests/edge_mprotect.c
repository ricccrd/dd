// EDGE: mprotect(PROT_NONE) must make a mapping inaccessible — a read then faults (SIGSEGV). We
// catch it with a handler + sigsetjmp and report whether the fault happened, then restore PROT_READ
// and confirm the byte is readable. A runtime that no-ops mprotect never faults (faulted=0).
// Deterministic verdict -> golden across engines.
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

static sigjmp_buf jb;
static void segv(int s) { (void)s; siglongjmp(jb, 1); }

int main(void) {
    struct sigaction sa = {0};
    sa.sa_handler = segv;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    volatile unsigned char *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) { perror("mmap"); return 1; }
    m[0] = 0x7e;
    mprotect((void *)m, 4096, PROT_NONE);

    int faulted = 0;
    if (sigsetjmp(jb, 1) == 0) {
        volatile unsigned char x = m[0]; // should fault under PROT_NONE
        (void)x;
    } else {
        faulted = 1;
    }
    mprotect((void *)m, 4096, PROT_READ);
    int readable = (m[0] == 0x7e);
    printf("mprotect faulted=%d readable_after=%d\n", faulted, readable); // 1 1
    return 0;
}
