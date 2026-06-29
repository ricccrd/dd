#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
int main(void){
  static char stk[16384];
  stack_t ss; memset(&ss,0,sizeof ss); ss.ss_sp=stk; ss.ss_size=sizeof stk; ss.ss_flags=0;
  long s=syscall(SYS_sigaltstack, &ss, (void*)0);
  stack_t old; memset(&old,0,sizeof old);
  long g=syscall(SYS_sigaltstack, (void*)0, &old);
  printf("sigaltstack set=%d sz_ok=%d\n", s==0, (g==0 && old.ss_size==(size_t)sizeof stk)); return 0; }
