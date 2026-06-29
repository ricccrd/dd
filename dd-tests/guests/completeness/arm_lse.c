#include <stdio.h>
#include <stdatomic.h>
__attribute__((target("arch=armv8.1-a"))) static long go(void){
  _Atomic long v=0; long r=0;
  r+=atomic_fetch_add(&v,5);          /* LDADD */
  r+=atomic_exchange(&v,9);           /* SWP */
  long e=9; atomic_compare_exchange_strong(&v,&e,42); /* CAS */
  r+=atomic_fetch_or(&v,0x100);       /* LDSET */
  r+=atomic_fetch_and(&v,0x1ff);      /* LDCLR */
  r+=atomic_load(&v);
  return r; }
int main(void){ printf("lse r=%ld\n", go()); return 0; }
