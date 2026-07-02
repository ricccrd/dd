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
#include <sys/mman.h>
#include <netinet/in.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach/machine.h>
#include <libkern/OSByteOrder.h>
#include <libkern/OSCacheControl.h>
#include "../../container_parse.h" // strict numeric parsing (the config trust boundary; see LAUNCH.md)
extern int sandbox_init(const char *profile, uint64_t flags, char **errorbuf);
// libcompiler_rt symbol that __builtin___clear_cache lowers to (aliased to dodge the clang builtin,
// whose address can't be taken); we interpose it to flip emulated-RWX pages executable.
extern void dj_clear_cache(void *start, void *end) __asm__("___clear_cache");

static const char *g_rootfs, *g_hostname; static int g_pid1;
// The container-side (guest) current working directory, kept as a canonical absolute container path.
// getcwd() returns THIS (never the leaked host rootfs path), and chdir() resolves its argument against
// it -- so "cd .." ascends to the real parent and clamps at the jail root "/". See dj_canon below.
static char g_cwd[1024] = "/";
static char *g_low[8]; static int g_nlow;
static struct { char *host, *cont; } g_vol[16]; static int g_nvol;
static struct { int host, cont; } g_pub[16]; static int g_npub;

static void split(char *v, void (*f)(char*)){ if(!v) return; char *s=strdup(v),*t,*sp=0;
    for(t=strtok_r(s,",",&sp); t; t=strtok_r(0,",",&sp)) f(t); }
// Collections fail loud on cap overflow (never silently drop) and ports are strictly range-checked.
static void add_low(char *d){
    if(g_nlow>=8){ fprintf(stderr,"dd: too many DD_LOWERS entries (max 8)\n"); exit(2); }
    g_low[g_nlow++]=strdup(d);
}
static void add_vol(char *hc){
    char *c=strchr(hc,':');
    if(!c){ fprintf(stderr,"dd: invalid DD_VOLUMES '%s': expected HOST:CONTAINER\n",hc); exit(2); }
    if(g_nvol>=16){ fprintf(stderr,"dd: too many DD_VOLUMES entries (max 16)\n"); exit(2); }
    *c=0; g_vol[g_nvol].host=strdup(hc); g_vol[g_nvol].cont=strdup(c+1); g_nvol++;
}
static void add_pub(char *hc){
    char *c=strchr(hc,':');
    if(!c){ fprintf(stderr,"dd: invalid DD_PUBLISH '%s': expected HOST:CONTAINER\n",hc); exit(2); }
    if(g_npub>=16){ fprintf(stderr,"dd: too many DD_PUBLISH entries (max 16)\n"); exit(2); }
    *c=0; g_pub[g_npub].host=(int)dd_parse_port("DD_PUBLISH host port",hc);
    g_pub[g_npub].cont=(int)dd_parse_port("DD_PUBLISH container port",c+1); g_npub++;
}

static const char *jail(const char *p, char *out); // defined below; used by init for DD_CWD
static void dj_canon(const char *base, const char *in, char *out, size_t outsz); // path canonicalizer

__attribute__((constructor)) static void init(void){
    g_rootfs = getenv("DD_ROOTFS"); g_hostname = getenv("DD_HOSTNAME"); g_pid1 = getenv("DD_PID1")!=0;
    split(getenv("DD_LOWERS"), add_low); split(getenv("DD_VOLUMES"), add_vol); split(getenv("DD_PUBLISH"), add_pub);
    // initial working directory (docker -w / the cwd ddcli mounts): chdir into the container path.
    const char *cwd = getenv("DD_CWD");
    if(cwd && cwd[0] && g_rootfs){
        char nc[1024]; dj_canon("/", cwd, nc, sizeof nc);     // canonical container path of the workdir
        char b[1024];
        if(chdir(jail(nc, b)) == 0) strlcpy(g_cwd, nc, sizeof g_cwd); // track it as the guest cwd
    }
    char *mm=getenv("DD_MEM_MAX"), *pm=getenv("DD_PIDS_MAX");
    if(mm){ rlim_t v=dd_parse_u64("DD_MEM_MAX",mm,0,RLIM_INFINITY); struct rlimit r={v,v}; setrlimit(RLIMIT_AS,&r); }
    if(pm){ rlim_t v=dd_parse_u64("DD_PIDS_MAX",pm,0,INT_MAX); struct rlimit r={v,v}; setrlimit(RLIMIT_NPROC,&r); }
    if(getenv("DD_SANDBOX") && g_rootfs){
        char prof[4096]; int n=snprintf(prof,sizeof prof,
            "(version 1)(allow default)(deny file-write* (subpath \"/\"))"
            "(allow file-write* (subpath \"%s\"))"
            "(allow file-write* (subpath \"/private/tmp\") (subpath \"/private/var/folders\") (subpath \"/dev\"))",
            g_rootfs);
        for(int i=0;i<g_nvol && n<3000;i++) n+=snprintf(prof+n,sizeof prof-n,"(allow file-write* (subpath \"%s\"))",g_vol[i].host);
        if(getenv("DD_NET_ISOLATE")) n+=snprintf(prof+n,sizeof prof-n,
            "(deny network-outbound (remote ip \"*:*\"))(allow network-outbound (remote ip \"localhost:*\"))");
        // The macOS Seatbelt doesn't nest. On a mac that already confines this process (e.g. Sequoia
        // sandboxes a notarized app's spawned children) sandbox_init fails "Operation not permitted" and
        // libsandbox prints its own "sandbox initialization failed" line to stderr. The container's real
        // jail is the libc path-interposition above, not Seatbelt -- so mute libsandbox's stderr across the
        // call, still apply Seatbelt where it works, and surface only an UNEXPECTED error.
        char *err=0; int sb;
        { int sv=dup(2), dn=open("/dev/null",O_WRONLY); if(dn>=0){ dup2(dn,2); close(dn); }
          sb=sandbox_init(prof,0,&err);
          if(sv>=0){ dup2(sv,2); close(sv); } }
        if(sb && (!err || !strstr(err,"Operation not permitted")))
            fprintf(stderr,"[darwinjail] sandbox: %s\n",err?err:"?");
    }
}
// Canonicalize container path `in` into `out` as an absolute container path: a relative `in` is resolved
// against `base`, then "." is dropped and ".." ascends one component -- but ".." CLAMPS at "/" (the jail
// root), so no sequence of ".." can ever walk above the rootfs. Pure string math (no host fs access): the
// guest namespace is the container's, not the host's. This both makes "cd .." work and seals the escape
// where e.g. open("/a/../../../etc/passwd") would otherwise let the host fs resolve ".." out of the rootfs.
static void dj_canon(const char *base, const char *in, char *out, size_t outsz){
    char tmp[2048];
    if(in[0]=='/') tmp[0]=0;                                  // absolute: base is irrelevant
    else strlcpy(tmp, (base && base[0]) ? base : "/", sizeof tmp);
    size_t l=strlen(tmp); snprintf(tmp+l, sizeof tmp-l, "/%s", in); // join base + "/" + in, then tokenize
    out[0]='/'; out[1]=0; size_t olen=1;                      // build a clean absolute path in `out`
    char *save=0;
    for(char *t=strtok_r(tmp,"/",&save); t; t=strtok_r(0,"/",&save)){
        if(!strcmp(t,".")) continue;
        if(!strcmp(t,"..")){                                  // ascend, clamped at root
            while(olen>1 && out[olen-1]!='/') olen--;         // strip the last component
            if(olen>1) olen--;                                // and its leading slash
            out[olen]=0;
            continue;
        }
        if(olen+1+strlen(t) >= outsz) break;                  // truncate rather than overflow
        if(olen>1) out[olen++]='/';
        size_t tl=strlen(t); memcpy(out+olen,t,tl); olen+=tl; out[olen]=0;
    }
}
// container path -> host path: bind volumes, then overlay (upper wins, else a lower, else upper for creates).
static const char *jail(const char *p, char *out){
    if(!p || p[0]!='/' || !g_rootfs) return p;
    char c[1024]; dj_canon("/", p, c, sizeof c);             // resolve "." / ".." within the jail first
    for(int i=0;i<g_nvol;i++){ size_t L=strlen(g_vol[i].cont);
        if(!strncmp(c,g_vol[i].cont,L) && (c[L]=='/'||c[L]==0)){ snprintf(out,1024,"%s%s",g_vol[i].host,c+L); return out; } }
    snprintf(out,1024,"%s%s",g_rootfs,c);
    if(access(out,F_OK)==0) return out;
    for(int i=0;i<g_nlow;i++){ char t[1024]; snprintf(t,1024,"%s%s",g_low[i],c);
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
// chdir confined to the container: resolve the (relative-or-absolute) target against the guest cwd, with
// ".." clamped at the jail root, then chdir into its host mapping. On success record the new guest cwd so
// getcwd() and subsequent relative chdirs stay consistent. "cd .." at "/" stays at "/" (never escapes).
int   jail_chdir(const char*p){
    if(!p || !g_rootfs) return chdir(p);
    char nc[1024]; dj_canon(g_cwd, p, nc, sizeof nc);
    char hb[1024]; int r = chdir(jail(nc, hb));
    if(r==0) strlcpy(g_cwd, nc, sizeof g_cwd);
    return r;
}
// Report the GUEST cwd (the canonical container path), never the host rootfs path getcwd() would leak.
// Mirrors libc's getcwd contract incl. the NULL-buf / size==0 malloc extension and ERANGE.
char *jail_getcwd(char *buf, size_t size){
    if(!g_rootfs) return getcwd(buf, size);
    size_t n=strlen(g_cwd);
    if(!buf){ size_t a=(size>n+1)?size:n+1; buf=malloc(a); if(!buf){ errno=ENOMEM; return 0; } }
    else if(size < n+1){ errno=ERANGE; return 0; }
    memcpy(buf, g_cwd, n+1);
    return buf;
}
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
// raise() on Darwin is pthread_kill(pthread_self(), sig) -- a THREAD-directed signal. The kqueue
// EVFILT_SIGNAL knote lives on the process klist and only fires for PROCESS-directed signals (the
// psignal path), so a self-raised signal is invisible to a guest's signal kqueue (libdispatch signal
// sources, the kq-signal case). Redirect to a process-directed kill so the knote posts, matching the
// BSD kqueue model. In a single-threaded guest this is equivalent to raise(); getpid() here is the
// real pid (intra-dylib calls are not interposed, as jail_getpid itself relies on).
int jail_raise(int sig){ return kill(getpid(), sig); }
int jail_bind(int s,const struct sockaddr*a,socklen_t l){
    if(a && a->sa_family==AF_INET){ struct sockaddr_in in=*(struct sockaddr_in*)a; int p=ntohs(in.sin_port);
        for(int i=0;i<g_npub;i++) if(g_pub[i].cont==p){ in.sin_port=htons(g_pub[i].host); return bind(s,(struct sockaddr*)&in,l); } }
    return bind(s,a,l);
}
// The jail dylib is plain arm64; dyld refuses to insert it into an arm64e process (system binaries
// under /usr/bin, /bin, … are arm64e) and aborts the child with "incompatible architecture". Such
// binaries can't be path-jailed via DYLD interposition anyway, so for an arm64e target we strip
// DYLD_INSERT_LIBRARIES from its environment and let it run un-jailed instead of crashing.
static int is_arm64e_image(const char *path){
    int fd = open(path, O_RDONLY);
    if(fd < 0) return 0;
    uint8_t hdr[4096];
    ssize_t n = read(fd, hdr, sizeof hdr);
    close(fd);
    if(n < (ssize_t)sizeof(uint32_t)) return 0;
    uint32_t magic = *(uint32_t*)hdr;
    if(magic == MH_MAGIC_64 || magic == MH_MAGIC){               // thin Mach-O (host byte order)
        struct mach_header_64 *mh = (void*)hdr;
        return mh->cputype == CPU_TYPE_ARM64 && (mh->cpusubtype & ~CPU_SUBTYPE_MASK) == CPU_SUBTYPE_ARM64E;
    }
    if(magic == FAT_MAGIC || magic == FAT_CIGAM || magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64){
        int is64 = (magic == FAT_MAGIC_64 || magic == FAT_CIGAM_64);      // fat: arch list is big-endian
        uint32_t nfat = OSSwapBigToHostInt32(((struct fat_header*)hdr)->nfat_arch);
        uint8_t *p = hdr + sizeof(struct fat_header), *end = hdr + n;
        for(uint32_t i=0;i<nfat;i++){
            cpu_type_t ct; cpu_subtype_t cs; size_t sz = is64 ? sizeof(struct fat_arch_64) : sizeof(struct fat_arch);
            if(p + sz > end) break;
            if(is64){ struct fat_arch_64 *fa=(void*)p; ct=OSSwapBigToHostInt32(fa->cputype); cs=OSSwapBigToHostInt32(fa->cpusubtype); }
            else    { struct fat_arch    *fa=(void*)p; ct=OSSwapBigToHostInt32(fa->cputype); cs=OSSwapBigToHostInt32(fa->cpusubtype); }
            // dyld prefers an arm64e slice on Apple Silicon, where our arm64-only dylib won't match.
            if(ct == CPU_TYPE_ARM64 && (cs & ~CPU_SUBTYPE_MASK) == CPU_SUBTYPE_ARM64E) return 1;
            p += sz;
        }
    }
    return 0;
}
static char **env_drop_dyld_insert(char *const env[]){            // malloc'd copy minus DYLD_INSERT_LIBRARIES
    size_t n=0; while(env && env[n]) n++;
    char **out = malloc((n+1)*sizeof *out);
    if(!out) return (char**)env;
    size_t j=0;
    for(size_t i=0;i<n;i++) if(strncmp(env[i],"DYLD_INSERT_LIBRARIES=",22)) out[j++]=env[i];
    out[j]=0;
    return out;
}
// Return a malloc'd env copy with DD_CWD set to the LIVE guest cwd. The daemon injects DD_CWD once (the
// docker -w) at container start, but a shell then cd's around IN-PROCESS -- so an exec'd child that
// inherited the stale start value would re-init() into the wrong directory and getcwd() would report it
// (the shell's builtin pwd hides this; real tools calling getcwd -- git/make/coreutils -- do not).
// Carrying the current g_cwd forward on every exec keeps getcwd() consistent across exec. Mirrors
// env_drop_dyld_insert's copy-on-exec pattern; the child seeds g_cwd from this DD_CWD in init().
static char **env_set_cwd(char *const env[]){
    size_t n=0; while(env && env[n]) n++;
    char **out = malloc((n+2)*sizeof *out);                       // room for the (re)added DD_CWD + NULL
    if(!out) return (char**)env;
    static __thread char kv[sizeof g_cwd + 8];                    // "DD_CWD=" + path + NUL
    snprintf(kv, sizeof kv, "DD_CWD=%s", g_cwd);
    size_t j=0;
    for(size_t i=0;i<n;i++) if(strncmp(env[i],"DD_CWD=",7)) out[j++]=env[i]; // drop any stale DD_CWD
    out[j++]=kv; out[j]=0;                                         // then append the live one
    return out;
}
// exec: jail the program path so a container PATH / a container-local binary resolves into the rootfs.
// The child inherits DYLD_INSERT_LIBRARIES (env), so the jail re-arms in the new process -- except for
// an arm64e target, where the insert is dropped (see is_arm64e_image). A jailed child also carries the
// live DD_CWD (env_set_cwd) so getcwd() stays anchored to the current dir across exec.
int jail_execve(const char*p,char*const a[],char*const e[]){
    const char *jp = JAIL(p);
    if(is_arm64e_image(jp)) return execve(jp, a, env_drop_dyld_insert(e)); // un-jailed child: DD_CWD unused
    char **ne=env_set_cwd(e); int r=execve(jp, a, ne); free(ne); return r; // free reached only if exec fails
}
int jail_posix_spawn (pid_t*pid,const char*p,const posix_spawn_file_actions_t*fa,const posix_spawnattr_t*at,char*const a[],char*const e[]){
    const char *jp = JAIL(p);
    if(is_arm64e_image(jp)){ char **ne=env_drop_dyld_insert(e); int r=posix_spawn(pid,jp,fa,at,a,ne); free(ne); return r; }
    char **ne=env_set_cwd(e); int r=posix_spawn(pid,jp,fa,at,a,ne); free(ne); return r; }
int jail_posix_spawnp(pid_t*pid,const char*p,const posix_spawn_file_actions_t*fa,const posix_spawnattr_t*at,char*const a[],char*const e[]){
    if(p && p[0]=='/' && is_arm64e_image(p)){ char **ne=env_drop_dyld_insert(e); int r=posix_spawnp(pid,p,fa,at,a,ne); free(ne); return r; }
    char **ne=env_set_cwd(e); int r=posix_spawnp(pid,p,fa,at,a,ne); free(ne); return r; } // p may be a name; PATH via interposed access()
// macOS forbids a page that is simultaneously writable and executable (W^X), so a guest's plain
// mmap(PROT_WRITE|PROT_EXEC) with no MAP_JIT -- the pattern guest JIT runtimes (JVM/V8/LuaJIT) use --
// returns EPERM. Emulate RWX with the MAP_JIT mechanism: add MAP_JIT under the hood and leave the
// region writable for this thread so the guest's code-write succeeds. The guest's mandatory icache
// flush before executing the new code (interposed below) flips the region to executable -- the natural
// W^X transition point, so write-then-execute works without the guest knowing about MAP_JIT.
void *jail_mmap(void *addr,size_t len,int prot,int flags,int fd,off_t off){
    if((prot & PROT_EXEC) && (flags & MAP_ANON) && !(flags & MAP_JIT)){
        void *p = mmap(addr, len, prot, flags | MAP_JIT, fd, off);
        if(p != MAP_FAILED) pthread_jit_write_protect_np(0);     // start writable so the guest can fill it
        return p;
    }
    return mmap(addr, len, prot, flags, fd, off);
}
// The W^X transition for the emulated-RWX MAP_JIT regions above: a guest flushes the icache after
// writing code and before executing it, so flip those regions back to executable, then flush as asked.
// Both libcompiler_rt's __clear_cache (what __builtin___clear_cache lowers to) and a direct
// sys_icache_invalidate land here; the toggle is harmless for guests that manage MAP_JIT themselves.
void jail_sys_icache_invalidate(void *start,size_t len){ pthread_jit_write_protect_np(1); sys_icache_invalidate(start, len); }
void jail_clear_cache(void *start,void *end){ pthread_jit_write_protect_np(1); sys_icache_invalidate(start, (char*)end-(char*)start); }

#define INTERPOSE(repl,orig) __attribute__((used)) static struct { const void*r; const void*o; } \
  _ip_##orig __attribute__((section("__DATA,__interpose"))) = { (const void*)repl, (const void*)orig };
INTERPOSE(jail_open, open)         INTERPOSE(jail_openat, openat)      INTERPOSE(jail_stat, stat)
INTERPOSE(jail_lstat, lstat)       INTERPOSE(jail_fstatat, fstatat)    INTERPOSE(jail_access, access)
INTERPOSE(jail_faccessat, faccessat) INTERPOSE(jail_readlink, readlink) INTERPOSE(jail_readlinkat, readlinkat)
INTERPOSE(jail_unlink, unlink)     INTERPOSE(jail_unlinkat, unlinkat)  INTERPOSE(jail_mkdir, mkdir)
INTERPOSE(jail_mkdirat, mkdirat)   INTERPOSE(jail_rmdir, rmdir)        INTERPOSE(jail_chdir, chdir)
INTERPOSE(jail_getcwd, getcwd)
INTERPOSE(jail_chmod, chmod)       INTERPOSE(jail_chown, chown)        INTERPOSE(jail_lchown, lchown)
INTERPOSE(jail_statfs, statfs)     INTERPOSE(jail_utimes, utimes)      INTERPOSE(jail_rename, rename)
INTERPOSE(jail_link, link)         INTERPOSE(jail_symlink, symlink)    INTERPOSE(jail_opendir, opendir)
INTERPOSE(jail_fopen, fopen)       INTERPOSE(jail_freopen, freopen)
INTERPOSE(jail_gethostname, gethostname) INTERPOSE(jail_bind, bind)    INTERPOSE(jail_getpid, getpid)
INTERPOSE(jail_raise, raise)
INTERPOSE(jail_execve, execve)     INTERPOSE(jail_posix_spawn, posix_spawn) INTERPOSE(jail_posix_spawnp, posix_spawnp)
INTERPOSE(jail_mmap, mmap)         INTERPOSE(jail_sys_icache_invalidate, sys_icache_invalidate)
INTERPOSE(jail_clear_cache, dj_clear_cache)
