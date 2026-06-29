// comparison results feeding conditional selects (csel/setcc lowering).
#include <stdio.h>
int main(void){
  long acc=0;
  for(int i=-500;i<500;i++){
    long x=i*7919L;
    acc += (x<0) ? -x : x;                       // abs via select
    acc += (x>1000) ? 1 : (x< -1000 ? -1 : 0);   // sign-ish bucket
    acc += (x==0) ? 999 : 0;
    int a=i, b=1000-i;
    acc += (a<b)?a:b;                            // min
    acc += (a>b)?a:b;                            // max
  }
  printf("acc=%ld\n", acc);
  return 0;
}
