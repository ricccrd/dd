#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
__attribute__((target("+crypto"))) static long go(void){
  uint32x4_t a=vdupq_n_u32(1), b=vdupq_n_u32(2), c=vdupq_n_u32(3);
  uint32x4_t h=vsha256hq_u32(a,b,c); uint32x4_t h2=vsha256h2q_u32(a,b,c);
  uint32x4_t s0=vsha256su0q_u32(a,b); uint32x4_t s1=vsha256su1q_u32(a,b,c);
  long r=0; uint32_t o[4];
  vst1q_u32(o,h); r+=o[0]; vst1q_u32(o,h2); r+=o[1];
  vst1q_u32(o,s0); r+=o[2]; vst1q_u32(o,s1); r+=o[3]; return r&0xffffff; }
int main(void){ printf("crypto_sha256 r=%ld\n", go()); return 0; }
