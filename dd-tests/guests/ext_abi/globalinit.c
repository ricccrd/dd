// static/global aggregate initializers, const tables, designated initializers in .data/.rodata.
#include <stdio.h>
static const int primes[]={2,3,5,7,11,13,17,19,23,29};
static const char* names[]={"zero","one","two","three"};
static struct { int k; long v; } tbl[]={ {1,100},{2,200},{3,300},[5]={9,500} };
static long counters[256];
int main(void){
  long acc=0;
  for(int i=0;i<10;i++) acc += primes[i]*(i+1);
  for(unsigned i=0;i<sizeof(tbl)/sizeof(tbl[0]);i++) acc += tbl[i].k*1000 + tbl[i].v;
  for(int i=0;i<256;i++){ counters[i]=i*i; acc += counters[i]&0xFF; }
  printf("acc=%ld n0=%s n3=%s\n", acc, names[0], names[3]);
  return 0;
}
