//! The test registry — declarative groups of cases. Add a case by adding a line.
//!
//! `src(name, file)`         compile `guests/<file>` (aarch64) and run it bare.
//! `.oracle()`               diff the JIT run's stdout+exit against running the same binary natively.
//! `.exit(n)/.out(s)/.has(s)` golden checks.
//! `in_rootfs(name, img, a)` run a program already inside an image's rootfs (container behaviour).
//! `fixture(name, &[(e,p)])` run a prebuilt binary on engine `e` (the only way to exercise x86-64 now).

use crate::{darwin_libc, darwin_src, fixture, group, in_rootfs, port, src, Case, Engine, Group};

pub mod ext;   // per-category basics expansion (one file per agent, appended below)

/// Every group, in display order. Base groups here + the per-agent extension groups in `ext`.
pub fn all() -> Vec<Group> {
    let mut g = vec![compat(), libc(), system(), net(), proc(), threads(), posix(), ipc(), clib(), linuxsys(),
         heavy(), soak(), edge(), compile(), realsw(), containersw(), perf(), busybox(), container(), sandbox(), x86(), darwin()];
    g.extend(ext::all());
    g
}

/// Threads — mutex/condvar producer-consumer, 64-way contention, and thread-local storage. Portable
/// across engines (Linux x2 + macOS), golden-checked. Proves the threading model is sound everywhere.
fn threads() -> Group {
    group("threads", vec![
        port("mutex", "threads_mutex.c").out("queue produced=40000 consumed=40000\n"), // mutex + condvar
        port("contention", "threads_many.c").out("threads mutex=640000 atomic=640000\n"), // 64 threads
        port("tls", "tls.c").out("tls ok=8\n"),                                          // __thread storage
    ])
}

/// IPC & fd plumbing — named pipes, POSIX + System V shared memory/semaphores, dup2 redirection,
/// fcntl flag commands. Portable across engines, golden-checked.
fn ipc() -> Group {
    group("ipc", vec![
        port("fifo", "mkfifo.c").out("fifo sum=125250\n"),                  // named pipe across fork
        port("shm-posix", "shmposix.c").out("shmposix sum=5559680\n"),       // shm_open + MAP_SHARED
        port("shm-sysv", "sysvshm.c").out("sysvshm sum=131328\n"),           // shmget/semget handshake
        port("dup2", "dup2redir.c").out("dup2 file=captured-line\n"),        // dup/dup2 redirection
        port("fcntl", "fcntlflags.c").out("fcntl dupfd=1 cloexec=1 nonblock=1\n"), // F_DUPFD/SETFD/SETFL
    ])
}

/// libc breadth & C-runtime behaviour — regex, glob, float parsing, calendar math, environment, exit
/// handlers, and signal control flow. Portable across engines, golden-checked.
fn clib() -> Group {
    group("clib", vec![
        port("regex", "regex.c").out("regex hit=1 group=2026 miss=1\n"),
        port("glob", "globmatch.c").out("glob txt=3 all=5\n"),
        port("strtod", "strtod.c").out("strtod pi=1 sci=1 hex=1 inf=1 acc=10000\n"),
        port("timefmt", "timefmt.c").out("timefmt fmt=1 roundtrip=1 wday=2\n"),
        port("environ", "environ.c").out("environ set=1 nooverwrite=1 overwrite=1 unset=1 haspath=1\n"),
        port("atexit", "atexit.c").out("atexit order=cba"),
        port("sigaction", "sigaction2.c").out("sigaction usr1=1 signo_ok=1 chld=1\n"), // SA_SIGINFO + SIGCHLD
        port("sigjmp", "sigjmp.c").out("sigjmp hops=3 from=3\n"),                    // sigsetjmp/siglongjmp
    ])
}

/// Networking — sockets over loopback (TCP/UDP/UNIX), client+server across a fork. PORTABLE: the one
/// POSIX source runs on every engine (Linux x2 + macOS), golden-checked so the behaviour must be
/// byte-identical across platforms. The acid test that a real networked service (postgres/redis shape)
/// works the same emulated-on-Linux and native-on-macOS.
fn net() -> Group {
    group("net", vec![
        port("tcp", "net_tcp.c").out("tcp echo=HELLO-SOCKET exit=0\n"), // socket/bind/listen/accept/connect
        port("udp", "net_udp.c").out("udp echo=datagram-42\n"),          // SOCK_DGRAM sendto/recvfrom
        port("unix", "net_unix.c").out("unix reply=sum=335\n"),          // AF_UNIX socketpair full-duplex
        port("sockopt", "net_sockopt.c").out("sockopt reuse=1 nodelay=1 soerr=0 ok=1\n"), // get/setsockopt
        port("nonblock", "net_nonblock.c").out("nonblock inprogress=1 writable=1 soerr=0\n"), // async connect
        port("sendmsg", "net_sendmsg.c").out("sendmsg sent=6 got=6 data=ABCDEF\n"), // sendmsg/recvmsg iovec
    ])
}

/// Process trees — fork/wait/exit-status propagation and parent<->child pipes. Portable across engines.
fn proc() -> Group {
    group("proc", vec![
        port("forkwait", "forkwait.c").out("forkwait reaped=8 sum=36\n"), // fork 8, reap, sum exit codes
        port("pipeproc", "pipeproc.c").out("pipeproc sum=500500\n"),       // producer/consumer over a pipe
    ])
}

/// Portable POSIX syscalls — the event/IO/IPC surface a real daemon leans on, expressed in pure POSIX
/// so it runs (and must agree) on Linux AND macOS. Golden-checked.
fn posix() -> Group {
    group("posix", vec![
        port("pollselect", "pollselect.c").out("poll=1 select=1 timeout=1\n"), // poll() + select() + timeout
        port("mmapshared", "mmapshared.c").out("mmapshared ok=1 sum=520192\n"), // file-backed MAP_SHARED + msync
        port("filelock", "filelock.c").out("filelock blocked=1 free_after=1\n"), // fcntl F_SETLK/F_GETLK + fork
        port("clock", "clockmono.c").has("mono_ok=1 slept_ge=1 realtime_ok=1"),  // clock_gettime + nanosleep
        port("realpath", "realpath.c").out("realpath readlink=1 resolve=1\n"),   // symlink/readlink/realpath
        port("getdents", "getdents.c").out("getdents count=4 namechk=2306\n"),   // opendir/readdir
        port("tmpfile", "tmpfile.c").out("tmpfile mkstemp=1 data=1 tmpfile=1 val=4242\n"), // mkstemp/tmpfile
        port("statvfs", "statvfs.c").out("statvfs ok=1 bsize_pow2=1 blocks_ok=1 consistent=1\n"), // statfs
    ])
}

/// Linux-specific syscalls — epoll/eventfd/signalfd/inotify/sendfile/getrandom have no portable POSIX
/// form (macOS uses kqueue/getentropy), so they're Linux-engine only and diffed against a native oracle.
/// On the macOS-hosted runtime these are emulated (kqueue/pipe), so this group is where emulation gaps
/// surface against real-Linux ground truth.
fn linuxsys() -> Group {
    group("linuxsys", vec![
        src("epoll", "epoll.c").oracle(),           // epoll_create1/ctl/wait readiness loop
        src("eventfd", "eventfd.c").oracle(),       // eventfd2 counter semantics
        src("eventfd-sema", "eventfd_sema.c").oracle(), // EFD_SEMAPHORE decrement-by-1 contract
        src("signalfd", "signalfd.c").oracle(),     // sigprocmask + signalfd4 read of a raised signal
        src("inotify", "inotify.c").oracle(),       // inotify watch -> IN_CREATE event read
        src("sendfile", "sendfile.c").oracle(),     // sendfile + readv/writev scatter-gather
        src("timerfd", "timerfd.c").oracle(),       // timerfd one-shot expiration
        src("memfd", "memfd.c").oracle(),           // memfd_create + ftruncate + mmap
        src("splice", "splice.c").oracle(),         // splice file->pipe->file (zero copy)
        src("prctl", "prctl.c").oracle(),           // prctl PR_SET_NAME/PR_GET_NAME
        src("sigqueue", "sigqueue.c").oracle(),     // realtime signal payload via si_value
        src("getrandom", "getrandom.c").has("nonzero=1 differ=1"), // entropy verdict (bytes not reproducible)
    ])
}

/// Long-running / heavy-footprint workloads — a sustained compute loop and a large sparse mmap (both
/// portable), plus a postgres-shaped networked DB service. The "is it actually stable under load" tier.
fn heavy() -> Group {
    group("heavy", vec![
        port("busyloop", "busyloop.c").out("busyloop acc=14881893564601462335\n"), // 300M-iter mixing loop
        port("bigmem", "bigmem.c").out("bigmem pages=131072 sum=16711680\n"),       // mmap 512 MiB, fault pages
        // fork-per-connection TCP server backed by real SQLite (WAL) + a 50-connection client (links
        // libsqlite3 -> Linux/aarch64 only), diffed against a native run.
        src("dbserver", "dbserver.c").only(&[Engine::LinuxAarch64]).oracle(),
    ])
}

/// EDGE — obscure syscall corners mined from reading os/linux/service.c + the frontends: flag bits,
/// ancillary data, sparse-file/seek semantics, abstract sockets, packet pipes, and protection faults
/// that a runtime is easy to get subtly wrong. Differential vs native (oracle) where output matches
/// real Linux; golden verdict where it's a yes/no. These are where production software trips.
fn edge() -> Group {
    // Confirmed JIT divergences vs real Linux (the engine-fixing lane owns these — see PLAN.md "Edge").
    // xfail-tracked so the gate stays green and XPASS fires the moment one is fixed. `msgflags`
    // (MSG_PEEK/MSG_DONTWAIT) is the only one that already matches Linux.
    let lin = &[Engine::LinuxAarch64, Engine::LinuxX86_64];
    group("edge", vec![
        src("madvise", "edge_madvise.c").oracle(),       // MADV_DONTNEED must drop anon pages
        src("renameat2", "edge_renameat2.c").oracle(),   // RENAME_NOREPLACE / RENAME_EXCHANGE
        src("scmrights", "edge_scmrights.c").oracle(),             // fd passing over AF_UNIX (SCM_RIGHTS) — FIXED (cmsg l2m/m2l)
        src("fallocate", "edge_fallocate.c").oracle(),   // FALLOC_FL_PUNCH_HOLE sparse hole
        src("lseekhole", "edge_lseekhole.c").oracle(),   // SEEK_HOLE / SEEK_DATA
        src("otmpfile", "edge_otmpfile.c").oracle().xfail(lin),     // O_TMPFILE unnamed file
        src("pipepacket", "edge_pipepacket.c").oracle(), // pipe2(O_DIRECT) packet boundaries
        src("msgflags", "edge_msgpeek.c").oracle(),                 // recv MSG_PEEK + MSG_DONTWAIT — WORKS
        src("abstract", "edge_abstract.c").oracle(),               // abstract-namespace AF_UNIX — FIXED (DD_NETNS fs-socket map)
        src("pipesz", "edge_pipesz.c").oracle(),                    // F_SET/GETPIPE_SZ (shadow-table emulation) + dup3 self-dup — FIXED
        // mprotect: portable — darwin (native) FAULTS correctly, the JIT no-ops it; xfail only Linux so
        // the darwin pass / Linux fail contrast is explicit.
        port("mprotect", "edge_mprotect.c").out("mprotect faulted=1 readable_after=1\n").xfail(lin),
        // clock_nanosleep TIMER_ABSTIME: emulated as (deadline - now) with an EINTR-recompute loop —
        // FIXED. Pinned to one engine to bound cost; previously hung to the 25s timeout when treated
        // as relative.
        src("clockabstime", "edge_clockabstime.c").only(&[Engine::LinuxAarch64]).has("abstime_ok=1"),
        src("sigpipe", "edge_sigpipe.c").has("survived=1 epipe=1"), // SO_NOSIGPIPE set on every guest socket at creation (socket/socketpair/accept) -> write/send to a broken socket returns EPIPE, never a fatal SIGPIPE
        src("procfd", "edge_procfd.c").has("resolves=1 enough_fds=1").xfail(lin), // /proc/self/fd
        // times(): tms_utime works on x86_64 but is 0 on aarch64 (clock() works on both) — engine split.
        src("times", "edge_times.c").has("utime_ok=1 clock_ok=1 ret_ok=1"),
        src("statfs", "edge_statfs.c").oracle().xfail(lin),                            // real fs geometry (not hardcoded)
    ])
}

/// SOAK / endurance — workloads that run for a sustained stretch and only fail through the JIT's
/// long-run machinery: code-cache recycling, block-chaining/IBTC drift, self-modifying-code
/// re-translation, and resource churn (threads/forks/heap) accumulating over thousands of cycles.
/// These catch bugs a short test never reaches. Deterministic -> golden (portable) / oracle (smc).
fn soak() -> Group {
    group("soak", vec![
        port("codecache", "soak_codecache.c").out("soak codecache acc=5966323930328914303\n"), // 256 blocks, 120M iters
        port("indirect", "soak_indirect.c").out("soak indirect acc=4633281659943884454\n"),    // 64-target IBTC, 80M iters
        port("threadchurn", "soak_threadchurn.c").out("soak threadchurn total=14000000\n"),     // 4000 short threads
        port("forkchurn", "soak_forkchurn.c").out("soak forkchurn reaped=3000 acc=151500\n"),   // 3000 fork/wait
        port("allocchurn", "soak_allocchurn.c").out("soak allocchurn sum=1529986411\n"),        // 6M malloc/free
        // self-modifying code: patch+flush+call an RWX page 200k times -> re-translation churn. aarch64
        // machine code, so Linux/aarch64 only (the real JIT path); diffed vs native. xfail: mmap(RWX)
        // is EPERM under macOS W^X (no MAP_JIT) -> guest-JIT runtimes can't get exec pages (see PLAN.md).
        src("smc", "soak_smc.c").only(&[Engine::LinuxAarch64]).oracle(),
    ])
}

/// COMPILE — the worst-case technical workload an engineer runs: a real GCC-14 toolchain
/// (gcc -> cc1/cc1plus -> as -> collect2/ld, a deep fork/exec/pipe pipeline over hundreds of MB of
/// headers/libs) compiling C and C++ *inside the container*, then running the result and checking its
/// output. If the JIT can host a compiler building+running correct code, it can host almost anything.
/// Sources are embedded base64 (self-contained, no host source needed). Linux/aarch64 (the gcc image).
fn compile() -> Group {
    // The GCC driver currently segfaults under the aarch64 JIT (a large dynamically-linked C++ binary;
    // no missing-syscall/UNIMPL diagnostic -> codegen/runtime bug — perl, another big dynamic binary,
    // works). Marked xfail so the gap is tracked (XPASS will fire the moment the engine fixes it).
    let gcc = |name, sh| in_rootfs(name, "gcc-bundle", &["/bin/sh", "-c", sh]).xfail(&[Engine::LinuxAarch64]);
    group("compile", vec![
        // the staged /hello.c — gcc -> cc1 -> as -> ld -> run.
        gcc("hello", "cd /tmp && gcc-14 -O2 -o h /hello.c && ./h && rm -f h").has("compiled by gcc"),
        // a prime sieve (pure integer -> optimizer-independent output); proves compiled code is correct.
        gcc("c-primes", "cd /tmp && echo I2luY2x1ZGUgPHN0ZGlvLmg+CmludCBtYWluKHZvaWQpe2ludCBjPTA7Zm9yKGludCBuPTI7bjwxMDAwMDA7bisrKXtpbnQgcD0xO2ZvcihpbnQgZD0yO2QqZDw9bjtkKyspaWYobiVkPT0wKXtwPTA7YnJlYWs7fWMrPXA7fXByaW50ZigicHJpbWVzPSVkXG4iLGMpO3JldHVybiAwO30K \
            | base64 -d > p.c && gcc-14 -O2 -o p p.c && ./p && rm -f p p.c").has("primes=9592"),
        // C++: g++ -> cc1plus -> libstdc++ (vector/sort/string) -- the heavy template+STL link path.
        gcc("cpp-stl", "cd /tmp && echo I2luY2x1ZGUgPHZlY3Rvcj4KI2luY2x1ZGUgPGFsZ29yaXRobT4KI2luY2x1ZGUgPHN0cmluZz4KI2luY2x1ZGUgPGNzdGRpbz4KaW50IG1haW4oKXsKICBzdGQ6OnZlY3Rvcjxsb25nPiB2OwogIGZvcihpbnQgaT0wO2k8MTAwMDAwO2krKykgdi5wdXNoX2JhY2soKGxvbmcpKChpKjI2NTQ0MzU3NjF1KSUxMDAwMDAzKSk7CiAgc3RkOjpzb3J0KHYuYmVnaW4oKSwgdi5lbmQoKSk7CiAgbG9uZyBzPTA7IGZvcihsb25nIHg6IHYpIHMrPXg7CiAgc3RkOjpzdHJpbmcgYT0iZGQiLCBiPSItY3BwIjsgYSs9YjsKICBwcmludGYoImNwcCBuPSV6dSBzdW09JWxkIG1lZD0lbGQgcz0lc1xuIiwgdi5zaXplKCksIHMsIHZbdi5zaXplKCkvMl0sIGEuY19zdHIoKSk7CiAgcmV0dXJuIDA7Cn0K \
            | base64 -d > p.cpp && g++-14 -O2 -o p p.cpp && ./p && rm -f p p.cpp").has("cpp n=100000 sum=50002557337 med=500032 s=dd-cpp"),
    ])
}

/// Real software inside a container — busybox applets doing networked + long-running + compression
/// work in the alpine rootfs. Exercises the container path (rootfs jail, private-loopback netns, fork/
/// exec of real binaries) the way an actual workload would. Linux/aarch64 (containers are Linux).
fn containersw() -> Group {
    group("containersw", vec![
        // busybox nc over the container's private loopback: a listener writes what it receives to a
        // file, a client sends a line, then we read it back. Exercises the netns lo_* unix-socket path.
        sh("nc-loopback", "(nc -l -p 18080 > /tmp/srv.out &) ; sleep 1; echo hello-nc | nc 127.0.0.1 18080; \
            sleep 1; cat /tmp/srv.out; rm -f /tmp/srv.out").has("hello-nc"),
        // gzip/gunzip roundtrip through a pipe (real DEFLATE, fork of two applets).
        sh("gzip", "echo compress-me-12345 > /tmp/d; gzip -c /tmp/d | gunzip -c; rm -f /tmp/d")
            .out("compress-me-12345\n"),
        // tar a directory to stdout and untar it elsewhere (streamed archive, fork/exec + fs churn).
        sh("tar", "cd /tmp; rm -rf ta tb; mkdir ta tb; echo content-X > ta/f1; tar cf - ta | (cd tb; tar xf -); \
            cat tb/ta/f1; rm -rf ta tb").has("content-X"),
        // a long-running shell arithmetic loop (200k iterations of ash $((...)) -- a multi-second guest).
        sh("longshell", "i=0; s=0; while [ $i -lt 200000 ]; do s=$((s+i)); i=$((i+1)); done; echo sum=$s")
            .has("sum=19999900000"),
    ])
}

/// Real software — the actual upstream engines, static-linked, doing real work. The acid test that the
/// runtime handles production code (file I/O, mmap, fsync, locking, libc breadth), not just microbench.
fn realsw() -> Group {
    group("realsw", vec![
        // SQLite 3: WAL, a 5000-row transaction, then an aggregate query. Diffed against a native run.
        src("sqlite", "sqlite.c").arg("/tmp/dd_sqlite_test.db").only(&[Engine::LinuxAarch64]).oracle(),
        // Perl 5 (the real Ubuntu interpreter): a prime sieve up to 10k -- heavy interpreter loop +
        // dynamic dispatch. 1229 primes, last is 9973.
        in_rootfs("perl-sieve", "ubuntu", &["/usr/bin/perl", "-e",
            "my @p; for my $n (2..10000){ my $pr=1; for my $d (2..int(sqrt($n))){ $pr=0,last if $n%$d==0 } \
             push @p,$n if $pr } print \"primes=\".scalar(@p).\" last=$p[-1]\\n\""])
            .has("primes=1229 last=9973"),
        // Intensive container-fs IO: create/write/read 200 files in a tight shell loop (open/write/close/
        // readdir/unlink churn through the rootfs jail).
        in_rootfs("io-churn", "ubuntu", &["/bin/sh", "-c",
            "cd /tmp && rm -rf io && mkdir io && cd io && i=0; while [ $i -lt 200 ]; do echo data-$i payload > f$i; \
             i=$((i+1)); done; cat f* | wc -l; cd /; rm -rf /tmp/io"])
            .has("200"),
    ])
}

/// Run `sh -c <cmd>` inside the alpine rootfs (the workhorse for container/busybox/sandbox cases).
fn sh(name: &'static str, cmd: &'static str) -> Case {
    in_rootfs(name, "alpine", &["/bin/sh", "-c", cmd])
}

/// Core ABI / codegen — compiled guests, diffed against a native oracle.
fn compat() -> Group {
    group("compat", vec![
        src("hello", "hello.c").exit(42).out("hi\n"),
        src("math", "math.c").oracle(),
        src("strings", "strings.c").oracle(),
        src("bitops", "bitops.c").oracle(),          // popcount/clz/ctz/bswap
        src("varargs", "varargs.c").oracle(),         // stdarg + snprintf formats
        src("longjmp", "longjmp.c").out("longjmp r=42\n"),
        src("recursion", "recursion.c").oracle(),     // fib(30) + ackermann (§B depth gate)
        src("fnptr", "fnptr.c").oracle(),             // function pointers -> IBTC / inline cache
        src("jumptable", "jumptable.c").oracle(),     // dense switch -> jump table
        src("floatmath", "floatmath.c").oracle(),
    ])
}

/// libc-heavy paths.
fn libc() -> Group {
    group("libc", vec![
        src("heap", "heap.c").oracle(),
        src("qsort", "qsort.c").oracle(),
        src("files", "files.c").out("files n=7 data=payload\n"),
        src("statfile", "statfile.c").oracle(),       // open/write/stat/access/unlink
        src("pipe", "pipe.c").out("pipe n=10 piped-data\n"),
        src("mmapanon", "mmapanon.c").oracle(),       // mmap/munmap anon
    ])
}

/// Threads, signals, syscalls.
fn system() -> Group {
    group("system", vec![
        src("threads", "threads.c").out("threads sum=800000\n"),
        src("atomics", "atomics.c").out("atomic v=1000000\n"),
        src("signals", "signals.c").out("signal got=12\n"),       // SIGUSR2 = 12
        src("sysinfo", "sysinfo.c").has("sys=Linux pid_ok=1"),    // uname + getpid
        src("shm", "shm.c").out("SHM-ROUNDTRIP-OK\n"),            // SysV shared memory get/at/dt/ctl
        src("sem", "sem.c").out("SEM v=0 w=1\n"),                 // SysV semaphores get/op/ctl
        src("msg", "msg.c").out("MSG=MSG-PAYLOAD\n"),             // SysV message queues get/snd/rcv/ctl
    ])
}

/// Heavier workloads (also exercise the timing column).
fn perf() -> Group {
    group("perf", vec![
        src("sortbig", "sortbig.c").oracle(),         // qsort 300k longs
    ])
}

/// busybox applets inside the alpine rootfs — golden output (aarch64 image).
fn busybox() -> Group {
    group("busybox", vec![
        sh("echo", "echo hello world").out("hello world\n"),
        sh("printf", "printf '%d-%s\\n' 42 hi").out("42-hi\n"),
        sh("expr", "expr 6 \\* 7").out("42\n"),
        sh("seq", "seq 1 5 | tr '\\n' ' '").out("1 2 3 4 5 "),
        sh("wc", "printf 'a\\nb\\nc\\n' | wc -l").has("3"),
        sh("tr", "echo hello | tr a-z A-Z").out("HELLO\n"),
        sh("cut", "echo a:b:c | cut -d: -f2").out("b\n"),
        sh("head", "seq 1 100 | head -3 | tr '\\n' ' '").out("1 2 3 "),
        sh("tail", "seq 1 100 | tail -2 | tr '\\n' ' '").out("99 100 "),
        sh("rev", "echo abc | rev").out("cba\n"),
        sh("sort", "printf 'c\\nb\\na\\n' | sort | tr '\\n' ' '").out("a b c "),
        sh("uniq", "printf 'a\\na\\nb\\n' | uniq | tr '\\n' ' '").out("a b "),
        sh("grep", "printf 'foo\\nbar\\n' | grep bar").out("bar\n"),
        sh("sed", "echo hello | sed s/l/L/g").out("heLLo\n"),
        sh("awk", "echo '3 4' | awk '{print $1+$2}'").out("7\n"),
        sh("basename", "basename /a/b/c.txt").out("c.txt\n"),
        sh("dirname", "dirname /a/b/c").out("/a/b\n"),
        sh("base64", "printf abc | base64").out("YWJj\n"),
        sh("md5", "printf abc | md5sum").has("900150983cd24fb0d6963f7d28e17f72"),
        sh("find", "find /etc -name hostname 2>/dev/null | head -1").has("/etc/hostname"),
    ])
}

/// Container behaviour — namespaces, fs, limits (alpine, aarch64).
fn container() -> Group {
    group("container", vec![
        sh("id-root", "id -u").out("0\n"),                                  // USER-ns: root by default
        sh("uname-m", "uname -m").out("aarch64\n"),
        sh("pwd", "pwd").out("/\n"),
        // write cases clean up after themselves (the image rootfs is shared — keep it pristine).
        sh("mkdir", "mkdir -p /x/y && echo /x/y; rm -rf /x").out("/x/y\n"),
        sh("chmod", "rm -f /f; touch /f && chmod 700 /f && stat -c %a /f; rm -f /f").out("700\n"),
        sh("symlink", "rm -f /l; ln -s /etc/hostname /l && readlink /l; rm -f /l").out("/etc/hostname\n"),
        sh("proc-self", "test -r /proc/self/status && echo proc-ok").out("proc-ok\n"),
        sh("dev-null", "echo discard > /dev/null && echo dev-ok").out("dev-ok\n"),
        // /dev completeness (#265): fd/std* symlinks + ptmx/pts/shm/console nodes the OCI unpacker strips.
        sh("dev-fd-link", "readlink /dev/fd").out("/proc/self/fd\n"),       // the standard symlink
        sh("dev-fd-open", "printf hi | cat /dev/fd/0").out("hi"),            // /dev/fd/N -> reopen host fd
        sh("dev-stdin", "printf yo | cat /dev/stdin").out("yo"),            // /dev/stdin open -> reopen fd 0
        sh("dev-stdin-link", "readlink /dev/stdin").out("/proc/self/fd/0\n"), // readlink keeps symlink text
        sh("dev-present", "for f in fd stdin stdout stderr ptmx shm console pts; do test -e /dev/$f || { echo MISSING $f; exit 1; }; done; echo all-present").out("all-present\n"),
        // `ls -l /dev` must not error (readlink of std* went via the on-disk symlink, not a pipe F_GETPATH).
        sh("dev-ls-clean", "ls -l /dev >/dev/null 2>/tmp/e; wc -l </tmp/e | tr -d ' '; rm -f /tmp/e").out("0\n"),
        sh("mem-ok", "echo cg ok").mem(64 << 20).out("cg ok\n"),            // runs under cgroup limit
    ])
}

/// Sandbox containment — the rootfs jail must not leak the host filesystem.
/// (NB: `..` paths are avoided here — the dev-only orbstack `mac` bridge mangles them; a real macOS
/// host runs the JIT directly. The jail itself is exercised via absolute host paths below.)
fn sandbox() -> Group {
    group("sandbox", vec![
        // the guest's "/" is the rootfs (has its own bin/etc), not the host root.
        sh("jail-root", "test -d /etc && test -d /bin && echo rootfs-root").out("rootfs-root\n"),
        // host-only absolute paths are not present inside the jail -> ENOENT, never the host dir.
        sh("jail-no-users", "cat /Users 2>&1; echo DONE").has("DONE").has("o such file"),
        sh("jail-no-private", "cat /private/etc/hosts 2>&1; echo DONE").has("DONE").has("o such file"),
        // --- untrusted-guest SENTRY split (DDJIT_UNTRUSTED) ---------------------------------------------
        // Each guest is registered TWICE against the SAME golden line: once on the trusted path (baseline)
        // and once with `.untrusted()` (DDJIT_UNTRUSTED=1) so every fs/net syscall is marshaled to the
        // forked sentry over the SPSC ring and the copied-back bytes must reproduce the baseline exactly.
        // This is the matrix's ONLY DDJIT_UNTRUSTED coverage. DDJIT_SANDBOX stays off (ring, not Seatbelt).
        // fs round-trip: openat/write/lseek/read/pread64/fstat/getdents64/close all cross the ring.
        src("sentry-fs", "sentry_fs.c").out("sentry_fs sum=32640 size=256 found=1\n"),
        src("sentry-fs-untrusted", "sentry_fs.c").out("sentry_fs sum=32640 size=256 found=1\n").untrusted(),
        // socket family: socket/bind/getsockname/sendto/recvfrom on a sentry-owned UDP loopback socket.
        src("sentry-net", "sentry_net.c").out("sentry_net echo=datagram-echo-42 len=16\n"),
        src("sentry-net-untrusted", "sentry_net.c").out("sentry_net echo=datagram-echo-42 len=16\n").untrusted(),
        // clone-FORK lane: a single fork() whose CHILD writes a /tmp file on a freshly CAS-claimed lane
        // (sentry_fork_child drops the inherited lane) while the PARENT reaps via wait4 then reads it back
        // on its own lane. Exercises lane-reclaim + the 8-ring pool under two live workers + owner-gated
        // reap -- the riskiest forwarding path. Same golden under the split as trusted (sum == sentry_fs).
        src("sentry-fork", "sentry_fork.c").out("sentry_fork child_exit=7 readback=ok sum=32640\n"),
        src("sentry-fork-untrusted", "sentry_fork.c").out("sentry_fork child_exit=7 readback=ok sum=32640\n").untrusted(),
    ])
}

/// macOS guest — native Mach-O binaries through the jitdarwin engine (no VM). Built via the mac
/// toolchain; golden-checked (can't run a Mach-O natively on a linux dev host for an oracle).
fn darwin() -> Group {
    group("darwin", vec![
        darwin_src("hello", "hello.c").exit(42).out("hi\n"),   // write(1,"hi\n") + exit(42) via BSD svc
        darwin_src("adrp", "adrp.c").exit(42).out("ADRP-OK\n"), // __cstring literal via adrp (segment slide)
        // BSD/Mach APIs with no Linux equivalent (full libSystem, run under darwinjail).
        darwin_libc("kqueue", "darwin/kqueue.c").out("kqueue readable=1 bytes=5\n"), // EVFILT_READ readiness
        darwin_libc("sysctl", "darwin/sysctl.c").out("sysctl ncpu_ok=1 ostype=Darwin\n"), // sysctlbyname
    ])
}

/// x86-64 guest — prebuilt glibc fixtures through the jit86 engine.
fn x86() -> Group {
    group("x86", vec![
        fixture("hello", &[(Engine::LinuxX86_64, "guests/x86/hello_x86")]).exit(42).out("hi\n"),
        fixture("glibc", &[(Engine::LinuxX86_64, "guests/x86/g_x64")]).has("glibc ok"),
        fixture("glibc-min", &[(Engine::LinuxX86_64, "guests/x86/gw")]).has("glibc-min ok"),
        fixture("ctest", &[(Engine::LinuxX86_64, "guests/x86/ctest_x64")]).exit(7),
        fixture("hx", &[(Engine::LinuxX86_64, "guests/x86/hx")]).has("42"),
    ])
}
