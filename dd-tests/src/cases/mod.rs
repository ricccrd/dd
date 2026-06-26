//! The test registry — declarative groups of cases. Add a case by adding a line here.
//!
//! `src(...)`     compiles a guest from `guests/<file>.c` (aarch64) — `.oracle()` diffs vs native run.
//! `in_rootfs(.)` runs a program already inside an image's rootfs (container behaviour).
//! `fixture(...)` runs a prebuilt binary (the only way to exercise x86-64 today — no cross-compiler).

use crate::{fixture, group, in_rootfs, src, Engine, Group};

/// Every group, in display order.
pub fn all() -> Vec<Group> {
    vec![compat(), system(), container(), x86()]
}

/// Bare ABI / libc compatibility — compiled guests, no rootfs.
fn compat() -> Group {
    group("compat", vec![
        src("hello", "hello.c").exit(42).out("hi\n"),
        src("math", "math.c").exit(0).oracle(),                       // float/int math vs native
        src("files", "files.c").out("files n=7 data=payload\n"),      // open/write/read/unlink
    ])
}

/// Threads + assorted syscalls.
fn system() -> Group {
    group("system", vec![
        src("threads", "threads.c").out("threads sum=800000\n"),      // 8 threads × 100k, mutex/futex
        src("sysinfo", "sysinfo.c").has("sys=Linux pid_ok=1"),        // uname + getpid
    ])
}

/// Container behaviour — programs inside the alpine rootfs (aarch64 image today).
fn container() -> Group {
    group("container", vec![
        in_rootfs("sh-echo", "alpine", &["/bin/sh", "-c", "echo hi"]).out("hi\n"),
        in_rootfs("id-root", "alpine", &["/bin/sh", "-c", "id -u"]).out("0\n"),        // USER-ns: root
        in_rootfs("uname-m", "alpine", &["/bin/sh", "-c", "uname -m"]).out("aarch64\n"),
        in_rootfs("sort", "alpine", &["/bin/sh", "-c", "printf 'c\\na\\nb\\n' | sort | tr '\\n' ' '"]).out("a b c "),
        in_rootfs("mem-ok", "alpine", &["/bin/sh", "-c", "echo cg ok"]).mem(64 << 20).out("cg ok\n"),
    ])
}

/// x86-64 guest — prebuilt fixtures (glibc binaries) run through the jit86 engine.
fn x86() -> Group {
    group("x86", vec![
        fixture("hello", &[(Engine::X86_64, "guests/x86/hello_x86")]).exit(42).out("hi\n"),
        fixture("glibc", &[(Engine::X86_64, "guests/x86/g_x64")]).has("glibc ok"),
        fixture("glibc-min", &[(Engine::X86_64, "guests/x86/gw")]).has("glibc-min ok"),
    ])
}
