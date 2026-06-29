#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("avx2"))) static long go(void){
  __m256i a=_mm256_set_epi32(1,2,3,4,5,6,7,8), b=_mm256_set1_epi32(3);
  __m256i s=_mm256_add_epi32(a,b); __m256i m=_mm256_mullo_epi32(a,b);
  __m256i pm=_mm256_permutevar8x32_epi32(a,_mm256_set_epi32(0,1,2,3,4,5,6,7));
  __m256i sv=_mm256_sllv_epi32(a,_mm256_set1_epi32(2));
  __m256i bc=_mm256_broadcastb_epi8(_mm_set1_epi8(4));
  long r=0; int t[8]; _mm256_storeu_si256((__m256i*)t,s); for(int i=0;i<8;i++) r+=t[i];
  _mm256_storeu_si256((__m256i*)t,m); for(int i=0;i<8;i++) r+=t[i];
  _mm256_storeu_si256((__m256i*)t,pm); for(int i=0;i<8;i++) r+=t[i];
  _mm256_storeu_si256((__m256i*)t,sv); for(int i=0;i<8;i++) r+=t[i];
  signed char c[32]; _mm256_storeu_si256((__m256i*)c,bc); for(int i=0;i<32;i++) r+=c[i]; return r; }
int main(void){ printf("avx2 r=%ld\n", go()); return 0; }
