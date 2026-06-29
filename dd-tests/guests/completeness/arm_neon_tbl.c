#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static long go(void){
  uint8_t t[16]; for(int i=0;i<16;i++) t[i]=i*2;
  uint8x16_t tbl=vld1q_u8(t);
  uint8_t ix[16]={15,14,13,12,11,10,9,8,7,6,5,4,3,2,1,0}; uint8x16_t idx=vld1q_u8(ix);
  uint8x16_t r1=vqtbl1q_u8(tbl,idx);
  uint8x16x2_t two={{tbl,tbl}}; uint8x16_t r2=vqtbl2q_u8(two,idx);
  long r=0; uint8_t o[16]; vst1q_u8(o,r1); for(int i=0;i<16;i++) r+=o[i];
  vst1q_u8(o,r2); for(int i=0;i<16;i++) r+=o[i]; return r; }
int main(void){ printf("neon_tbl r=%ld\n", go()); return 0; }
