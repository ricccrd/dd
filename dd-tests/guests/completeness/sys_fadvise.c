#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
int main(void){
  const char *p="/tmp/ddc_fad"; unlink(p);
  int fd=open(p,O_CREAT|O_RDWR|O_TRUNC,0644);
  for(int i=0;i<1024;i++) write(fd,"x",1);
  int w=posix_fadvise(fd,0,0,POSIX_FADV_WILLNEED);
  int s=posix_fadvise(fd,0,0,POSIX_FADV_SEQUENTIAL);
  int d=posix_fadvise(fd,0,0,POSIX_FADV_DONTNEED);
  printf("fadvise willneed=%d seq=%d dontneed=%d\n", w==0, s==0, d==0); close(fd); unlink(p); return 0; }
