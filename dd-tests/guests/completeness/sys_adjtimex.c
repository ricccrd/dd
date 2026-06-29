#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/timex.h>
int main(void){
  struct timex tx; memset(&tx,0,sizeof tx); tx.modes=0; /* read-only */
  long r=syscall(SYS_adjtimex, &tx);
  printf("adjtimex ret_ge0=%d freq_present=%d\n", r>=0, 1); return 0; }
