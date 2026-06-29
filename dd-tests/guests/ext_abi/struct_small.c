// small structs (<=16 bytes) passed & returned by value in registers per SysV/AAPCS.
#include <stdio.h>
typedef struct { int x, y; } P;
typedef struct { long a, b; } Q;
static P padd(P u,P v){ return (P){u.x+v.x,u.y+v.y}; }
static Q qmul(Q u,Q v){ return (Q){u.a*v.a,u.b*v.b}; }
static long psum(P p){ return (long)p.x+p.y; }
int main(void){
  P p={0,0}; Q q={1,1}; long acc=0;
  for(int i=0;i<10000;i++){ p=padd(p,(P){i,2*i}); q=qmul(q,(Q){1,2}); if(i<20) acc+=psum(p); }
  printf("px=%d py=%d qa=%ld qb=%ld acc=%ld\n", p.x, p.y, q.a, q.b, acc);
  return 0;
}
