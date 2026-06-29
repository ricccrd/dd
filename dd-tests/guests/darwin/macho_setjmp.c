// ABI corner: setjmp/longjmp save and restore the arm64 callee-saved + SP/LR state. The darwin
// _setjmp/_longjmp must round-trip the non-zero longjmp value. darwin engine only, golden-checked.
#include <stdio.h>
#include <setjmp.h>

static jmp_buf jb;

int main(void) {
    int r = setjmp(jb);
    if (r == 0) longjmp(jb, 7);
    printf("setjmp r=%d\n", r); // 7
    return 0;
}
