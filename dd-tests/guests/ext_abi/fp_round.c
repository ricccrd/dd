// rounding-mode control (fesetround) + rint/nearbyint/round/trunc/ceil/floor.
#include <stdio.h>
#include <math.h>
#include <fenv.h>
#pragma STDC FENV_ACCESS ON
int main(void){
  double xs[]={2.5,-2.5,0.5,-0.5,3.49,3.5,-3.5,1.9999999};
  long acc=0;
  int modes[]={FE_TONEAREST,FE_UPWARD,FE_DOWNWARD,FE_TOWARDZERO};
  for(int m=0;m<4;m++){ fesetround(modes[m]); for(unsigned i=0;i<8;i++) acc += (long)(lrint(xs[i])*(m+1)); }
  fesetround(FE_TONEAREST);
  printf("acc=%ld rnd=%.0f trunc=%.0f ceil=%.0f floor=%.0f nint=%.0f\n",
    acc, round(2.5), trunc(-2.7), ceil(-2.1), floor(2.9), nearbyint(0.5));
  return 0;
}
