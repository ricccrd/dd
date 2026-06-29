#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("sse2"))) static long go(void){
  __m128i a=_mm_set_epi32(1,2,3,4), b=_mm_set_epi32(5,6,7,8);
  __m128i s=_mm_add_epi32(a,b); s=_mm_sub_epi32(s,_mm_set1_epi32(1));
  __m128i m=_mm_mullo_epi16(a,b); __m128i sl=_mm_slli_epi32(a,2);
  __m128d d=_mm_add_pd(_mm_set1_pd(1.5),_mm_set1_pd(2.25));
  __m128i u=_mm_unpacklo_epi32(a,b);
  long r=0; int t[4]; _mm_storeu_si128((__m128i*)t,s); for(int i=0;i<4;i++) r+=t[i];
  _mm_storeu_si128((__m128i*)t,m); for(int i=0;i<4;i++) r+=t[i];
  _mm_storeu_si128((__m128i*)t,sl); for(int i=0;i<4;i++) r+=t[i];
  _mm_storeu_si128((__m128i*)t,u); for(int i=0;i<4;i++) r+=t[i];
  r+=(long)_mm_cvtsd_f64(d); return r; }
int main(void){ printf("sse2 r=%ld\n", go()); return 0; }
