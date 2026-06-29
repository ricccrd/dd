#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
__attribute__((target("+dotprod"))) static long go(void){
  int8_t va[16],vb[16]; for(int i=0;i<16;i++){va[i]=i-8; vb[i]=(i&3)+1;}
  int8x16_t a=vld1q_s8(va), b=vld1q_s8(vb);
  int32x4_t acc=vdupq_n_s32(0); acc=vdotq_s32(acc,a,b);
  uint8_t ua[16],ub[16]; for(int i=0;i<16;i++){ua[i]=i+1; ub[i]=2;}
  uint32x4_t uacc=vdupq_n_u32(0); uacc=vdotq_u32(uacc,vld1q_u8(ua),vld1q_u8(ub));
  long r=0; int32_t o[4]; uint32_t uo[4];
  vst1q_s32(o,acc); for(int i=0;i<4;i++) r+=o[i];
  vst1q_u32(uo,uacc); for(int i=0;i<4;i++) r+=uo[i]; return r; }
int main(void){ printf("dotprod r=%ld\n", go()); return 0; }
