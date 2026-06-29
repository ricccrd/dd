#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
int main(void){
  struct rusage ru; memset(&ru,0,sizeof ru);
  long r=syscall(SYS_getrusage, RUSAGE_SELF, &ru);
  printf("getrusage ok=%d maxrss_pos=%d\n", r==0, ru.ru_maxrss>0); return 0; }
