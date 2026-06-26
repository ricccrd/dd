#include <stdio.h>
#include <setjmp.h>
static jmp_buf jb; static void f(int n){ if(!n) longjmp(jb,42); f(n-1); }
int main(void){ int r=setjmp(jb); if(!r){ f(8); printf("bad\n"); } else printf("longjmp r=%d\n",r); return 0; }
