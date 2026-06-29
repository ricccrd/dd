// homogeneous float aggregates — AAPCS passes these in 2-4 FP regs; SysV uses SSE. Per-arch oracle.
#include <stdio.h>
typedef struct { float x, y; } V2;
typedef struct { double a, b, c, d; } V4;
static V2 v2add(V2 p,V2 q){ return (V2){p.x+q.x,p.y+q.y}; }
static double v4dot(V4 p,V4 q){ return p.a*q.a+p.b*q.b+p.c*q.c+p.d*q.d; }
int main(void){
  V2 p={0,0}; double acc=0;
  for(int i=0;i<3000;i++){ p=v2add(p,(V2){i*0.5f, i*0.25f}); V4 a={i,i+1,i+2,i+3}, b={1,2,3,4}; acc+=v4dot(a,b); }
  printf("px=%.1f py=%.1f acc=%.1f\n", (double)p.x, (double)p.y, acc);
  return 0;
}
