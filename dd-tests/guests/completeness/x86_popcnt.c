#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("popcnt"))) static long go(void){
  return _mm_popcnt_u64(0xF0F0F0F0F0F0F0F0UL) + _mm_popcnt_u32(0xDEADBEEFu); }
int main(void){ printf("popcnt r=%ld\n", go()); return 0; }
