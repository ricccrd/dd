#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
__attribute__((target("+crypto"))) static long go(void){
  uint8_t k[16]; for(int i=0;i<16;i++) k[i]=i+1;
  uint8x16_t s=vdupq_n_u8(0x33), key=vld1q_u8(k);
  uint8x16_t e=vaeseq_u8(s,key); e=vaesmcq_u8(e);
  uint8x16_t d=vaesdq_u8(e,key); d=vaesimcq_u8(d);
  long r=0; uint8_t o[16]; vst1q_u8(o,e); for(int i=0;i<16;i++) r+=o[i];
  vst1q_u8(o,d); for(int i=0;i<16;i++) r+=o[i]; return r&0xffffff; }
int main(void){ printf("crypto_aes r=%ld\n", go()); return 0; }
