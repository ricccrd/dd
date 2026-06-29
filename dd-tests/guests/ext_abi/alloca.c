// alloca() dynamic stack allocation in a loop — stack pointer juggling under the JIT.
#include <stdio.h>
#include <alloca.h>
static long work(int n){ volatile long* p=alloca(n*sizeof(long)); for(int i=0;i<n;i++) p[i]=(long)i*i; long s=0; for(int i=0;i<n;i++) s+=p[i]; return s; }
int main(void){
  long acc=0;
  for(int i=1;i<=500;i++) acc += work((i%64)+1);
  printf("acc=%ld\n", acc);
  return 0;
}
