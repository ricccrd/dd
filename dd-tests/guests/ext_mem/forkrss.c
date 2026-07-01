#include "memrss.h"
#include <sys/wait.h>
#include <stdlib.h>
int main(void){
  for(int k=0;k<8;k++){pid_t p=fork();if(p==0)_exit(0);int st;waitpid(p,&st,0);}
  long base=rss_kb();
  for(int i=0;i<400;i++){pid_t p=fork();if(p==0)_exit(0);if(p<0){printf("forkrss bounded=0\n");return 1;}int st;waitpid(p,&st,0);}
  verdict("forkrss",base,rss_kb(),150*1024); return 0; }
