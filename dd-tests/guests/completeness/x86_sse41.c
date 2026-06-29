#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
#include <smmintrin.h>
__attribute__((target("sse4.1"))) static long go(void){
  __m128i a=_mm_set_epi32(1,2,3,4), b=_mm_set_epi32(5,6,7,8);
  __m128i m=_mm_mullo_epi32(a,b); __m128i mn=_mm_min_epu32(a,b); __m128i mx=_mm_max_epi32(a,b);
  __m128 fa=_mm_set_ps(1,2,3,4); __m128 dp=_mm_dp_ps(fa,fa,0xff);
  __m128d rd=_mm_round_pd(_mm_set1_pd(2.7),_MM_FROUND_TO_NEAREST_INT|_MM_FROUND_NO_EXC);
  int ex=_mm_extract_epi32(a,2);
  __m128i wb=_mm_cvtepi8_epi16(_mm_set1_epi8(-3));
  long r=ex; int t[4]; _mm_storeu_si128((__m128i*)t,m); for(int i=0;i<4;i++) r+=t[i];
  _mm_storeu_si128((__m128i*)t,mn); for(int i=0;i<4;i++) r+=t[i];
  _mm_storeu_si128((__m128i*)t,mx); for(int i=0;i<4;i++) r+=t[i];
  short s[8]; _mm_storeu_si128((__m128i*)s,wb); for(int i=0;i<8;i++) r+=s[i];
  r+=(long)_mm_cvtss_f32(dp); r+=(long)_mm_cvtsd_f64(rd); return r; }
int main(void){ printf("sse41 r=%ld\n", go()); return 0; }
