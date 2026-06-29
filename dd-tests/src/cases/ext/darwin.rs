//! darwin — basics expansion (in-process JIT matrix). Owner: darwin agent. Edit ONLY this file.
//! Keep this module compiling at all times (`cargo build -p dd-tests`).
//!
//! macOS-native breadth for the jitdarwin engine: BSD/Mach APIs with NO Linux equivalent (kqueue,
//! sysctl, mach_*, GCD/dispatch, libproc, getattrlist, copyfile) plus Mach-O/AArch64-ABI corners the
//! darwin translator must get right (constructors, TLV, dyld stubs, HFA/variadic ABI, segment slide,
//! dual-register syscall returns). Golden-checked — there is no native-Linux oracle for Mach-O, so all
//! outputs are deterministic markers. `darwin_libc` = full libSystem (main); `darwin_src` = raw
//! syscall-ABI Mach-O via `_start`. The existing `darwin()` group in cases/mod.rs covers hello/adrp/
//! kqueue/sysctl basics; this module ADDS breadth and does not duplicate them.
#![allow(unused_imports)]
use crate::{group, src, port, darwin_src, darwin_libc, fixture, in_rootfs, Case, Engine, Group};

const MAC: &[Engine] = &[Engine::DarwinAarch64];

pub fn groups() -> Vec<Group> {
    vec![
        // ---- kqueue/kevent: the BSD event-notification primitive (epoll/inotify/timerfd/eventfd
        // analogues, but the darwin-only API surface). ----
        group("darwin-kqueue", vec![
            darwin_libc("kq-timer", "darwin/kqueue_timer.c").out("kqueue timer fired=1\n"),
            darwin_libc("kq-write", "darwin/kqueue_write.c").out("kqueue writable=1\n"),
            // EVFILT_SIGNAL never fires under the darwin engine — kevent blocks the full timeout and
            // returns 0 even though SIGUSR1 was raised (the other four filters all work). GAPS:
            // darwin-kqueue-signal.
            darwin_libc("kq-signal", "darwin/kqueue_signal.c").out("kqueue signal=1\n").xfail(MAC),
            darwin_libc("kq-vnode", "darwin/kqueue_vnode.c").out("kqueue vnode write=1\n"),
            darwin_libc("kq-user", "darwin/kqueue_user.c").out("kqueue user=1\n"),
        ]),

        // ---- sysctl: BSD system-info (no Linux equivalent; Linux uses /proc + uname). ----
        group("darwin-sysctl", vec![
            darwin_libc("sysctl-memsize", "darwin/sysctl_memsize.c").out("sysctl mem_ok=1 page_ok=1\n"),
            darwin_libc("sysctl-mib", "darwin/sysctl_mib.c").out("sysctl mib ostype=Darwin ok=1\n"),
            darwin_libc("sysctl-kernproc", "darwin/sysctl_kernproc.c").out("sysctl kern_proc pid_match=1\n"),
        ]),

        // ---- Mach APIs: host/task/thread ports, Mach time, Mach VM, clock service — the Mach RPC
        // surface beneath libSystem. No Linux equivalent. ----
        group("darwin-mach", vec![
            darwin_libc("mach-time", "darwin/mach_time.c").out("mach time tb_ok=1 mono=1\n"),
            darwin_libc("mach-host", "darwin/mach_host.c").out("mach host ok=1\n"),
            darwin_libc("mach-task", "darwin/mach_task.c").out("mach task ok=1\n"),
            darwin_libc("mach-port", "darwin/mach_port.c").out("mach port alloc=1 free=1\n"),
            darwin_libc("mach-self", "darwin/mach_self.c").out("mach thread_self_match=1\n"),
            darwin_libc("mach-vm", "darwin/mach_vm.c").out("mach vm ok=1\n"),
            darwin_libc("mach-hoststats", "darwin/mach_hoststats.c").out("host_stats ok=1\n"),
            darwin_libc("mach-clock", "darwin/mach_clock.c").out("mach clock mono=1\n"),
        ]),

        // ---- pthread (Apple libSystem, Mach-thread backed): create/join, mutex, TSD, once, cond. ----
        group("darwin-pthread", vec![
            darwin_libc("pthread-basic", "darwin/pthread_basic.c").out("pthread ret=42\n"),
            darwin_libc("pthread-mutex", "darwin/pthread_mutex.c").out("pthread mutex counter=40000\n"),
            darwin_libc("pthread-tsd", "darwin/pthread_tsd.c").out("pthread tsd=99\n"),
            darwin_libc("pthread-once", "darwin/pthread_once.c").out("pthread once count=1\n"),
            darwin_libc("pthread-cond", "darwin/pthread_cond.c").out("pthread cond val=42\n"),
        ]),

        // ---- GCD / libdispatch + the clang Blocks runtime: queues, async, group, apply, semaphore. ----
        group("darwin-dispatch", vec![
            darwin_libc("dispatch-sync", "darwin/dispatch_sync.c").out("dispatch sync x=42\n"),
            darwin_libc("dispatch-async", "darwin/dispatch_async.c").out("dispatch async x=42\n"),
            darwin_libc("dispatch-group", "darwin/dispatch_group.c").out("dispatch group sum=55\n"),
            darwin_libc("dispatch-apply", "darwin/dispatch_apply.c").out("dispatch apply sum=4950\n"),
            darwin_libc("dispatch-once", "darwin/dispatch_once.c").out("dispatch once count=1\n"),
            darwin_libc("dispatch-sem", "darwin/dispatch_sem.c").out("dispatch sem 1111\n"),
        ]),

        // ---- Mach-O image + AArch64 (Apple) ABI corners the translator must get exactly right. ----
        group("darwin-macho", vec![
            darwin_libc("macho-ctor", "darwin/macho_ctor.c").out("ctor order=23\n"),
            darwin_libc("macho-data", "darwin/macho_data.c").out("data sum=100 bss=0 str=DATAOK\n"),
            darwin_libc("macho-tls", "darwin/macho_tls.c").out("tls main=7 thread=99\n"),
            darwin_libc("macho-static", "darwin/macho_static.c").out("static 123\n"),
            darwin_libc("macho-stub", "darwin/macho_stub.c").out("stub len=5 snp=6 buf=42-xyz\n"),
            darwin_libc("macho-hfa", "darwin/macho_hfa.c").out("hfa dot=32\n"),
            darwin_libc("macho-varargs", "darwin/macho_varargs.c").out("varargs=15\n"),
            darwin_libc("macho-setjmp", "darwin/macho_setjmp.c").out("setjmp r=7\n"),
        ]),

        // ---- BSD/darwin syscalls with no Linux equivalent (or distinct struct/ABI shape). ----
        group("darwin-bsd", vec![
            darwin_libc("bsd-getpath", "darwin/bsd_getpath.c").out("fcntl getpath ok=1\n"),
            darwin_libc("bsd-getattrlist", "darwin/bsd_getattrlist.c").out("getattrlist ok=1\n"),
            darwin_libc("bsd-statfs", "darwin/bsd_statfs.c").out("statfs ok=1 type_ok=1\n"),
            darwin_libc("bsd-realpath", "darwin/bsd_realpath.c").out("realpath ok=1\n"),
            darwin_libc("bsd-procpath", "darwin/bsd_procpath.c").out("proc_pidpath ok=1\n"),
            darwin_libc("bsd-arc4random", "darwin/bsd_arc4random.c").out("arc4random ok=1\n"),
            darwin_libc("bsd-progname", "darwin/bsd_progname.c").out("progname=ddtest\n"),
            darwin_libc("bsd-copyfile", "darwin/bsd_copyfile.c").out("copyfile ok=1\n"),
            darwin_libc("bsd-gettimeofday", "darwin/bsd_gettimeofday.c").out("gettimeofday mono=1\n"),
            // posix_spawn of a system binary fails under the darwin engine: the guest runs natively
            // under the arm64 darwinjail.dylib (DYLD_INSERT_LIBRARIES), which is inherited by the
            // spawned child — but /usr/bin/true is arm64e, so dyld aborts the child on an arch
            // mismatch and the spawn never completes. GAPS: darwin-spawn-jail-arch.
            darwin_libc("bsd-spawn", "darwin/bsd_spawn.c").out("spawn true=0 false=1\n").xfail(MAC),
        ]),

        // ---- raw syscall-ABI Mach-O guests (no libc, _start entry) — bare BSD svc path + the darwin
        // dual-register (x0/x1) syscall return convention. ----
        group("darwin-raw", vec![
            darwin_src("raw-pid", "raw_pid.c").exit(7),
            darwin_src("raw-pipe", "raw_pipe.c").exit(0),
        ]),

        // ---- executable JIT pages on darwin (W^X). MAP_JIT + pthread_jit_write_protect_np is how
        // real guest JITs (JVM/V8/LuaJIT) get exec memory. This PASSES under jitdarwin — the proper
        // MAP_JIT/W^X-toggle path works (only plain mmap(PROT_EXEC) without MAP_JIT is the gap; see
        // the rwx-mmap GAPS row). Keep as positive coverage so a regression is caught. ----
        group("darwin-wx", vec![
            darwin_libc("mmap-jit", "darwin/mmap_jit.c").out("jit ret=42\n"),
        ]),
    ]
}
