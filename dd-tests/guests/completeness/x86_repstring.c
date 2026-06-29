#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
int main(void){
  char a[16]="abcdefghij", b[16]; memset(b,0,16);
  char *sp=a,*dp=b; size_t n=11;
  __asm__ volatile("rep movsb":"+S"(sp),"+D"(dp),"+c"(n)::"memory");
  char fill[16]; char *fp=fill; size_t fn=16;
  __asm__ volatile("rep stosb":"+D"(fp),"+c"(fn):"a"(0x41):"memory");
  char *s1=a,*s2=b; size_t cn=11; unsigned char eq;
  __asm__ volatile("repe cmpsb; sete %0":"=r"(eq),"+S"(s1),"+D"(s2),"+c"(cn)::"cc","memory");
  long r=b[0]+b[10]+fill[0]+eq; printf("repstring r=%ld\n", r); return 0; }
