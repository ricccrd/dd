#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
#include <wmmintrin.h>
__attribute__((target("aes,sse4.1"))) static long go(void){
  __m128i a=_mm_set_epi32(0x01020304,0x05060708,0x090a0b0c,0x0d0e0f10), k=_mm_set1_epi32(0x11);
  __m128i e=_mm_aesenc_si128(a,k); e=_mm_aesenclast_si128(e,k);
  __m128i d=_mm_aesdec_si128(e,k); __m128i im=_mm_aesimc_si128(a);
  __m128i kg=_mm_aeskeygenassist_si128(a,1);
  long r=0; r+=_mm_extract_epi32(e,0); r+=_mm_extract_epi32(d,1);
  r+=_mm_extract_epi32(im,2); r+=_mm_extract_epi32(kg,3); return r; }
int main(void){ printf("aesni r=%ld\n", go()); return 0; }
