#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
int main(void){
  const char *p="/tmp/ddc_chm"; unlink(p);
  int fd=open(p,O_CREAT|O_WRONLY,0644); close(fd);
  long r=syscall(SYS_fchmodat, AT_FDCWD, p, 0640, 0);
  struct stat st; stat(p,&st);
  printf("fchmodat ok=%d mode=%o\n", r==0, st.st_mode&0777); unlink(p); return 0; }
