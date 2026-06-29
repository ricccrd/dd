// ordered/unordered float comparisons and the boolean lowering of each predicate.
#include <stdio.h>
#include <math.h>
int main(void){
  double v[]={-1.0,0.0,1.0,INFINITY,-INFINITY,NAN,3.14,-2.71};
  int lt=0,le=0,eq=0,ge=0,gt=0,ne=0;
  for(int i=0;i<8;i++)for(int j=0;j<8;j++){
    lt+=v[i]<v[j]; le+=v[i]<=v[j]; eq+=v[i]==v[j]; ge+=v[i]>=v[j]; gt+=v[i]>v[j]; ne+=v[i]!=v[j];
  }
  printf("lt=%d le=%d eq=%d ge=%d gt=%d ne=%d\n", lt,le,eq,ge,gt,ne);
  return 0;
}
