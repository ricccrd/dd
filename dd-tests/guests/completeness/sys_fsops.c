#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
int main(void){
  const char *a="/tmp/ddc_fa",*b="/tmp/ddc_fb",*s="/tmp/ddc_fs",*d="/tmp/ddc_fd";
  unlink(a);unlink(b);unlink(s); rmdir(d);
  int fd=open(a,O_CREAT|O_WRONLY,0644); close(fd);
  long l=syscall(SYS_linkat, AT_FDCWD,a, AT_FDCWD,b, 0);
  long sy=syscall(SYS_symlinkat, "ddc_fa", AT_FDCWD, s);
  long mk=syscall(SYS_mkdirat, AT_FDCWD, d, 0755);
  struct stat st; stat(a,&st); int nl=st.st_nlink;
  long un=syscall(SYS_unlinkat, AT_FDCWD, b, 0);
  printf("fsops link=%d nlink=%d sym=%d mkdir=%d unlink=%d\n", l==0,nl,sy==0,mk==0,un==0);
  unlink(a);unlink(s); rmdir(d); return 0; }
