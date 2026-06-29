#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("bmi2"))) static long go(void){
  unsigned long r=_pdep_u64(0xFFUL,0x5555UL); r+=_pext_u64(0xAAAAUL,0xF0F0UL);
  unsigned long long hi; unsigned long long lo=_mulx_u64(0x100000001UL,0x3UL,&hi); r+=lo+hi;
  r+=_bzhi_u64(0xFFFFFFFFUL,12); return (long)(r&0xffffff); }
int main(void){ printf("bmi2 r=%ld\n", go()); return 0; }
