// boolean materialization: !!x, logical &&/||, _Bool narrowing, short-circuit side effects.
#include <stdio.h>
#include <stdbool.h>
static int calls=0; static int eff(int v){ calls++; return v; }
int main(void){
  long acc=0;
  for(int i=-200;i<200;i++){
    bool b=!!i;
    acc += b;
    acc += (i>0 && i<100) ? 1 : 0;
    acc += (i<0 || i>150) ? 2 : 0;
    acc += (eff(i) && eff(i+1)) ? 5 : 0;     // short-circuit: second eff skipped when i==0
    _Bool t=(_Bool)(i*2);
    acc += t;
  }
  printf("acc=%ld calls=%d\n", acc, calls);
  return 0;
}
