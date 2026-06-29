#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
static long go(void){
  long double a=2.0L, r;
  __asm__("fsqrt":"=t"(r):"0"(a));            /* x87 sqrt(2) */
  long double b=1.0L, c;
  __asm__("fldln2; fxch; fyl2x":"=t"(c):"0"(b):"st(1)"); /* trivial x87 transcendental path */
  return (long)(r*1000) + (long)(a+b); }
int main(void){ printf("x87 r=%ld\n", go()); return 0; }
