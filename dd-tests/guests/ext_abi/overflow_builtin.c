// __builtin_{add,sub,mul}_overflow flag detection across signed/unsigned widths.
#include <stdio.h>
#include <limits.h>
int main(void){
  int r; long lr; unsigned ur;
  int o1=__builtin_add_overflow(INT_MAX,1,&r);
  int o2=__builtin_sub_overflow(INT_MIN,1,&r);
  int o3=__builtin_mul_overflow(65536,65536,&r);
  int o4=__builtin_add_overflow(2000000000L,2000000000L,&lr);   // fits in long
  int o5=__builtin_add_overflow(UINT_MAX,1u,&ur);
  int o6=__builtin_sub_overflow(0u,1u,&ur);
  int sum=o1+o2+o3+o4+o5+o6;
  printf("o1=%d o2=%d o3=%d o4=%d o5=%d o6=%d sum=%d lr=%ld\n", o1,o2,o3,o4,o5,o6,sum,lr);
  return 0;
}
