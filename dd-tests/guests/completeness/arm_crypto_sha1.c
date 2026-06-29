#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
__attribute__((target("+crypto"))) static long go(void){
  uint32x4_t a=vdupq_n_u32(0x67452301), b=vdupq_n_u32(0xefcdab89), w=vdupq_n_u32(0x5a827999);
  uint32x4_t c=vsha1cq_u32(a,0x12345678,w); uint32x4_t p=vsha1pq_u32(a,0x9abcdef0,w);
  uint32x4_t m=vsha1mq_u32(a,0x11111111,w); uint32_t h=vsha1h_u32(0x67452301);
  uint32x4_t su=vsha1su0q_u32(a,b,w);
  long r=h; uint32_t o[4]; vst1q_u32(o,c); r+=o[0]; vst1q_u32(o,p); r+=o[1];
  vst1q_u32(o,m); r+=o[2]; vst1q_u32(o,su); r+=o[3]; return r&0xffffff; }
int main(void){ printf("crypto_sha1 r=%ld\n", go()); return 0; }
