// dd/runtime/os/darwin -- the macOS (Darwin/aarch64) guest JIT (jitdarwin), brought in WHOLE.
//
// Same-ISA aarch64->aarch64 DBT for NATIVE macOS Mach-O binaries -- the JIT is purely the syscall-
// interception point (BSD + Mach traps at svc), not a translator. This is the darwin/aarch64 target
// of the guest-OS x guest-ISA matrix. Minimal POC under active development upstream
// (poc/runtime/jitdarwin/jitdarwin.c); brought in whole until the dedup onto the shared jit/ +
// frontend/aarch64 engine (it shares the ENTIRE aarch64 codegen with the linux/aarch64 target --
// the only deltas are the Mach-O loader, the Darwin sysno->canonical map, and Mach traps).
// Re-sync with: make sync-darwin.

// jit-darwin-aarch64 — a same-ISA (aarch64→aarch64) dynamic binary translator for NATIVE macOS
// Mach-O binaries, intercepting every Darwin syscall (BSD + Mach) at the `svc` boundary. No VM.
// Structured like jit.c/jit86.c: code cache, block translator, dispatcher with spill/reload, block
// chaining via the map, an indirect-branch exit (ret/blr/br), and a Mach-O loader. The interceptor
// `darwin_service()` is the macOS-container sentry hook (path-rewrite / PID / cgroup go there).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <mach-o/loader.h>
#include <pthread.h>

// ---- guest CPU state (spilled here at block boundaries; lives in real regs inside a block) ----
struct cpu { uint64_t x[31]; uint64_t sp; uint64_t pc; uint64_t reason; uint64_t host_sp; uint64_t host_save[12]; };
static struct cpu CPU;
enum { R_NEXT=0, R_SYSCALL=1 };
#define OFF_SP (31*8)
#define OFF_PC (32*8)
#define OFF_RSN (33*8)
#define OFF_HSP (34*8)
#define OFF_HS (35*8)

// ---- loaded image: segments slid to a non-colliding base; guest vmaddr V lives at V+g_slide ----
static int64_t g_slide;
static uint64_t g_text_lo;
static uint32_t guest_insn(uint64_t va){ return *(uint32_t*)va; }   // pc is a LOADED address

// ---- code cache + block map (guest vmaddr -> translated host entry) ----
#define CACHE_SZ (32<<20)
static uint8_t *CACHE; static uint32_t *CP;
struct ent { uint64_t gpc; void *host; };
static struct ent MAP[1<<17];
static void *map_get(uint64_t g){ for(uint32_t h=(g>>2)&0x1FFFF;;h=(h+1)&0x1FFFF){ if(MAP[h].host&&MAP[h].gpc==g) return MAP[h].host; if(!MAP[h].host) return 0; } }
static void map_put(uint64_t g, void*host){ for(uint32_t h=(g>>2)&0x1FFFF;;h=(h+1)&0x1FFFF){ if(!MAP[h].host){ MAP[h].gpc=g; MAP[h].host=host; return; } } }

// ---- emit helpers ----
static void emit(uint32_t w){ *CP++=w; }
static void e_movc(int rd, uint64_t v){ emit(0xD2800000u|((v&0xFFFF)<<5)|rd); for(int s=1;s<4;s++){ uint32_t h=(v>>(16*s))&0xFFFF; if(h)emit(0xF2800000u|((unsigned)s<<21)|(h<<5)|rd);} }
static void e_ldr(int rt,int rn,int off){ emit(0xF9400000u|(((unsigned)(off/8)&0xFFF)<<10)|(rn<<5)|rt); }
static void e_str(int rt,int rn,int off){ emit(0xF9000000u|(((unsigned)(off/8)&0xFFF)<<10)|(rn<<5)|rt); }
static void e_stur(int rt,int rn,int off){ emit(0xF8000000u|(((unsigned)off&0x1FF)<<12)|(rn<<5)|rt); }
static void e_ldur(int rt,int rn,int off){ emit(0xF8400000u|(((unsigned)off&0x1FF)<<12)|(rn<<5)|rt); }
static void e_movsp_from(int r){ emit(0x91000000u|(r<<5)|31); }   // mov sp, xr
static void e_movsp_to(int r){ emit(0x91000000u|(31<<5)|r); }     // mov xr, sp
static void e_cpu(int r){ e_movc(r,(uint64_t)&CPU); }             // r = &CPU (single-threaded)

// Prologue: entered with x0=&CPU. Save host callee-saved x19..x30, load guest sp + x1..x30, x0 last.
static void emit_prologue(void){
    for(int r=19;r<=30;r++) e_str(r,0,OFF_HS+(r-19)*8);          // preserve host callee-saved
    e_movsp_to(9); e_str(9,0,OFF_HSP);                          // save host sp
    e_ldr(9,0,OFF_SP); e_movsp_from(9);                        // load guest sp
    for(int r=1;r<=30;r++) if(r!=18) e_ldr(r,0,r*8);            // x18 stays in CPU (macOS-volatile)
    e_ldr(0,0,0);                                               // guest x0 last
}
// Spill guest x0..x30 + sp to CPU; leaves x0=&CPU. Done FIRST, before any x0/x1 clobber.
static void emit_spill(void){
    e_stur(0,31,-16); e_cpu(0);                                 // park guest x0; x0=&CPU
    for(int r=1;r<=30;r++) if(r!=18) e_str(r,0,r*8);
    e_ldur(1,31,-16); e_str(1,0,0);                            // CPU.x[0]=guest x0
    e_movsp_to(1); e_str(1,0,OFF_SP);                          // save guest sp
}
static void emit_ret(void){                                    // restore host sp + callee-saved, ret to dispatcher
    e_ldr(1,0,OFF_HSP); e_movsp_from(1);
    for(int r=19;r<=30;r++) e_ldr(r,0,OFF_HS+(r-19)*8);
    emit(0xD65F03C0u);
}
// exits (x0=&CPU after emit_spill). pc is a LOADED address.
static void exit_const(uint64_t pc,int rsn){ emit_spill(); e_movc(1,pc); e_str(1,0,OFF_PC); e_movc(1,rsn); e_str(1,0,OFF_RSN); emit_ret(); }
static void exit_reg(int rn){ emit_spill(); e_ldr(1,0,rn*8); e_str(1,0,OFF_PC); e_movc(1,R_NEXT); e_str(1,0,OFF_RSN); emit_ret(); }       // ret/br: pc=saved guest xrn
static void exit_call(uint64_t ret,uint64_t pc,int rn){       // bl(rn<0): pc const; blr(rn>=0): pc=xrn. set guest x30=ret.
    emit_spill();
    if(rn>=0) e_ldr(2,0,rn*8);                                 // x2 = target (before clobbering x30)
    e_movc(1,ret); e_str(1,0,30*8);                            // CPU.x[30] = ret addr
    if(rn>=0) e_str(2,0,OFF_PC); else { e_movc(1,pc); e_str(1,0,OFF_PC); }
    e_movc(1,R_NEXT); e_str(1,0,OFF_RSN); emit_ret();
}

static void translate_block(uint64_t gpc){                     // gpc is a LOADED address
    map_put(gpc,CP);
    emit_prologue();
    for(;;){
        uint32_t in=guest_insn(gpc);
        if(in==0xD4001001u){ exit_const(gpc+4,R_SYSCALL); return; }                                  // svc
        if((in&0xFC000000u)==0x14000000u){ int64_t o=in&0x3FFFFFF; if(o&(1<<25))o|=~0x3FFFFFFLL; exit_const(gpc+(o<<2),R_NEXT); return; } // b
        if((in&0xFC000000u)==0x94000000u){ int64_t o=in&0x3FFFFFF; if(o&(1<<25))o|=~0x3FFFFFFLL; exit_call(gpc+4,gpc+(o<<2),-1); return; } // bl
        if((in&0xFFFFFC1Fu)==0xD65F0000u){ exit_reg((in>>5)&31); return; }                            // ret
        if((in&0xFFFFFC1Fu)==0xD61F0000u){ exit_reg((in>>5)&31); return; }                            // br
        if((in&0xFFFFFC1Fu)==0xD63F0000u){ exit_call(gpc+4,0,(in>>5)&31); return; }                   // blr
        if((in&0xFF000010u)==0x54000000u){ int64_t o=(in>>5)&0x7FFFF; if(o&(1<<18))o|=~0x7FFFFLL;     // b.cond
            uint32_t *p=CP; emit(0); exit_const(gpc+4,R_NEXT);
            *p=(in&0xFF00001Fu)|(((uint32_t)(CP-p)&0x7FFFF)<<5); exit_const(gpc+(o<<2),R_NEXT); return; }
        if((in&0x7E000000u)==0x34000000u){ int64_t o=(in>>5)&0x7FFFF; if(o&(1<<18))o|=~0x7FFFFLL;     // cbz/cbnz
            uint32_t *p=CP; emit(0); exit_const(gpc+4,R_NEXT);
            *p=(in&0xFF00001Fu)|(((uint32_t)(CP-p)&0x7FFFF)<<5); exit_const(gpc+(o<<2),R_NEXT); return; }
        if((in&0x7E000000u)==0x36000000u){ int64_t o=(in>>5)&0x3FFF; if(o&(1<<13))o|=~0x3FFFLL;       // tbz/tbnz (imm14)
            uint32_t *p=CP; emit(0); exit_const(gpc+4,R_NEXT);
            *p=(in&0xFFF8001Fu)|(((uint32_t)(CP-p)&0x3FFF)<<5); exit_const(gpc+(o<<2),R_NEXT); return; }
        if((in&0x9F000000u)==0x10000000u){ int rd=in&0x1F; int64_t imm=(((in>>5)&0x7FFFF)<<2)|((in>>29)&3); if(imm&(1LL<<20))imm|=~((1LL<<21)-1); e_movc(rd,gpc+imm); gpc+=4; continue; } // adr -> loaded target
        if((in&0x9F000000u)==0x90000000u){ int rd=in&0x1F; int64_t imm=(((in>>5)&0x7FFFF)<<2)|((in>>29)&3); if(imm&(1LL<<20))imm|=~((1LL<<21)-1); e_movc(rd,(int64_t)(gpc&~0xFFFLL)+(imm<<12)); gpc+=4; continue; } // adrp -> loaded PAGE (slide-relocated; copying it verbatim would page off the host cache PC)
        emit(in); gpc+=4;                                                                            // same-ISA copy
    }
}

// ---- Darwin syscall service (BSD + Mach) — the macOS-container sentry hook ----
extern long darwin_raw_syscall(long,long,long,long,long,long,long);
__asm__(".global _darwin_raw_syscall\n.align 2\n_darwin_raw_syscall:\n"
"  mov x16,x0\n mov x0,x1\n mov x1,x2\n mov x2,x3\n mov x3,x4\n mov x4,x5\n mov x5,x6\n svc #0x80\n ret\n");
int g_log=1, g_n;
const char *g_rootfs;                                          // container rootfs jail (--rootfs)
struct vol { const char *cont, *host; } g_vols[16]; int g_nvol; // bind volumes (--volume HOST:CONT)
int g_pid1=1;                                                  // PID namespace: container init is pid 1
// Rewrite an absolute guest path: volume binds first (a container path -> a host dir), then the rootfs
// jail. Same confinement model as jit.c (DDVOL + rootfs).
static const char *jail(const char *p, char *out){
    if(!p || p[0]!='/') return p;
    for(int i=0;i<g_nvol;i++){ size_t L=strlen(g_vols[i].cont);
        if(!strncmp(p,g_vols[i].cont,L) && (p[L]=='/'||p[L]==0)){ snprintf(out,1024,"%s%s",g_vols[i].host,p+L); return out; } }
    if(g_rootfs){ snprintf(out,1024,"%s%s",g_rootfs,p); return out; }
    return p;
}
// Darwin BSD syscall numbers (the os/darwin personality's table)
enum { SYS_exit=1, SYS_read=3, SYS_write=4, SYS_open=5, SYS_close=6, SYS_link=9, SYS_unlink=10,
       SYS_chdir=12, SYS_chmod=15, SYS_getpid=20, SYS_access=33, SYS_getppid=39, SYS_symlink=57,
       SYS_readlink=58, SYS_rename=128, SYS_mkdir=136, SYS_rmdir=137, SYS_stat64=338,
       SYS_lstat64=340, SYS_getattrlist=220, SYS_openat=463 };
static void darwin_service(void){
    long nr=(long)CPU.x[16];
    long a[6]={(long)CPU.x[0],(long)CPU.x[1],(long)CPU.x[2],(long)CPU.x[3],(long)CPU.x[4],(long)CPU.x[5]};
    char b0[1024], b1[1024]; const char *tag="";
    g_n++;
    // --- PID namespace: the container's first process sees itself as pid 1 ---
    if(g_pid1 && nr==SYS_getpid){ CPU.x[0]=1; return; }
    if(g_pid1 && nr==SYS_getppid){ CPU.x[0]=0; return; }
    // --- filesystem confinement (rootfs jail + volume binds) ---
    switch(nr){
      case SYS_open: case SYS_access: case SYS_stat64: case SYS_lstat64: case SYS_unlink:
      case SYS_chdir: case SYS_chmod: case SYS_readlink: case SYS_mkdir: case SYS_rmdir:
      case SYS_getattrlist:
        a[0]=(long)jail((char*)a[0],b0); tag="  (jailed)"; break;
      case SYS_openat:                                        // openat: path is arg1
        a[1]=(long)jail((char*)a[1],b1); tag="  (jailed)"; break;
      case SYS_rename: case SYS_link: case SYS_symlink:       // two paths
        a[0]=(long)jail((char*)a[0],b0); a[1]=(long)jail((char*)a[1],b1); tag="  (jailed)"; break;
    }
    if(g_log) fprintf(stderr,"[jitdarwin] %s #%ld a0=%#lx a1=%#lx%s\n", nr<0?"MACH-TRAP":"BSD", nr<0?-nr:nr, a[0],a[1], tag);
    if(nr==SYS_exit) exit((int)a[0]);                          // SYS_exit -> terminate
    CPU.x[0]=(uint64_t)darwin_raw_syscall(nr,a[0],a[1],a[2],a[3],a[4],a[5]);
}

typedef void (*blockfn)(struct cpu*);
static void run_guest(void){
    for(;;){
        void *host=map_get(CPU.pc);
        if(!host){ pthread_jit_write_protect_np(0); translate_block(CPU.pc); pthread_jit_write_protect_np(1);
                   __builtin___clear_cache((char*)CACHE,(char*)CP); host=map_get(CPU.pc); }
        ((blockfn)host)(&CPU);
        if(CPU.reason==R_SYSCALL) darwin_service();             // pc already advanced to gpc+4
    }
}

int main(int argc,char**argv){
    int ai=1;
    for(;ai<argc && argv[ai][0]=='-';ai++){                     // flags (mirrors jit.c's CLI)
        if(!strcmp(argv[ai],"--rootfs") && ai+1<argc) g_rootfs=argv[++ai];
        else if(!strcmp(argv[ai],"--volume") && ai+1<argc){     // --volume HOST:CONTAINER (docker order)
            char *s=strdup(argv[++ai]), *c=strchr(s,':');
            if(c && g_nvol<16){ *c=0; g_vols[g_nvol].host=s; g_vols[g_nvol].cont=c+1; g_nvol++; } }
        else if(!strcmp(argv[ai],"-q")) g_log=0;
    }
    if(ai>=argc){ fprintf(stderr,"usage: %s [--rootfs DIR] [-q] <mach-o>\n",argv[0]); return 2; }
    char *path=argv[ai];
    int fd=open(path,O_RDONLY); off_t sz=lseek(fd,0,SEEK_END); lseek(fd,0,SEEK_SET);
    uint8_t *f=malloc(sz); if(read(fd,f,sz)!=sz) return 2; close(fd);
    struct mach_header_64 *mh=(void*)f; if(mh->magic!=MH_MAGIC_64){ fprintf(stderr,"not Mach-O 64\n"); return 2; }
    uint8_t *base=mmap(0,256<<20,PROT_NONE,MAP_PRIVATE|MAP_ANON,-1,0);    // reserve a non-colliding window
    uint8_t *lc=f+sizeof *mh; uint64_t first_vm=0,entryoff=0; int got=0;
    for(uint32_t i=0;i<mh->ncmds;i++){ struct load_command*c=(void*)lc;
        if(c->cmd==LC_SEGMENT_64){ struct segment_command_64*s=(void*)lc;
            if(s->vmsize && strcmp(s->segname,"__PAGEZERO")){
                if(!got){ first_vm=s->vmaddr; g_slide=(int64_t)base-(int64_t)first_vm; got=1; }
                uint64_t dst=s->vmaddr+g_slide;
                mmap((void*)dst,(s->vmsize+0xFFF)&~0xFFFULL,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_FIXED,-1,0);
                if(s->filesize) memcpy((void*)dst,f+s->fileoff,s->filesize);
                if(!strcmp(s->segname,"__TEXT")) g_text_lo=s->vmaddr; } }
        else if(c->cmd==LC_MAIN) entryoff=((struct entry_point_command*)lc)->entryoff;
        lc+=c->cmdsize; }
    uint8_t *stk=mmap(0,1<<20,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON,-1,0);
    CPU.sp=((uint64_t)(stk+(1<<20)-256))&~15ULL;
    CPU.pc=g_text_lo+entryoff+g_slide; CPU.x[18]=0;     // LOADED entry address
    CACHE=mmap(0,CACHE_SZ,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANON|MAP_JIT,-1,0); CP=(uint32_t*)CACHE;
    fprintf(stderr,"[jitdarwin] entry=%#llx slide=%#llx; running native macOS code via JIT (no VM)...\n",
            (unsigned long long)CPU.pc,(unsigned long long)g_slide);
    run_guest();
    return 0;
}
