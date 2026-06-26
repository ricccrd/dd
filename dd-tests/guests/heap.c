#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){ int n=2000; char**p=malloc(n*sizeof(char*));
  for(int i=0;i<n;i++){p[i]=malloc(48);memset(p[i],'x',47);p[i][47]=0;}
  long s=0; for(int i=0;i<n;i++)s+=strlen(p[i]); for(int i=0;i<n;i++)free(p[i]);
  char*q=realloc(NULL,32); strcpy(q,"realloc-ok"); printf("heap sum=%ld q=%s\n",s,q); free(q); free(p); return 0; }
