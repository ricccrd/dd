// setjmp/longjmp value passing incl the 0->1 conversion rule. Portable verdicts.
#include <stdio.h>
#include <setjmp.h>

static jmp_buf jb;
static void f(int n) { longjmp(jb, n); }

int main(void) {
    volatile int d1 = 0, d2 = 0;
    int v = setjmp(jb);
    if (v == 0) { f(42); }
    else { d1 = (v == 42); }

    static jmp_buf jb2;
    int w = setjmp(jb2);
    if (w == 0) { longjmp(jb2, 0); } // 0 must come back as 1
    else { d2 = (w == 1); }

    printf("setjmp_val d1=%d d2=%d\n", d1, d2);
    return 0;
}
