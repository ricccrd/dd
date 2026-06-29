#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
#include <wmmintrin.h>
__attribute__((target("pclmul,sse4.1"))) static long go(void){
  __m128i a=_mm_set_epi64x(0x1122334455667788UL,0x99aabbccddeeff00UL), b=_mm_set1_epi64x(0xff);
  __m128i p0=_mm_clmulepi64_si128(a,b,0x00); __m128i p1=_mm_clmulepi64_si128(a,b,0x11);
  return (long)(_mm_extract_epi64(p0,0) ^ _mm_extract_epi64(p1,1)) & 0xffffff; }
int main(void){ printf("pclmul r=%ld\n", go()); return 0; }
