#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("sha,sse4.1"))) static long go(void){
  __m128i a=_mm_set_epi32(1,2,3,4), b=_mm_set_epi32(5,6,7,8), c=_mm_set1_epi32(9);
  __m128i r=_mm_sha256rnds2_epu32(a,b,c); r=_mm_sha256msg1_epu32(r,b);
  __m128i e=_mm_sha1rnds4_epu32(a,b,0); 
  return (long)((_mm_extract_epi32(r,0) ^ _mm_extract_epi32(e,1)) & 0xffffff); }
int main(void){ printf("sha r=%ld\n", go()); return 0; }
