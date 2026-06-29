// deep non-tail recursion (real stack frames) + Ackermann depth gate.
#include <stdio.h>
static long depth(long n){ if(n==0) return 0; return 1+depth(n-1); }
static long ack(long m,long n){ return m==0?n+1:(n==0?ack(m-1,1):ack(m-1,ack(m,n-1))); }
static long tree(int d){ if(d==0) return 1; return tree(d-1)+tree(d-1)+d; }
int main(void){
  printf("depth=%ld ack=%ld tree=%ld\n", depth(50000), ack(3,6), tree(20));
  return 0;
}
