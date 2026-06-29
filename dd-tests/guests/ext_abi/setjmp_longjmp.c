// setjmp/longjmp across several frames; state kept in volatile globals (well-defined across longjmp).
#include <stdio.h>
#include <setjmp.h>
static jmp_buf jb;
static volatile long acc=0;
static volatile int hops=0;
static void deep(int d){ if(d==0) longjmp(jb, 77); deep(d-1); }
int main(void){
  int rc=setjmp(jb);
  if(rc==0){ acc+=1; deep(20); }
  else {
    hops++; acc += rc;
    if(hops<3){ acc+=10; longjmp(jb, rc+1); }   // re-arm a couple of times
  }
  printf("acc=%ld hops=%d last_rc=%d\n", acc, hops, rc);
  return 0;
}
