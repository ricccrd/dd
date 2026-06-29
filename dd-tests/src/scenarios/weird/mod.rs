//! Weird / edge software — the cases that hammer the engine's corners hardest: JIT-in-JIT runtimes
//! (V8/JVM/LuaJIT/PyPy/BEAM/RyuJIT/Julia emitting + executing their own machine code), self-modifying
//! code, exotic syscalls (io_uring, eBPF, seccomp, ptrace, userfaultfd, memfd, timerfd, inotify),
//! compression/crypto codegen (gzip/bzip2/xz/zstd/openssl), unusual languages (Haskell/Erlang/Forth/
//! Tcl/Lua/R), and CPU-feature probing (cpuid/NEON, getauxval AT_HWCAP, rdtsc/cntvct, cpu-topology).
//! These are where a translator is most likely to diverge. Both Linux arches. Owner: weird agent.
//!
//! Every scenario below is proven on the REAL oracle (Docker Desktop, arm64) — the marker matches the
//! real output, so the TEST is correct. `.xfail()` flags a *suspected* dd divergence (see GAPS.md):
//!   * gcc-compiled C cases → the documented toolchain fork-exec / exec-loader gap blocks `cc`/`ld`
//!     (and `ghc`/`dotnet build`), so they xfail on both Linux arches; each ALSO probes a deeper corner
//!     (RWX exec, SMC re-translation, signal-on-fault, rdtsc, futex, a syscall) — XPASS after the
//!     toolchain fix reveals whether that corner works.
//!   * python:3.12-slim cases → xfail amd64 only (jit86-opcode-1c: silent exit 255 on x86_64).
//!   * cpu-topology / non-PIE exec seed cases → the existing GAPS rows.

use crate::scenario::{scen, sgroup, ScenGroup, Scenario, Target};

/// A C program compiled+run inside `gcc:latest` (glibc, both arches). Compiling forks cc1/as/ld — the
/// documented toolchain fork-exec / exec-loader gap — so these xfail on both Linux arches; the comment
/// at each call site names the deeper corner the program additionally exercises.
fn cc(id: &'static str, flags: &str, src: &str) -> Scenario {
    let script = format!("cat > /m.c <<'CEOF'\n{src}\nCEOF\ncc /m.c {flags} -o /m && /m");
    scen(id, "gcc:latest").exec(&script).long().xfail(&Target::LINUX)
}

pub fn group() -> ScenGroup {
    sgroup("weird", vec![
        // ============================ JIT-in-JIT: managed runtimes ============================
        // Each forces the GUEST's own JIT to emit + execute machine code inside the dd JIT.
        // seed (proven on Real): node = a V8 JIT running inside the dd JIT. Deterministic.
        scen("weird/v8-jit-in-jit", "node:alpine")
            .exec("node -e 'let s=0;for(let i=1;i<=1000000;i++)s+=i;console.log(\"sum\",s)'")
            .has("sum 500000500000"),   // sum(1..1e6); corrected from the seed's typo
        // V8 BigInt fib(90) — exercises the BigInt slow path + JIT'd hot loop.
        scen("weird/v8-bigint", "node:alpine")
            .exec("node -e 'let a=0n,b=1n;for(let i=0;i<90;i++){[a,b]=[b,a+b]};console.log(\"FIB=\"+a)'")
            .has("FIB=2880067194370816120"),
        // V8 hot integer loop (prime sieve) — forces TurboFan to compile the inner loop.
        scen("weird/node-primes", "node:alpine")
            .exec("node -e 'let p=0;for(let n=2;n<100000;n++){let q=1;for(let d=2;d*d<=n;d++)if(n%d===0){q=0;break}p+=q}console.log(\"PRIMES=\"+p)'")
            .has("PRIMES=9592"),
        // JVM C2: 50M-iteration loop forces HotSpot C2 to JIT-compile the method (javac then java).
        scen("weird/jvm-c2-jit", "eclipse-temurin:21")
            .exec("cat > /M.java <<'EOF'\npublic class M{public static void main(String[] a){long s=0;for(long i=0;i<50000000L;i++)s+=i%7;System.out.println(\"JVM=\"+s);}}\nEOF\njavac /M.java -d /o && java -cp /o M")
            .has("JVM=149999997"),
        // JVM bigint-ish fib via javac+run (compile + managed JIT).
        scen("weird/jvm-fib", "eclipse-temurin:21")
            .exec("cat > /F.java <<'EOF'\npublic class F{public static void main(String[] a){long x=0,y=1;for(int i=0;i<50;i++){long t=x+y;x=y;y=t;}System.out.println(\"JFIB=\"+x);}}\nEOF\njavac /F.java -d /o && java -cp /o F")
            .has("JFIB=12586269025"),
        // LuaJIT trace compiler: a 10M-iteration loop triggers LuaJIT's tracing JIT to emit native code.
        scen("weird/luajit-trace", "openresty/openresty:alpine")
            .exec("/usr/local/openresty/luajit/bin/luajit -e 'local s=0 for i=1,10000000 do s=s+i%7 end print(\"LUAJIT=\"..s)'")
            .has("LUAJIT=29999997"),
        // PyPy: tracing JIT (RPython) compiles the prime-counting loop.
        scen("weird/pypy-compute", "pypy:3")
            .exec("pypy3 -c \"print('PYPY='+str(sum(1 for n in range(2,30000) if all(n%d for d in range(2,int(n**0.5)+1)))))\"")
            .has("PYPY=3245"),
        // Ruby YJIT: --yjit enables the in-process YJIT compiler on a hot loop.
        scen("weird/ruby-yjit", "ruby:3.3")
            .exec("ruby --yjit -e 's=0; (1..2000000).each{|i| s+=i%7}; puts \"YJIT=#{s}\"'")
            .has("YJIT=5999997"),
        // Ruby MRI bigint fib (interpreter + Bignum path).
        scen("weird/ruby-fib", "ruby:3.3")
            .exec("ruby -e 'a,b=0,1;50.times{a,b=b,a+b};puts \"RB=#{a}\"'")
            .has("RB=12586269025"),
        // Erlang/BEAM: OTP 27 ships BeamAsm, an in-process native-code JIT.
        scen("weird/beam-jit", "erlang:27")
            .exec("erl -noshell -eval 'io:format(\"ERL=~w~n\",[lists:sum(lists:seq(1,1000))]), halt().'")
            .has("ERL=500500").long(),
        // Julia: every function is JIT-compiled through LLVM on first call (in-process codegen).
        scen("weird/julia-jit", "julia:1")
            .exec("julia -e 'println(\"JL=\", sum(1:1000))'")
            .has("JL=500500").long(),
        // .NET CoreCLR RyuJIT: `dotnet run` compiles IL → native; build forks the SDK toolchain.
        scen("weird/dotnet-ryujit", "mcr.microsoft.com/dotnet/sdk:8.0")
            .exec("mkdir -p /app && cd /app && dotnet new console -o . >/dev/null 2>&1 && cat > Program.cs <<'EOF'\nlong s=0; for(long i=1;i<=1000;i++) s+=i; System.Console.WriteLine(\"NET=\"+s);\nEOF\ndotnet run -c Release 2>/dev/null")
            .has("NET=500500").long().timeout(300)
            .xfail(&Target::LINUX),   // dotnet build/restore fork-exec under dd — GAPS toolchain

        // ===================== unusual languages (interpreters / compilers) ====================
        // Haskell GHC: compile to native then run — ghc forks the assembler/linker (toolchain gap).
        scen("weird/haskell-ghc", "haskell:9.8")
            .exec("echo 'main = putStrLn (\"HS=\" ++ show (sum [1..1000::Int]))' > /m.hs && ghc -O2 -o /mh /m.hs >/dev/null 2>&1 && /mh")
            .has("HS=500500").long()
            .xfail(&Target::LINUX),   // ghc forks as/ld — GAPS toolchain fork-exec
        // R: vectorized numeric reduction through the R interpreter.
        scen("weird/r-compute", "r-base")
            .exec("Rscript -e 'cat(\"R=\", sum(as.numeric(1:1000)), \"\\n\", sep=\"\")'")
            .has("R=500500").long(),
        // Forth: gforth (threaded-code interpreter) sums 1..1000 via a defined word.
        scen("weird/gforth", "debian:bookworm")
            .exec("apt-get update >/dev/null 2>&1 && apt-get install -y gforth >/dev/null 2>&1; gforth -e ': sm 0 1001 1 do i + loop . ; sm cr bye'")
            .has("500500").long(),
        // Tcl: classic string/loop interpreter.
        scen("weird/tcl", "debian:bookworm")
            .exec("apt-get update >/dev/null 2>&1 && apt-get install -y tcl >/dev/null 2>&1; echo 'set s 0; for {set i 1} {$i<=1000} {incr i} {incr s $i}; puts TCL=$s' | tclsh")
            .has("TCL=500500").long(),
        // Lua (PUC-Rio reference interpreter, distinct from LuaJIT).
        scen("weird/lua", "debian:bookworm")
            .exec("apt-get update >/dev/null 2>&1 && apt-get install -y lua5.4 >/dev/null 2>&1; lua5.4 -e 's=0 for i=1,1000 do s=s+i end print(\"LUA=\"..s)'")
            .has("LUA=500500").long(),
        // Perl: heavy regex backtracking over a 300KB string (100k matches).
        scen("weird/perl-regex", "perl:5.40")
            .exec("perl -e '$s=\"abc\"x100000; $n=()=$s=~/abc/g; print \"REGEX=$n\\n\"'")
            .has("REGEX=100000"),
        // BusyBox awk: bytecode-VM text processor.
        scen("weird/awk-compute", "alpine:latest")
            .exec("awk 'BEGIN{for(i=1;i<=1000;i++)s+=i;print \"AWK=\"s}'")
            .has("AWK=500500"),

        // ======================= Python C-extensions (libcrypto / zlib / FFI) ==================
        // CPython through python:3.12-slim → xfail amd64 (jit86-opcode-1c: silent exit 255 on x86_64).
        // zlib deflate/inflate roundtrip (C extension, zlib asm).
        scen("weird/python-zlib", "python:3.12-slim")
            .exec("python3 -c \"import zlib;d=bytes(range(256))*100;print('ZLIB='+str(len(zlib.decompress(zlib.compress(d,9)))))\"")
            .has("ZLIB=25600").xfail(&[Target::AmdLinux]),
        // hashlib → OpenSSL libcrypto SHA-256 (SHA-NI / armv8 crypto-ext path).
        scen("weird/python-hashlib", "python:3.12-slim")
            .exec("python3 -c \"import hashlib;print('SHA='+hashlib.sha256(b'abc').hexdigest())\"")
            .has("SHA=ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
            .xfail(&[Target::AmdLinux]),
        // ctypes FFI: dlopen(NULL) + call libc strlen — the foreign-call / trampoline path.
        scen("weird/python-ctypes", "python:3.12-slim")
            .exec("python3 -c \"import ctypes;libc=ctypes.CDLL(None);print('CTYPES='+str(libc.strlen(b'hello')))\"")
            .has("CTYPES=5").xfail(&[Target::AmdLinux]),

        // ====================== self-modifying / dynamic code (native) =========================
        // mmap(RWX), write machine code, jump to it — the canonical SMC / DBT translate-on-execute test.
        cc("weird/self-modifying-rwx", "-O0", r#"#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(void){
  unsigned char *m=mmap(0,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(m==MAP_FAILED){perror("mmap");return 1;}
#if defined(__x86_64__)
  unsigned char code[]={0xB8,0x2A,0x00,0x00,0x00,0xC3};
#elif defined(__aarch64__)
  unsigned char code[]={0x40,0x05,0x80,0x52,0xC0,0x03,0x5F,0xD6};
#endif
  memcpy(m,code,sizeof(code));
  __builtin___clear_cache((char*)m,(char*)m+sizeof(code));
  int (*f)(void)=(int(*)(void))m;
  printf("RWX=%d\n",f());
  return 0;
}"#).has("RWX=42"),
        // OVERWRITE an already-executed RWX page with new code and re-run — forces the JIT to invalidate
        // its translated-block cache for that guest page (the killer DBT corner).
        cc("weird/smc-rewrite", "-O0", r#"#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
typedef int(*fn)(void);
int main(void){
  unsigned char *m=mmap(0,4096,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(m==MAP_FAILED){perror("mmap");return 1;}
#if defined(__x86_64__)
  unsigned char c1[]={0xB8,0x01,0x00,0x00,0x00,0xC3};
  unsigned char c2[]={0xB8,0x02,0x00,0x00,0x00,0xC3};
#elif defined(__aarch64__)
  unsigned char c1[]={0x20,0x00,0x80,0x52,0xC0,0x03,0x5F,0xD6};
  unsigned char c2[]={0x40,0x00,0x80,0x52,0xC0,0x03,0x5F,0xD6};
#endif
  memcpy(m,c1,sizeof c1); __builtin___clear_cache((char*)m,(char*)m+16);
  int r1=((fn)m)();
  memcpy(m,c2,sizeof c2); __builtin___clear_cache((char*)m,(char*)m+16);
  int r2=((fn)m)();
  printf("SMC=%d,%d\n",r1,r2); return 0;
}"#).has("SMC=1,2"),

        // ============================ signals / faults / threads (native) ======================
        // Install a SIGSEGV handler, fault on a null write, recover via siglongjmp — the JIT must
        // synthesize + deliver a guest signal from a fault in translated code, then resume.
        cc("weird/sigsegv-recover", "-O0", r#"#include <stdio.h>
#include <signal.h>
#include <setjmp.h>
static sigjmp_buf jb;
static void h(int s){ (void)s; siglongjmp(jb,1); }
int main(){
  signal(SIGSEGV,h);
  if(sigsetjmp(jb,1)==0){ volatile int *p=0; *p=1; printf("NOFAULT\n"); }
  else printf("RECOVERED\n");
  return 0;
}"#).has("RECOVERED"),
        // 8 pthreads × 100k atomic increments — TLS, clone/futex, and atomics under the translator.
        cc("weird/pthread-atomics", "-O2 -pthread", r#"#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
atomic_long c=0;
void*w(void*x){ (void)x; for(int i=0;i<100000;i++) atomic_fetch_add(&c,1); return 0; }
int main(){ pthread_t t[8]; for(int i=0;i<8;i++)pthread_create(&t[i],0,w,0);
  for(int i=0;i<8;i++)pthread_join(t[i],0);
  printf("THREADS=%ld\n",(long)c); return 0;}"#).has("THREADS=800000"),

        // ================================ exotic syscalls (native) =============================
        // memfd_create — anonymous in-memory fd.
        cc("weird/memfd-create", "-O2", r#"#define _GNU_SOURCE
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>
int main(){
  int fd=memfd_create("dd",0);
  if(fd<0){perror("memfd");return 1;}
  write(fd,"dd-memfd-ok",11);
  char b[32]={0}; lseek(fd,0,SEEK_SET); read(fd,b,11);
  printf("%s\n",b); return 0;
}"#).has("dd-memfd-ok"),
        // eventfd counter semantics (41 + 1 → read 42, reset).
        cc("weird/eventfd", "-O2", r#"#include <stdio.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <stdint.h>
int main(){
  int fd=eventfd(0,0);
  uint64_t v=41; write(fd,&v,8); v=1; write(fd,&v,8);
  uint64_t r; read(fd,&r,8);
  printf("EVENTFD=%llu\n",(unsigned long long)r); return 0;
}"#).has("EVENTFD=42"),
        // timerfd: one 50ms one-shot expiration → read returns 1.
        cc("weird/timerfd", "-O2", r#"#include <stdio.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <stdint.h>
int main(){
  int fd=timerfd_create(CLOCK_MONOTONIC,0);
  if(fd<0){perror("timerfd");return 1;}
  struct itimerspec its={{0,0},{0,50000000}};
  timerfd_settime(fd,0,&its,0);
  uint64_t e; read(fd,&e,8);
  printf("TIMERFD=%llu\n",(unsigned long long)e); return 0;
}"#).has("TIMERFD=1"),
        // inotify: watch /tmp, create a file, read the IN_CREATE event.
        cc("weird/inotify", "-O2", r#"#define _GNU_SOURCE
#include <stdio.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <fcntl.h>
int main(){
  int fd=inotify_init1(0);
  if(fd<0){perror("inotify");return 1;}
  inotify_add_watch(fd,"/tmp",IN_CREATE);
  int f=open("/tmp/dd-inotify",O_CREAT|O_WRONLY,0644); close(f);
  char buf[4096]; read(fd,buf,sizeof buf);
  struct inotify_event *ev=(void*)buf;
  printf("INOTIFY=%s\n", ev->len?ev->name:"none"); return 0;
}"#).has("INOTIFY=dd-inotify"),
        // prctl PR_SET_NAME / PR_GET_NAME round-trip.
        cc("weird/prctl-name", "-O2", r#"#include <stdio.h>
#include <sys/prctl.h>
int main(){
  prctl(PR_SET_NAME,"dd-proc");
  char n[16]={0}; prctl(PR_GET_NAME,n);
  printf("PRCTL=%s\n",n); return 0;
}"#).has("PRCTL=dd-proc"),
        // getrandom(2): 16 bytes from the kernel CSPRNG.
        cc("weird/getrandom", "-O2", r#"#define _GNU_SOURCE
#include <stdio.h>
#include <sys/random.h>
int main(){char b[16]; ssize_t n=getrandom(b,16,0); printf("GETRANDOM=%zd\n",n); return 0;}"#)
            .has("GETRANDOM=16"),
        // ptrace(PTRACE_TRACEME): the anti-debug / tracer-attach primitive.
        cc("weird/ptrace-traceme", "-O2", r#"#include <stdio.h>
#include <sys/ptrace.h>
int main(){
  long r=ptrace(PTRACE_TRACEME,0,0,0);
  printf("PTRACE=%ld\n", r); return 0;
}"#).has("PTRACE=0"),
        // sched_getaffinity: CPU-mask population (same family as the mongo cpu-topology gap).
        cc("weird/sched-affinity", "-O2", r#"#define _GNU_SOURCE
#include <stdio.h>
#include <sched.h>
int main(){
  cpu_set_t s; CPU_ZERO(&s);
  if(sched_getaffinity(0,sizeof s,&s)<0){perror("aff");return 1;}
  printf("AFFINITY=%s\n", CPU_COUNT(&s)>0?"nonzero":"zero"); return 0;
}"#).has("AFFINITY=nonzero"),
        // userfaultfd: Docker's default seccomp returns EPERM — a deterministic blocked-syscall probe.
        cc("weird/userfaultfd", "-O2", r#"#define _GNU_SOURCE
#include <stdio.h>
#include <linux/userfaultfd.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>
int main(){
  int fd=syscall(SYS_userfaultfd,0);
  if(fd<0){perror("userfaultfd");return 1;}
  struct uffdio_api api={.api=UFFD_API};
  if(ioctl(fd,UFFDIO_API,&api)<0){perror("api");return 1;}
  printf("UFFD=ok\n"); return 0;
}"#).has("userfaultfd: Operation not permitted"),
        // io_uring_setup: also EPERM under Docker default seccomp.
        cc("weird/io-uring", "-O2", r#"#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <linux/io_uring.h>
#include <unistd.h>
int main(){
  struct io_uring_params p; memset(&p,0,sizeof p);
  int fd=syscall(SYS_io_uring_setup,8,&p);
  if(fd<0){perror("io_uring_setup");return 1;}
  printf("IOURING=ok sq=%u\n",p.sq_entries); return 0;
}"#).has("io_uring_setup: Operation not permitted"),
        // bpf(BPF_MAP_CREATE): needs CAP_BPF — EPERM under default Docker.
        cc("weird/bpf-map-create", "-O2", r#"#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <linux/bpf.h>
#include <unistd.h>
int main(){
  union bpf_attr a; memset(&a,0,sizeof a);
  a.map_type=BPF_MAP_TYPE_ARRAY; a.key_size=4; a.value_size=4; a.max_entries=1;
  long r=syscall(SYS_bpf,BPF_MAP_CREATE,&a,sizeof a);
  printf("BPF=%s\n", r<0?strerror(errno):"ok"); return 0;
}"#).has("BPF=Operation not permitted"),
        // seccomp(2): install a (allow-all) BPF filter — the sandboxing self-restriction path.
        cc("weird/seccomp-filter", "-O2", r#"#define _GNU_SOURCE
#include <stdio.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(){
  struct sock_filter f[]={ BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW) };
  struct sock_fprog prog={.len=1,.filter=f};
  prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0);
  if(syscall(SYS_seccomp,SECCOMP_SET_MODE_FILTER,0,&prog)<0){perror("seccomp");return 1;}
  printf("SECCOMP=installed\n"); return 0;
}"#).has("SECCOMP=installed"),

        // ================================== CPU features =======================================
        // getauxval: exercise the auxv-reading path. AT_HWCAP bit semantics differ by arch (and QEMU
        // amd64 reports a sparse mask), so assert on AT_PAGESZ which is a stable 4096 on both arches.
        cc("weird/auxval-hwcap", "-O2", r#"#include <stdio.h>
#include <sys/auxv.h>
int main(){
  unsigned long hw=getauxval(AT_HWCAP);   /* exercise AT_HWCAP read */
  unsigned long ps=getauxval(AT_PAGESZ);
  (void)hw;
  printf("AUXV=%lu\n", ps); return 0;
}"#).has("AUXV=4096"),
        // SIMD dispatch: x86 executes the `cpuid` instruction (a classic translator trap) to detect
        // SSE2 (EDX bit26 — baseline-guaranteed on every x86-64, incl. QEMU); arm reads HWCAP_ASIMD.
        cc("weird/simd-probe", "-O2", r#"#include <stdio.h>
#if defined(__x86_64__)
#include <cpuid.h>
int main(){ unsigned a,b,c,d; __get_cpuid(1,&a,&b,&c,&d);
  printf("SIMD=%s\n",(d&(1u<<26))?"ok":"no"); return 0; }
#elif defined(__aarch64__)
#include <sys/auxv.h>
int main(){ unsigned long h=getauxval(AT_HWCAP);
  printf("SIMD=%s\n",(h&(1u<<1))?"ok":"no"); return 0; }
#endif"#).has("SIMD=ok"),
        // Timestamp counter: x86 `rdtsc` / arm `mrs cntvct_el0` inline asm read monotonic — the JIT
        // must emulate the privileged-ish counter read.
        cc("weird/tsc-counter", "-O2", r#"#include <stdio.h>
#include <stdint.h>
static inline uint64_t rd(){
#if defined(__x86_64__)
 unsigned lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); return ((uint64_t)hi<<32)|lo;
#elif defined(__aarch64__)
 uint64_t v; __asm__ volatile("mrs %0, cntvct_el0":"=r"(v)); return v;
#endif
}
int main(){uint64_t a=rd(); for(volatile long i=0;i<100000;i++); uint64_t b=rd();
 printf("TSC=%s\n", b>=a?"ok":"no"); return 0;}"#).has("TSC=ok"),
        // vDSO: clock_gettime(CLOCK_MONOTONIC) is serviced via the vDSO mapping (no real syscall);
        // the translator must run vDSO code correctly and keep the clock monotonic.
        cc("weird/vdso-clock", "-O2", r#"#include <stdio.h>
#include <time.h>
int main(){struct timespec a,b; clock_gettime(CLOCK_MONOTONIC,&a);
  for(volatile long i=0;i<1000000;i++); clock_gettime(CLOCK_MONOTONIC,&b);
  printf("CLOCK=%s\n",(b.tv_sec>a.tv_sec||(b.tv_sec==a.tv_sec&&b.tv_nsec>=a.tv_nsec))?"ok":"no"); return 0;}"#)
            .has("CLOCK=ok"),
        // Plain compile-and-run canary: documents the toolchain (cc1/as/ld fork-exec) gap directly,
        // separate from any syscall — XPASS here means `cc` works under dd again.
        cc("weird/cc-canary", "-O2", r#"#include <stdio.h>
int main(){ long s=0; for(long i=1;i<=1000;i++) s+=i; printf("CC=%ld\n", s); return 0; }"#)
            .has("CC=500500"),
        // nproc: online-CPU count (shell, no compiler) — isolates cpu-topology from the toolchain.
        scen("weird/nproc", "alpine:latest")
            .exec("[ \"$(nproc)\" -ge 1 ] && echo NPROC-OK || echo NPROC-BAD")
            .has("NPROC-OK"),
        // /proc/cpuinfo presence + feature line (shell).
        scen("weird/proc-cpuinfo", "alpine:latest")
            .exec("grep -qE '^(processor|Features|flags)' /proc/cpuinfo && echo CPUINFO-OK || echo CPUINFO-NO")
            .has("CPUINFO-OK"),

        // ============================ compression / crypto codegen =============================
        // gzip (zlib deflate) roundtrip — the value survives compress+decompress.
        scen("weird/gzip-roundtrip", "alpine:latest")
            .exec("seq 1 1000 | gzip -9 | gzip -d | awk '{s+=$1}END{print \"GZIP=\"s}'")
            .has("GZIP=500500"),
        // bzip2 (Burrows-Wheeler) roundtrip.
        scen("weird/bzip2-roundtrip", "alpine:latest")
            .exec("seq 1 1000 | bzip2 -9 | bzip2 -d | awk '{s+=$1}END{print \"BZIP=\"s}'")
            .has("BZIP=500500"),
        // xz (LZMA) roundtrip.
        scen("weird/xz-roundtrip", "debian:bookworm")
            .exec("apt-get update >/dev/null 2>&1 && apt-get install -y xz-utils >/dev/null 2>&1; seq 1 1000 | xz -9 | xz -d | awk '{s+=$1}END{print \"XZ=\"s}'")
            .has("XZ=500500").long(),
        // zstd roundtrip.
        scen("weird/zstd-roundtrip", "debian:bookworm")
            .exec("apt-get update >/dev/null 2>&1 && apt-get install -y zstd >/dev/null 2>&1; seq 1 1000 | zstd -19 2>/dev/null | zstd -d 2>/dev/null | awk '{s+=$1}END{print \"ZSTD=\"s}'")
            .has("ZSTD=500500").long(),
        // OpenSSL SHA-256 NIST vector — libcrypto SHA-NI / armv8 crypto-extension asm.
        scen("weird/openssl-sha256", "alpine:latest")
            .exec("apk add --no-cache openssl >/dev/null 2>&1; printf abc | openssl dgst -sha256 | awk '{print $NF}'")
            .has("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad").long(),
        // OpenSSL SHA-512 NIST vector.
        scen("weird/openssl-sha512", "alpine:latest")
            .exec("apk add --no-cache openssl >/dev/null 2>&1; printf abc | openssl dgst -sha512 | awk '{print $NF}'")
            .has("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f").long(),
        // OpenSSL speed: a bounded tight AES asm loop (AES-NI / armv8 AES).
        scen("weird/openssl-speed", "alpine:latest")
            .exec("apk add --no-cache openssl >/dev/null 2>&1; openssl speed -seconds 1 -evp aes-128-cbc 2>&1 | grep -q Doing && echo SPEED-OK || echo SPEED-FAIL")
            .has("SPEED-OK").long(),

        // ============================== seed: documented gaps ==================================
        // mongod aborts on empty possible-CPU set (tcmalloc) — a CPU-topology syscall gap. xfail+GAPS.
        scen("weird/mongo-cpu-topology", "mongo")
            .run(&["mongod", "--version"]).has("db version")
            .xfail(&[Target::ArmLinux, Target::AmdLinux]),   // NumPossibleCPUs empty — GAPS.md
        // hello-world: the canonical static non-PIE ET_EXEC binary — the exec/loader gap's repro. xfail.
        scen("weird/static-nonpie-helloworld", "hello-world")
            .run(&[]).has("Hello from Docker")
            .xfail(&[Target::ArmLinux, Target::AmdLinux]),   // non-PIE exec loader gap — GAPS.md
    ])
}
