// struct returns of varying size: register-pair return vs hidden-sret-pointer return.
#include <stdio.h>
typedef struct { int a, b; } Small;
typedef struct { long a, b, c, d, e; } Wide;
static Small mk_small(int i){ return (Small){i*3, i*3+1}; }
static Wide mk_wide(long i){ return (Wide){i, i*2, i*3, i*4, i*5}; }
int main(void){
  long s=0;
  for(int i=0;i<10000;i++){ Small sm=mk_small(i); Wide w=mk_wide(i); s += sm.a+sm.b + w.a+w.e; }
  printf("s=%ld\n", s);
  return 0;
}
