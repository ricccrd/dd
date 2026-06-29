#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
int main(void){
  const char *a="/tmp/ddc_cfr_a",*b="/tmp/ddc_cfr_b"; unlink(a); unlink(b);
  int fa=open(a,O_CREAT|O_RDWR|O_TRUNC,0644); write(fa,"hello world",11); lseek(fa,0,SEEK_SET);
  int fb=open(b,O_CREAT|O_RDWR|O_TRUNC,0644);
  long n=syscall(SYS_copy_file_range, fa,(void*)0, fb,(void*)0, 11, 0);
  char buf[16]={0}; lseek(fb,0,SEEK_SET); int g=read(fb,buf,15); (void)g;
  printf("copy_file_range n=%ld data=%s\n", n, buf); close(fa); close(fb); unlink(a); unlink(b); return 0; }
