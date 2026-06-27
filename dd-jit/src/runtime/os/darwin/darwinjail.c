// darwinjail -- run REAL native macOS (arm64) binaries in a container, no VM, no DBT.
//
// Injected via DYLD_INSERT_LIBRARIES; interposes libSystem's path/host/net calls and rewrites them into
// the container (rootfs upper + overlay lowers + bind volumes), plus Seatbelt write-confinement and
// rlimits (cgroup analog). The guest binaries execute natively -- so dynamic linking, the dyld shared
// cache, etc. all "just work" -- they just see a jailed filesystem. Same container model as os/linux.
// (Only plain-arm64, non-SIP binaries are injectable; the userland is a nix/custom arm64 toolchain.)
//
// Config via env: DD_ROOTFS, DD_LOWERS="a,b", DD_VOLUMES="HOST:CONT,…", DD_HOSTNAME, DD_PUBLISH="H:C,…",
//                 DD_MEM_MAX, DD_PIDS_MAX, DD_SANDBOX=1, DD_NET_ISOLATE=1, DD_PID1=1.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <dirent.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/mount.h>
#include <netinet/in.h>
#include <unistd.h>
extern int sandbox_init(const char *profile, uint64_t flags, char **errorbuf);

static const char *g_rootfs, *g_hostname; static int g_pid1;
static char *g_low[8]; static int g_nlow;
static struct { char *host, *cont; } g_vol[16]; static int g_nvol;
static struct { int host, cont; } g_pub[16]; static int g_npub;

static void split(char *v, void (*f)(char*)){ if(!v) return; char *s=strdup(v),*t,*sp=0;
    for(t=strtok_r(s,",",&sp); t; t=strtok_r(0,",",&sp)) f(t); }
static void add_low(char *d){ if(g_nlow<8) g_low[g_nlow++]=strdup(d); }
static void add_vol(char *hc){ char *c=strchr(hc,':'); if(c&&g_nvol<16){ *c=0; g_vol[g_nvol].host=strdup(hc); g_vol[g_nvol].cont=strdup(c+1); g_nvol++; } }
static void add_pub(char *hc){ char *c=strchr(hc,':'); if(c&&g_npub<16){ *c=0; g_pub[g_npub].host=atoi(hc); g_pub[g_npub].cont=atoi(c+1); g_npub++; } }

static const char *jail(const char *p, char *out); // defined below; used by init for DD_CWD

__attribute__((constructor)) static void init(void){
    g_rootfs = getenv("DD_ROOTFS"); g_hostname = getenv("DD_HOSTNAME"); g_pid1 = getenv("DD_PID1")!=0;
    split(getenv("DD_LOWERS"), add_low); split(getenv("DD_VOLUMES"), add_vol); split(getenv("DD_PUBLISH"), add_pub);
    // initial working directory (docker -w / the cwd ddcli mounts): chdir into the container path.
    const char *cwd = getenv("DD_CWD");
    if(cwd && cwd[0] && g_rootfs){ char b[1024]; chdir(jail(cwd, b)); }
    char *mm=getenv("DD_MEM_MAX"), *pm=getenv("DD_PIDS_MAX");
    if(mm){ struct rlimit r={strtoull(mm,0,10),strtoull(mm,0,10)}; setrlimit(RLIMIT_AS,&r); }
    if(pm){ struct rlimit r={strtoull(pm,0,10),strtoull(pm,0,10)}; setrlimit(RLIMIT_NPROC,&r); }
    if(getenv("DD_SANDBOX") && g_rootfs){
        char prof[4096]; int n=snprintf(prof,sizeof prof,
            "(version 1)(allow default)(deny file-write* (subpath \"/\"))"
            "(allow file-write* (subpath \"%s\"))"
            "(allow file-write* (subpath \"/private/tmp\") (subpath \"/private/var/folders\") (subpath \"/dev\"))",
            g_rootfs);
        for(int i=0;i<g_nvol && n<3000;i++) n+=snprintf(prof+n,sizeof prof-n,"(allow file-write* (subpath \"%s\"))",g_vol[i].host);
        if(getenv("DD_NET_ISOLATE")) n+=snprintf(prof+n,sizeof prof-n,
            "(deny network-outbound (remote ip \"*:*\"))(allow network-outbound (remote ip \"localhost:*\"))");
        char *err=0; if(sandbox_init(prof,0,&err)) fprintf(stderr,"[darwinjail] sandbox: %s\n",err?err:"?");
    }
}
// container path -> host path: bind volumes, then overlay (upper wins, else a lower, else upper for creates).
static const char *jail(const char *p, char *out){
    if(!p || p[0]!='/' || !g_rootfs) return p;
    for(int i=0;i<g_nvol;i++){ size_t L=strlen(g_vol[i].cont);
        if(!strncmp(p,g_vol[i].cont,L) && (p[L]=='/'||p[L]==0)){ snprintf(out,1024,"%s%s",g_vol[i].host,p+L); return out; } }
    snprintf(out,1024,"%s%s",g_rootfs,p);
    if(access(out,F_OK)==0) return out;
    for(int i=0;i<g_nlow;i++){ char t[1024]; snprintf(t,1024,"%s%s",g_low[i],p);
        if(access(t,F_OK)==0){ snprintf(out,1024,"%s",t); return out; } }
    return out;
}
#define JAIL(p) ({ static __thread char _b[1024]; jail((p),_b); })
// for two-path syscalls: jail both into distinct thread-local buffers.
static const char *jail2(const char *p, char *out){ return jail(p, out); }
#define JAIL_A(p) ({ static __thread char _a[1024]; jail2((p),_a); })

int jail_open(const char *path, int flags, ...){
    mode_t m=0; if(flags & O_CREAT){ va_list ap; va_start(ap,flags); m=(mode_t)va_arg(ap,int); va_end(ap); }
    return open(JAIL(path), flags, m);
}
int jail_openat(int fd,const char*path,int flags,...){
    mode_t m=0; if(flags&O_CREAT){ va_list ap; va_start(ap,flags); m=(mode_t)va_arg(ap,int); va_end(ap); }
    return openat(fd, path&&path[0]=='/'?JAIL(path):path, flags, m);
}
int   jail_stat (const char*p, struct stat*s){ return stat (JAIL(p), s); }
int   jail_lstat(const char*p, struct stat*s){ return lstat(JAIL(p), s); }
int   jail_fstatat(int fd,const char*p,struct stat*s,int f){ return fstatat(fd, p&&p[0]=='/'?JAIL(p):p, s, f); }
int   jail_access(const char*p,int m){ return access(JAIL(p), m); }
int   jail_faccessat(int fd,const char*p,int m,int f){ return faccessat(fd, p&&p[0]=='/'?JAIL(p):p, m, f); }
ssize_t jail_readlink(const char*p,char*b,size_t n){ return readlink(JAIL(p), b, n); }
ssize_t jail_readlinkat(int fd,const char*p,char*b,size_t n){ return readlinkat(fd, p&&p[0]=='/'?JAIL(p):p, b, n); }
int   jail_unlink(const char*p){ return unlink(JAIL(p)); }
int   jail_unlinkat(int fd,const char*p,int f){ return unlinkat(fd, p&&p[0]=='/'?JAIL(p):p, f); }
int   jail_mkdir(const char*p,mode_t m){ return mkdir(JAIL(p), m); }
int   jail_mkdirat(int fd,const char*p,mode_t m){ return mkdirat(fd, p&&p[0]=='/'?JAIL(p):p, m); }
int   jail_rmdir(const char*p){ return rmdir(JAIL(p)); }
int   jail_chdir(const char*p){ return chdir(JAIL(p)); }
int   jail_chmod(const char*p,mode_t m){ return chmod(JAIL(p), m); }
int   jail_chown(const char*p,uid_t u,gid_t g){ return chown(JAIL(p), u, g); }
int   jail_lchown(const char*p,uid_t u,gid_t g){ return lchown(JAIL(p), u, g); }
int   jail_statfs(const char*p,struct statfs*s){ return statfs(JAIL(p), s); }
int   jail_utimes(const char*p,const struct timeval t[2]){ return utimes(JAIL(p), t); }
int   jail_rename(const char*a,const char*b){ char x[1024]; snprintf(x,sizeof x,"%s",JAIL_A(a)); return rename(x, JAIL(b)); }
int   jail_link  (const char*a,const char*b){ char x[1024]; snprintf(x,sizeof x,"%s",JAIL_A(a)); return link  (x, JAIL(b)); }
int   jail_symlink(const char*t,const char*l){ return symlink(t, JAIL(l)); } // target is stored verbatim
DIR  *jail_opendir(const char*p){ return opendir(JAIL(p)); }
FILE *jail_fopen(const char*p,const char*m){ char b[1024]; return fopen(jail(p,b), m); }
FILE *jail_freopen(const char*p,const char*m,FILE*s){ char b[1024]; return freopen(jail(p,b), m, s); }
int jail_gethostname(char*name,size_t len){ if(g_hostname){ strlcpy(name,g_hostname,len); return 0; } return gethostname(name,len); }
pid_t jail_getpid(void){ return g_pid1 ? 1 : getpid(); }
int jail_bind(int s,const struct sockaddr*a,socklen_t l){
    if(a && a->sa_family==AF_INET){ struct sockaddr_in in=*(struct sockaddr_in*)a; int p=ntohs(in.sin_port);
        for(int i=0;i<g_npub;i++) if(g_pub[i].cont==p){ in.sin_port=htons(g_pub[i].host); return bind(s,(struct sockaddr*)&in,l); } }
    return bind(s,a,l);
}
// exec: jail the program path so a container PATH / a container-local binary resolves into the rootfs.
// The child inherits DYLD_INSERT_LIBRARIES (env), so the jail re-arms in the new process.
int jail_execve(const char*p,char*const a[],char*const e[]){ return execve(JAIL(p), a, e); }
int jail_posix_spawn (pid_t*pid,const char*p,const posix_spawn_file_actions_t*fa,const posix_spawnattr_t*at,char*const a[],char*const e[]){
    return posix_spawn (pid, JAIL(p), fa, at, a, e); }
int jail_posix_spawnp(pid_t*pid,const char*p,const posix_spawn_file_actions_t*fa,const posix_spawnattr_t*at,char*const a[],char*const e[]){
    return posix_spawnp(pid, p, fa, at, a, e); } // p is a name; PATH search uses interposed access()

#define INTERPOSE(repl,orig) __attribute__((used)) static struct { const void*r; const void*o; } \
  _ip_##orig __attribute__((section("__DATA,__interpose"))) = { (const void*)repl, (const void*)orig };
INTERPOSE(jail_open, open)         INTERPOSE(jail_openat, openat)      INTERPOSE(jail_stat, stat)
INTERPOSE(jail_lstat, lstat)       INTERPOSE(jail_fstatat, fstatat)    INTERPOSE(jail_access, access)
INTERPOSE(jail_faccessat, faccessat) INTERPOSE(jail_readlink, readlink) INTERPOSE(jail_readlinkat, readlinkat)
INTERPOSE(jail_unlink, unlink)     INTERPOSE(jail_unlinkat, unlinkat)  INTERPOSE(jail_mkdir, mkdir)
INTERPOSE(jail_mkdirat, mkdirat)   INTERPOSE(jail_rmdir, rmdir)        INTERPOSE(jail_chdir, chdir)
INTERPOSE(jail_chmod, chmod)       INTERPOSE(jail_chown, chown)        INTERPOSE(jail_lchown, lchown)
INTERPOSE(jail_statfs, statfs)     INTERPOSE(jail_utimes, utimes)      INTERPOSE(jail_rename, rename)
INTERPOSE(jail_link, link)         INTERPOSE(jail_symlink, symlink)    INTERPOSE(jail_opendir, opendir)
INTERPOSE(jail_fopen, fopen)       INTERPOSE(jail_freopen, freopen)
INTERPOSE(jail_gethostname, gethostname) INTERPOSE(jail_bind, bind)    INTERPOSE(jail_getpid, getpid)
INTERPOSE(jail_execve, execve)     INTERPOSE(jail_posix_spawn, posix_spawn) INTERPOSE(jail_posix_spawnp, posix_spawnp)
