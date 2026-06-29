#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("avx"))) static long go(void){
  __m256 a=_mm256_set_ps(1,2,3,4,5,6,7,8), b=_mm256_set1_ps(2.0f);
  __m256 s=_mm256_add_ps(a,b); __m256 m=_mm256_mul_ps(a,b);
  __m256d d=_mm256_add_pd(_mm256_set1_pd(1.5),_mm256_set1_pd(2.5));
  __m256 p=_mm256_permute2f128_ps(a,a,1);
  float f[8]; long r=0; _mm256_storeu_ps(f,s); for(int i=0;i<8;i++) r+=(long)f[i];
  _mm256_storeu_ps(f,m); for(int i=0;i<8;i++) r+=(long)f[i];
  _mm256_storeu_ps(f,p); for(int i=0;i<8;i++) r+=(long)f[i];
  double dd[4]; _mm256_storeu_pd(dd,d); for(int i=0;i<4;i++) r+=(long)dd[i]; return r; }
int main(void){ printf("avx r=%ld\n", go()); return 0; }
