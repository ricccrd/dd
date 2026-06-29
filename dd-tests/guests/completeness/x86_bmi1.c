#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("bmi"))) static long go(void){
  unsigned long a=0xF0F0F0F0UL, b=0x0FF00FF0UL;
  unsigned long r=_andn_u64(a,b); r+=_bextr_u64(0x12345678UL,4,8);
  r+=_blsi_u64(0xF0UL); r+=_blsr_u64(0xF0UL); r+=_blsmsk_u64(0xF0UL);
  r+=_tzcnt_u64(0x1000UL); return (long)(r&0xffffff); }
int main(void){ printf("bmi1 r=%ld\n", go()); return 0; }
