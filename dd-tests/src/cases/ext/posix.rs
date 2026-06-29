//! posix — basics expansion (in-process JIT matrix). Owner: posix agent. Edit ONLY this file.
//! Builders: src(name,file).oracle()/.exit()/.out()/.has(); port(name,file) for cross-engine golden.
//! Keep this module compiling at all times (`cargo build -p dd-tests`).
//!
//! Two groups:
//!  - `posixext`  PORTABLE POSIX (port): one source compiled for Linux x2 + darwin, golden-checked so
//!    the behaviour must be byte-identical emulated-on-Linux and native-on-macOS. Outputs are reduced to
//!    boolean verdicts (errno/uid/inode/pid all differ across platforms, so never printed raw).
//!  - `posixlin`  Linux-form syscalls (statx/sendfile/dup3/pipe2/ppoll/getdents64/uname) that have no
//!    portable shape — Linux-engine only, diffed against a native oracle.
#![allow(unused_imports)]
use crate::{group, src, port, fixture, in_rootfs, Case, Engine, Group};

pub fn groups() -> Vec<Group> { vec![portable(), linuxform()] }

/// Portable POSIX file/dir/mmap/poll/signal/process surface — golden across all three engines.
fn portable() -> Group {
    group("posixext", vec![
        // --- open / read / write / seek ---
        port("openflags", "ext_posix/openflags.c").out("openflags created=1 excl=1 appended=1 truncated=1\n"),
        port("openat", "ext_posix/openat.c").out("openat dirfd=1 readback=1 atcwd=1\n"),
        port("rw", "ext_posix/rw.c").out("rw wrote=256 sum=32640 reads=7\n"),
        port("preadwrite", "ext_posix/preadwrite.c").out("preadwrite match=1 off_kept=1\n"),
        port("iovec", "ext_posix/iovec.c").out("iovec wrote=9 read=9 ok=1\n"),
        port("lseek", "ext_posix/lseek.c").out("lseek set=3 cur=5 end=10 size=1001\n"),
        // --- metadata ---
        port("statfam", "ext_posix/statfam.c").out("statfam reg=1 size=1 ino=1 lnk=1 follow=1\n"),
        port("access", "ext_posix/access.c").out("access f=1 r=1 w=1 missing=1 at=1\n"),
        port("fstatat", "ext_posix/fstatat.c").out("fstatat rel=1 nofollow=1 follow=1\n"),
        // fchmod(2) (fd-based chmod) is a no-op on the Linux JIT — mode stays unchanged (m755=0). chmod()
        // by path works. darwin native applies it. See GAPS `posix-fchmod`.
        port("chmodchown", "ext_posix/chmodchown.c").out("chmodchown chmod=1 m600=1 fchmod=1 m755=1 chown=1\n")
            .xfail(&[Engine::LinuxAarch64, Engine::LinuxX86_64]),
        port("utimensat", "ext_posix/utimensat.c").out("utimensat set=1 atime=1 mtime=1 futimens=1 fmtime=1\n"),
        port("umask", "ext_posix/umask.c").out("umask prev_set=1 masked=1\n"),
        // --- directory / namespace ops ---
        port("mkdirrmdir", "ext_posix/mkdirrmdir.c").out("mkdirrmdir made=1 isdir=1 eexist=1 removed=1 gone=1\n"),
        port("rename", "ext_posix/rename.c").out("rename moved=1 oldgone=1 newhas=1 overwrote=1\n"),
        // hardlink count: link() bumps st_nlink to 2 (seen), but after unlink() of the extra link the
        // Linux JIT still reports nlink!=1 (stale link count). darwin native is correct. GAPS `posix-nlink`.
        port("linksym", "ext_posix/linksym.c").out("linksym hardlink=1 nlink2=1 symlink=1 readlink=1 nlink1=1\n")
            .xfail(&[Engine::LinuxAarch64, Engine::LinuxX86_64]),
        port("truncate", "ext_posix/truncate.c").out("truncate shrunk=1 grew=1 zeros=1 pathtrunc=1\n"),
        // rewinddir() (lseek(dirfd,0) on the getdents stream) does not reset enumeration on the Linux
        // JIT — the second pass sees 0 entries (rewind=0). darwin native rewinds. GAPS `posix-rewinddir`.
        port("readdir-dtype", "ext_posix/readdir_dtype.c").out("readdir_dtype files=2 dirs=1 rewind=3\n")
            .xfail(&[Engine::LinuxAarch64, Engine::LinuxX86_64]),
        port("getcwdchdir", "ext_posix/getcwdchdir.c").out("getcwdchdir before=1 chdir=1 ends=1\n"),
        // --- fd plumbing ---
        port("dup", "ext_posix/dup.c").out("dup dupped=1 sharedoff=1 dup2=1 read=1\n"),
        port("fcntlmisc", "ext_posix/fcntlmisc.c").out("fcntlmisc rw=1 nonblock=1 dupfd=1 cloexec=1\n"),
        port("flock", "ext_posix/ftruncate_lock.c").out("flock locked=1 child_blocked=1\n"),
        port("fsync", "ext_posix/fsync.c").out("fsync fsync=1 fdatasync=1 survived=1\n"),
        // --- readiness ---
        port("pollpipe", "ext_posix/pollpipe.c").out("pollpipe timeout=1 writable=1 readable=1\n"),
        port("selectpipe", "ext_posix/selectpipe.c").out("selectpipe timeout=1 ready=1 pselect=1\n"),
        // --- memory ---
        port("mmapfile", "ext_posix/mmapfile.c").out("mmapfile mapped=1 persisted=1\n"),
        port("mmapanon2", "ext_posix/mmapanon2.c").out("mmapanon2 mapped=1 zeroed=1 wrote=1 mprotect=1 munmap=1\n"),
        port("madvise", "ext_posix/madvise.c").out("madvise normal=1 willneed=1 dontneed=1 readable=1\n"),
        // --- time ---
        port("clockid", "ext_posix/clockid.c").out("clockid real=1 mono=1 cputime=1 realpos=1\n"),
        port("nanosleep", "ext_posix/nanosleep.c").out("nanosleep rc=0 slept_ge=1\n"),
        // --- signals ---
        port("sigmask", "ext_posix/sigmask.c").out("sigmask not_yet=1 pending=1 delivered=1\n"),
        port("killraise", "ext_posix/killraise.c").out("killraise raise=1 kill=1 count=1\n"),
        // --- process / limits / identity ---
        // getpgrp() returns 0 (not the process group) on the x86_64 JIT; getpgid(0)/getsid work. The
        // aarch64 and darwin engines are correct. GAPS `posix-getpgrp-x86`.
        port("getids", "ext_posix/getids.c").out("getids pid=1 ppid=1 uid=1 gid=1 pgrp=1 sid=1\n"),
        port("setpgid", "ext_posix/setpgid.c").out("setpgid child_own=1 parent_sees=1\n"),
        port("waitstatus", "ext_posix/waitstatus.c").out("waitstatus exit7=1 sigkill=1 sigabrt=1\n"),
        port("pipe2way", "ext_posix/pipe2way.c").out("pipe2way reply=10 reaped=1\n"),
        port("getrlimit", "ext_posix/getrlimit.c").out("getrlimit nofile=1 stack=1 rusage=1\n"),
        // NB: _SC_OPEN_MAX is -1 on the Linux JIT (RLIMIT_NOFILE is reported as unlimited, so glibc
        // returns "unlimited" → not asserted here). See GAPS `posix-nofile-infinity`.
        port("sysconf", "ext_posix/sysconf.c").out("sysconf ps=1 clk=1 ok=1 pow2=1 nproc_ge1=1\n"),
        // --- exec (loader replace-image path) ---
        port("execself", "ext_posix/execself.c").out("execself child ok=1\n"),
    ])
}

/// Linux-form syscalls with no portable shape — Linux engines only, oracle-diffed against native.
fn linuxform() -> Group {
    group("posixlin", vec![
        src("statx", "ext_posix/statx.c").oracle(),
        src("sendfile2", "ext_posix/sendfile.c").oracle(),
        src("dup3", "ext_posix/dup3.c").oracle(),
        // pipe2(O_NONBLOCK): F_GETFL shows O_NONBLOCK but a read on the empty pipe does NOT return
        // EAGAIN on the JIT (the nonblocking flag isn't honored on the pipe read path). GAPS `posix-pipe2-nonblock`.
        src("pipe2", "ext_posix/pipe2.c").oracle(),
        src("ppoll", "ext_posix/ppoll.c").oracle(),
        src("getdents64", "ext_posix/getdents64.c").oracle(),
        src("uname", "ext_posix/uname.c").oracle(),
    ])
}
