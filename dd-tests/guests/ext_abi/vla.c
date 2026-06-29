// C99 variable-length arrays (incl. a 2-D VLA) — runtime-sized frames and index arithmetic.
#include <stdio.h>
static long fill(int r,int c){ long a[r][c]; for(int i=0;i<r;i++)for(int j=0;j<c;j++) a[i][j]=(i+1)*(j+1); long s=0; for(int i=0;i<r;i++)for(int j=0;j<c;j++) s+=a[i][j]; return s; }
int main(void){
  long acc=0;
  for(int n=1;n<=40;n++){ int v[n]; for(int i=0;i<n;i++) v[i]=i*i-i; for(int i=0;i<n;i++) acc+=v[i]; acc+=fill(n%7+1,n%5+1); }
  printf("acc=%ld\n", acc);
  return 0;
}
