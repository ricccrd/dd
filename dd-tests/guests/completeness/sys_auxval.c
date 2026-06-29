#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/auxv.h>
int main(void){
  unsigned long ps=getauxval(AT_PAGESZ), ck=getauxval(AT_CLKTCK), hw=getauxval(AT_HWCAP);
  printf("auxval pagesz=%lu clktck=%lu hwcap_nz=%d\n", ps, ck, hw!=0); return 0; }
