#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
static long go(void){
  int32_t va[4]={1,2,3,4},vb[4]={5,6,7,8}; int32x4_t a=vld1q_s32(va),b=vld1q_s32(vb);
  int32x4_t z=vzip1q_s32(a,b), u=vuzp1q_s32(a,b), t=vtrn1q_s32(a,b), r=vrev64q_s32(a);
  long acc=0; int32_t o[4];
  vst1q_s32(o,z); for(int i=0;i<4;i++) acc+=o[i]*(i+1);
  vst1q_s32(o,u); for(int i=0;i<4;i++) acc+=o[i]*(i+1);
  vst1q_s32(o,t); for(int i=0;i<4;i++) acc+=o[i]*(i+1);
  vst1q_s32(o,r); for(int i=0;i<4;i++) acc+=o[i]*(i+1); return acc; }
int main(void){ printf("neon_perm r=%ld\n", go()); return 0; }
