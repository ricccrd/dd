#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
int main(void){
  const char *p="/tmp/ddc_trc"; unlink(p);
  int fd=open(p,O_CREAT|O_RDWR|O_TRUNC,0644);
  for(int i=0;i<100;i++) write(fd,"x",1);
  long r1=syscall(SYS_ftruncate, fd, (long)10);
  close(fd);
  long r2=syscall(SYS_truncate, p, (long)20);
  struct stat st; stat(p,&st);
  printf("truncate ftr=%d tr=%d size=%lld\n", r1==0, r2==0, (long long)st.st_size); unlink(p); return 0; }
