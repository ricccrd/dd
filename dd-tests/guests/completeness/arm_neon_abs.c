#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static long go(void){
  int32_t va[4]={-5,6,-7,8}; int32x4_t a=vld1q_s32(va);
  int32x4_t ab=vabsq_s32(a), ng=vnegq_s32(a);
  int32_t vb[4]={1,10,3,20}; int32x4_t b=vld1q_s32(vb);
  int32x4_t adf=vabdq_s32(a,b);
  long r=0; int32_t o[4];
  vst1q_s32(o,ab); for(int i=0;i<4;i++) r+=o[i];
  vst1q_s32(o,ng); for(int i=0;i<4;i++) r+=o[i];
  vst1q_s32(o,adf); for(int i=0;i<4;i++) r+=o[i]; return r; }
int main(void){ printf("neon_abs r=%ld\n", go()); return 0; }
