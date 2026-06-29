#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
#include <tmmintrin.h>
__attribute__((target("ssse3"))) static long go(void){
  __m128i a=_mm_set_epi8(1,-2,3,-4,5,-6,7,-8,9,-10,11,-12,13,-14,15,-16);
  __m128i idx=_mm_set_epi8(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15);
  __m128i sh=_mm_shuffle_epi8(a,idx); __m128i ab=_mm_abs_epi8(a);
  __m128i ph=_mm_hadd_epi16(a,a); __m128i al=_mm_alignr_epi8(a,a,3);
  long r=0; signed char t[16]; _mm_storeu_si128((__m128i*)t,sh); for(int i=0;i<16;i++) r+=t[i];
  _mm_storeu_si128((__m128i*)t,ab); for(int i=0;i<16;i++) r+=t[i];
  _mm_storeu_si128((__m128i*)t,ph); for(int i=0;i<16;i++) r+=t[i];
  _mm_storeu_si128((__m128i*)t,al); for(int i=0;i<16;i++) r+=t[i]; return r; }
int main(void){ printf("ssse3 r=%ld\n", go()); return 0; }
