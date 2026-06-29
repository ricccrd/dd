#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
int main(void){
  int a=open("/dev/null",O_RDONLY), b=open("/dev/null",O_RDONLY), c=open("/dev/null",O_RDONLY);
  (void)a;
  long r=syscall(__NR_close_range, (unsigned)(b<c?b:c), (unsigned)(b>c?b:c), 0);
  int closed = (fcntl(b,F_GETFD)==-1 && fcntl(c,F_GETFD)==-1);
  printf("close_range ok=%d closed=%d\n", r==0, r==0?closed:0); return 0; }
