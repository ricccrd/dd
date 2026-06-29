#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
int main(void){
  const char *p="/tmp/ddc_fal"; unlink(p);
  int fd=open(p,O_CREAT|O_RDWR|O_TRUNC,0644);
  long r=syscall(SYS_fallocate, fd, 0, (long)0, (long)8192);
  struct stat st; fstat(fd,&st);
  printf("fallocate ok=%d size=%lld\n", r==0, (long long)st.st_size); close(fd); unlink(p); return 0; }
