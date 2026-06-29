// mixed int+float struct fields by value — SysV splits classes across INTEGER/SSE registers.
#include <stdio.h>
typedef struct { int id; double w; int n; } Item;
typedef struct { double a; long b; } Pair;
static double weigh(Item it){ return it.w*it.n + it.id; }
static Pair combine(Pair p,Item it){ p.a += it.w; p.b += it.id+it.n; return p; }
int main(void){
  Pair p={0,0}; double acc=0;
  for(int i=1;i<=2000;i++){ Item it={i, i*0.25, i%7}; acc += weigh(it); p=combine(p,it); }
  printf("acc=%.2f pa=%.2f pb=%ld\n", acc, p.a, p.b);
  return 0;
}
