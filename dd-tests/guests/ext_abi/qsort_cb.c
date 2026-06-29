// qsort with a comparator callback (indirect call into guest code from libc) + bsearch.
#include <stdio.h>
#include <stdlib.h>
static int cmp(const void*a,const void*b){ long x=*(const long*)a, y=*(const long*)b; return (x>y)-(x<y); }
int main(void){
  enum{N=5000}; static long v[N];
  unsigned long st=0x243F6A88;
  for(int i=0;i<N;i++){ st=st*6364136223846793005UL+1442695040888963407UL; v[i]=(long)(st>>33); }
  qsort(v,N,sizeof(long),cmp);
  int sorted=1; for(int i=1;i<N;i++) if(v[i]<v[i-1]) sorted=0;
  long key=v[N/2]; long* hit=bsearch(&key,v,N,sizeof(long),cmp);
  printf("sorted=%d first=%ld last=%ld found=%d\n", sorted, v[0], v[N-1], hit!=NULL);
  return 0;
}
