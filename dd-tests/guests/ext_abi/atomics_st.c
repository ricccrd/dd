// __atomic builtins (single-thread, deterministic): fetch_add/sub/or/and/xor + CAS + exchange.
#include <stdio.h>
int main(void){
  long v=0; unsigned long bits=0; long cas_ok=0;
  for(int i=0;i<100000;i++){
    __atomic_fetch_add(&v,3,__ATOMIC_SEQ_CST);
    __atomic_fetch_sub(&v,1,__ATOMIC_RELAXED);
    __atomic_fetch_or(&bits,(unsigned long)1<<(i&63),__ATOMIC_SEQ_CST);
    __atomic_fetch_xor(&bits,(unsigned long)1<<((i*7)&63),__ATOMIC_ACQ_REL);
    long exp=v; if(__atomic_compare_exchange_n(&v,&exp,exp+5,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST)) cas_ok++;
  }
  long old=__atomic_exchange_n(&v,42,__ATOMIC_SEQ_CST);
  printf("v=%ld bits=%lx cas=%ld old=%ld\n", v, bits, cas_ok, old);
  return 0;
}
