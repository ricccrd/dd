#include <stdio.h>
#include <stdlib.h>
static int cmp(const void*a,const void*b){ return *(const int*)a-*(const int*)b; }
int main(void){ int a[24]; unsigned s=12345; for(int i=0;i<24;i++){s=s*1103515245u+12345u;a[i]=(s>>16)%1000;}
  qsort(a,24,sizeof(int),cmp); for(int i=0;i<24;i++)printf("%d ",a[i]); printf("\n"); return 0; }
