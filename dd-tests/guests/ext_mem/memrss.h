/* Shared RSS sampler for leak tests. Prefers /proc/self/statm (current resident, Linux + the JIT's
   emulated procfs); falls back to getrusage(RUSAGE_SELF).ru_maxrss high-water (macOS native). Both
   detect a per-iteration leak: a leak makes resident climb monotonically, so final-baseline blows past
   the threshold; a clean release keeps it near baseline. Verdict is golden (bounded=1), RSS is not. */
#ifndef MEMRSS_H
#define MEMRSS_H
#include <stdio.h>
#include <unistd.h>
#include <sys/resource.h>
static long rss_kb(void){
  FILE*f=fopen("/proc/self/statm","r");
  if(f){ long sz=0,res=0; int n=fscanf(f,"%ld %ld",&sz,&res); fclose(f);
    if(n==2){ long pg=sysconf(_SC_PAGESIZE)/1024; return res*(pg>0?pg:4); } }
  struct rusage ru;
  if(getrusage(RUSAGE_SELF,&ru)==0){
#ifdef __APPLE__
    return ru.ru_maxrss/1024;   /* bytes -> KB */
#else
    return ru.ru_maxrss;        /* already KB on Linux */
#endif
  }
  return 0;
}
/* prints "<name> bounded=1" to stdout when growth < thresh_kb (golden), grew=<kb> to stderr (debug). */
static int verdict(const char*name,long base,long fin,long thresh_kb){
  long grew=fin-base; if(grew<0) grew=0;
  fprintf(stderr,"%s base=%ldKB fin=%ldKB grew=%ldKB thresh=%ldKB\n",name,base,fin,grew,thresh_kb);
  printf("%s bounded=%d\n",name,grew<thresh_kb?1:0);
  return 0;
}
#endif
