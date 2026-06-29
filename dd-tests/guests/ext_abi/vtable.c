// C-style vtable dispatch through structs of function pointers (IBTC / inline-cache stress).
#include <stdio.h>
typedef struct Shape Shape;
struct Shape { long (*area)(const Shape*); long (*peri)(const Shape*); long a, b; };
static long r_area(const Shape*s){return s->a*s->b;}  static long r_peri(const Shape*s){return 2*(s->a+s->b);}
static long s_area(const Shape*s){return s->a*s->a;}  static long s_peri(const Shape*s){return 4*s->a;}
int main(void){
  struct Shape rect={r_area,r_peri,7,3}, sq={s_area,s_peri,5,0};
  Shape* shapes[2]={&rect,&sq};
  long acc=0;
  for(int i=0;i<50000;i++){ Shape*s=shapes[i&1]; acc += s->area(s) + s->peri(s); }
  printf("acc=%ld\n", acc);
  return 0;
}
