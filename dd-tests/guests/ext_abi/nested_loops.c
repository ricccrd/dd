// deeply nested loops with break/continue + loop-carried dependencies (block-chaining stress).
#include <stdio.h>
int main(void){
  long acc=0;
  for(int a=0;a<40;a++)
    for(int b=0;b<40;b++){
      if((a+b)%7==0) continue;
      for(int c=0;c<40;c++){
        if(a*b*c>50000) break;
        acc += (a^b)+(b&c)-(c|a);
      }
    }
  printf("acc=%ld\n", acc);
  return 0;
}
