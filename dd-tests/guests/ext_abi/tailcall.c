// self/mutual tail recursion at depth (must not blow the stack if tail-call optimized; correct either way).
#include <stdio.h>
static long sum_to(long n,long acc){ return n==0?acc:sum_to(n-1,acc+n); }
static int is_even(unsigned n); static int is_odd(unsigned n){ return n==0?0:is_even(n-1); }
static int is_even(unsigned n){ return n==0?1:is_odd(n-1); }
int main(void){
  long s=sum_to(1000000,0);
  int e=is_even(99999), o=is_odd(99999);
  printf("s=%ld even=%d odd=%d\n", s, e, o);
  return 0;
}
