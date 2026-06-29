#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <arm_neon.h>
__attribute__((target("+i8mm"))) static long go(void){
  int8_t va[16],vb[16]; for(int i=0;i<16;i++){va[i]=(i&7)-3; vb[i]=(i&3)+1;}
  int8x16_t a=vld1q_s8(va), b=vld1q_s8(vb);
  int32x4_t acc=vdupq_n_s32(0); acc=vmmlaq_s32(acc,a,b);
  long r=0; int32_t o[4]; vst1q_s32(o,acc); for(int i=0;i<4;i++) r+=o[i]; return r; }
int main(void){ printf("i8mm r=%ld\n", go()); return 0; }
