#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("adx"))) static long go(void){
  unsigned long long s=0; unsigned char c=0;
  c=_addcarryx_u64(c, 0xFFFFFFFFFFFFFFFFUL, 1UL, &s);
  unsigned long long s2=0; c=_addcarryx_u64(c, 5UL, 6UL, &s2);
  return (long)(s2 + c); }
int main(void){ printf("adx r=%ld\n", go()); return 0; }
