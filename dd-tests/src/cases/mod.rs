//! The test registry — declarative groups of cases. Add a case by adding a line.
//!
//! `src(name, file)`         compile `guests/<file>` (aarch64) and run it bare.
//! `.oracle()`               diff the JIT run's stdout+exit against running the same binary natively.
//! `.exit(n)/.out(s)/.has(s)` golden checks.
//! `in_rootfs(name, img, a)` run a program already inside an image's rootfs (container behaviour).
//! `fixture(name, &[(e,p)])` run a prebuilt binary on engine `e` (the only way to exercise x86-64 now).

use crate::{darwin_src, fixture, group, in_rootfs, src, Case, Engine, Group};

/// Every group, in display order.
pub fn all() -> Vec<Group> {
    vec![compat(), libc(), system(), perf(), busybox(), container(), sandbox(), x86(), darwin()]
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
        src("floatmath", "floatmath.c").oracle(),     // sin/cos/sqrt/log/pow/fmod (libm)
    ])
}

/// libc-heavy paths.
fn libc() -> Group {
    group("libc", vec![
        src("heap", "heap.c").oracle(),               // malloc/realloc/free churn
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
        src("threads", "threads.c").out("threads sum=800000\n"),  // clone/futex + §B under threads
        src("atomics", "atomics.c").out("atomic v=1000000\n"),    // LSE atomic idiom
        src("signals", "signals.c").out("signal got=12\n"),       // SIGUSR2 = 12
        src("sysinfo", "sysinfo.c").has("sys=Linux pid_ok=1"),    // uname + getpid
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
    ])
}

/// macOS guest — native Mach-O binaries through the jitdarwin engine (no VM). Built via the mac
/// toolchain; golden-checked (can't run a Mach-O natively on a linux dev host for an oracle).
fn darwin() -> Group {
    group("darwin", vec![
        darwin_src("hello", "hello.c").exit(42).out("hi\n"),   // write(1,"hi\n") + exit(42) via BSD svc
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
