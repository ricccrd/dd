#include <stdio.h>
#include <stdlib.h>
static int cmp(const void*a,const void*b){ long x=*(const long*)a,y=*(const long*)b; return (x>y)-(x<y); }
int main(void){ int n=300000; long*a=malloc(n*sizeof(long)); unsigned long s=88172645463325252UL;
  for(int i=0;i<n;i++){s^=s<<13;s^=s>>7;s^=s<<17;a[i]=(long)(s%1000000);}
  qsort(a,n,sizeof(long),cmp); long chk=0; for(int i=0;i<n;i++)chk=chk*31+a[i];
  printf("sortbig chk=%ld first=%ld last=%ld\n",chk,a[0],a[n-1]); free(a); return 0; }
