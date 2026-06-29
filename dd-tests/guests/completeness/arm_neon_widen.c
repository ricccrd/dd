#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static long go(void){
  int16_t va[4]={100,-200,300,-400}; int16x4_t a=vld1_s16(va);
  int32x4_t w=vmovl_s16(a); int32x4_t ml=vmull_s16(a,vdup_n_s16(3));
  int16x8_t va8=vcombine_s16(a,a); int8x8_t n=vqmovn_s16(va8);
  long r=0; int32_t o[4];
  vst1q_s32(o,w); for(int i=0;i<4;i++) r+=o[i];
  vst1q_s32(o,ml); for(int i=0;i<4;i++) r+=o[i];
  int8_t no[8]; vst1_s8(no,n); for(int i=0;i<8;i++) r+=no[i]; return r; }
int main(void){ printf("neon_widen r=%ld\n", go()); return 0; }
