# Syscalls

The aarch64 guest's Linux syscall layer lives in `dd-jit/src/runtime/os/linux/service.c` — one switch
on the syscall number, grouped by category. Each handler translates the Linux ABI to the macOS host:
errno (`m2l_errno`), struct layouts (`stat`/`statfs`/`termios`/`sigaction`), flag bits, fd semantics.
**Every path argument is resolved through the container VFS jail** (`os/linux/container/vfs.c`).

The x86-64 guest (jit86) keeps its own x86-numbered switch for now; the dedup will key both off a
canonical id so these bodies are shared. Table auto-derived from the source.

**166 syscall numbers handled.**

## I/O — read/write/seek (+ eventfd/timerfd/signalfd fd redirection) (10)

| nr | name | notes |
|---:|------|-------|
| 62 | `lseek` |  |
| 63 | `read` |  |
| 64 | `write` | eventfd -> pipe |
| 65 | `readv` | readv |
| 66 | `writev` | writev |
| 67 | `pread64` | pread64 |
| 68 | `pwrite64` | pwrite64 |
| 71 | `sendfile` | sendfile(out,in,off*,count) |
| 76 | `splice` | splice(fd_in,off_in,fd_out,off_out,len,fl) / tee -> emulate |
| 77 | `tee` | splice(fd_in,off_in,fd_out,off_out,len,fl) / tee -> emulate |

## Filesystem — open/stat/dir/link/perm/xattr/cwd, all path-confined to the rootfs jail (56)

| nr | name | notes |
|---:|------|-------|
| 5 | `setxattr` | setxattr/lsetxattr/fsetxattr -> ignore |
| 6 | `lsetxattr` | setxattr/lsetxattr/fsetxattr -> ignore |
| 7 | `fsetxattr` | setxattr/lsetxattr/fsetxattr -> ignore |
| 8 | `getxattr` | getxattr/... -> ENODATA (no such attr) |
| 9 | `lgetxattr` | getxattr/... -> ENODATA (no such attr) |
| 10 | `fgetxattr` | getxattr/... -> ENODATA (no such attr) |
| 11 | `listxattr` | listxattr/... -> empty list |
| 12 | `llistxattr` | listxattr/... -> empty list |
| 13 | `flistxattr` | listxattr/... -> empty list |
| 14 | `removexattr` | removexattr/... -> ok |
| 15 | `lremovexattr` | removexattr/... -> ok |
| 16 | `fremovexattr` | removexattr/... -> ok |
| 17 | `getcwd` | getcwd -> the GUEST cwd (not the host path) |
| 23 | `dup` | dup |
| 24 | `dup3` | dup3(old,new,flags) |
| 25 | `fcntl` | fcntl -- Linux cmd# -> macOS (they diverge!) |
| 29 | `ioctl` | ioctl(fd, req, arg) -- Linux req# -> macOS |
| 33 | `mknodat` | mknodat(dirfd, path, mode, dev) |
| 34 | `mkdirat` | mkdirat(dirfd, path, mode) -- confined |
| 35 | `unlinkat` | unlinkat(dirfd, path, flags) -- confined |
| 36 | `symlinkat` | symlinkat(target, newdirfd, linkpath) |
| 37 | `linkat` | linkat(odir,opath,ndir,npath,flags) |
| 38 | `renameat` | renameat / renameat2 (flags ignored) |
| 39 | `umount2` | mount / umount2 / pivot_root -> ok |
| 40 | `mount` | mount / umount2 / pivot_root -> ok |
| 41 | `pivot_root` | mount / umount2 / pivot_root -> ok |
| 43 | `statfs` | statfs/fstatfs |
| 44 | `fstatfs` | statfs/fstatfs |
| 46 | `ftruncate` | ftruncate |
| 47 | `fallocate` | fallocate(fd,mode,offset,len): extend (no shrink) |
| 48 | `faccessat` | faccessat |
| 49 | `chdir` | chdir (confined; tracks guest cwd) |
| 50 | `fchdir` | fchdir (tracks guest cwd) |
| 52 | `fchmod` | fchmod(fd, mode) |
| 53 | `fchmodat` | fchmodat(dirfd,path,mode,flags) / fchmodat2 |
| 54 | `fchownat` | fchownat(dirfd,path,uid,gid,flags) -- best-effort (rootless) |
| 55 | `fchown` | fchown(fd,uid,gid) -- best-effort |
| 56 | `openat` | openat -- Linux O_* -> macOS O_* (they differ!) |
| 57 | `close` | reap eventfd peer / timerfd / overlay dir / loopback |
| 59 | `pipe2` | pipe2(fds, flags) |
| 61 | `getdents64` | getdents64 |
| 78 | `readlinkat` | readlinkat |
| 79 | `newfstatat` | newfstatat(dfd, path, buf, flags) |
| 80 | `fstat` | fstat(fd, buf) |
| 81 | `sync` | sync |
| 82 | `fsync` | fsync |
| 83 | `fdatasync` | fdatasync -> fsync (no macOS fdatasync) |
| 88 | `utimensat` | utimensat(dirfd, path, times, flags) |
| 166 | `umask` | umask -> old mask |
| 223 | `fadvise64` | fadvise64 -- advisory no-op |
| 276 | `renameat2` | renameat / renameat2 (flags ignored) |
| 285 | `copy_file_range` | copy_file_range(fdin,offin*,fdout,offout*,len,flags) |
| 291 | `statx` | statx(dfd, path, flags, mask, buf) |
| 437 | `openat2` | openat2(dirfd, path, open_how*, size) -- glibc uses it; MUST confine |
| 439 | `faccessat2` | faccessat2(dirfd,path,mode,flags) -- glibc access() uses it; same path/confinement, flags ig |
| 452 | `fchmodat2` | fchmodat(dirfd,path,mode,flags) / fchmodat2 |

## Memory — mmap/brk/mprotect/madvise (anon charged against cgroup memory.max) (9)

| nr | name | notes |
|---:|------|-------|
| 214 | `brk` | brk |
| 215 | `munmap` | munmap |
| 216 | `mremap` | mremap (copy+grow) |
| 222 | `mmap` | mmap |
| 226 | `mprotect` | mprotect |
| 228 | `mlock` | mlock/munlock (no-op) |
| 229 | `munlock` | mlock/munlock (no-op) |
| 232 | `mincore` | mincore -> unsupported (callers fall back) |
| 233 | `madvise` | madvise |

## Process & scheduling — clone/exec/wait/ids/prctl/futex/caps/sched (41)

| nr | name | notes |
|---:|------|-------|
| 90 | `capget` | capget -> all caps present |
| 91 | `capset` | capset -> ok |
| 93 | `exit` | exit: end THIS thread |
| 94 | `exit_group` | exit_group: end the whole process |
| 96 | `set_tid_address` | set_tid_address -> returns caller's TID (musl stores it; 0 -> a_crash()) |
| 97 | `unshare` | unshare / setns -> ok (no real ns) |
| 98 | `futex` | futex |
| 99 | `set_robust_list` | set_robust_list |
| 116 | `syslog` | syslog |
| 122 | `sched_setaffinity` | sched_setaffinity |
| 123 | `sched_getaffinity` | sched_getaffinity(pid,size,MASK=a2!) |
| 124 | `sched_yield` | sched_yield |
| 140 | `setpriority` | setpriority (best-effort) |
| 141 | `getpriority` | getpriority -> Linux raw (20-nice) |
| 144 | `setgid` | setgid/setfsuid/setresuid/setresgid -> ok |
| 145 | `setuid` | getpgid |
| 146 | `?` | setgid/setfsuid/setresuid/setresgid -> ok |
| 147 | `?` | setgid/setfsuid/setresuid/setresgid -> ok |
| 148 | `getresuid` | getresuid(r,e,s) |
| 149 | `?` | setgid/setfsuid/setresuid/setresgid -> ok |
| 150 | `getresgid` | getresgid(r,e,s) |
| 154 | `setpgid` | setpgid |
| 155 | `getpgid` | getpgid (bash job control) |
| 156 | `getsid` | getsid |
| 158 | `getgroups` | getgroups -> [container gid] |
| 159 | `setgroups` | setgroups (privileged; ignore) |
| 165 | `getrusage` | getrusage(who, *usage) -- a1 is the buffer, not a0! |
| 167 | `prctl` | prctl(option,...) |
| 172 | `getpid` | getpid (PID ns: init -> 1) |
| 173 | `getppid` | getppid (init's parent is 0 in the ns) |
| 174 | `getuid` | getuid/geteuid -> container uid (0=root by default) |
| 175 | `?` | getuid/geteuid -> container uid (0=root by default) |
| 176 | `getgid` | getgid/getegid |
| 177 | `?` | getgid/getegid |
| 178 | `gettid` | gettid |
| 220 | `clone` | clone(flags,stack,ptid,tls,ctid) |
| 221 | `execve` | execve(path, argv, envp) |
| 260 | `wait4` | wait4(pid, *status, opts, *rusage) |
| 261 | `prlimit64` | prlimit64(pid,res,new,OLD): old=a3! |
| 268 | `?` | unshare / setns -> ok (no real ns) |
| 435 | `clone3` | clone3(clone_args*, size) |

## Signals — Linux signal numbers -> macOS; kill/sigaction/sigreturn (8)

| nr | name | notes |
|---:|------|-------|
| 129 | `kill` | kill(pid,sig) |
| 130 | `tkill` | tkill(tid,sig) |
| 131 | `tgkill` | tgkill(tgid,tid,sig) |
| 132 | `sigaltstack` | sigaltstack(new, old) |
| 134 | `rt_sigaction` | rt_sigaction(sig, *act, *old) |
| 135 | `rt_sigprocmask` | rt_sigprocmask(how, *set, *old) |
| 136 | `rt_sigpending` | rt_sigpending(set, sigsetsize) |
| 139 | `rt_sigreturn` | rt_sigreturn (restorer path) |

## Time — clock_gettime/nanosleep/gettimeofday (Linux clock-id translation) (6)

| nr | name | notes |
|---:|------|-------|
| 101 | `nanosleep` | nanosleep |
| 113 | `clock_gettime` | clock_gettime -- Linux clockid -> macOS |
| 114 | `clock_getres` | clock_getres -> 1ns |
| 115 | `clock_nanosleep` | clock_nanosleep |
| 153 | `times` | times |
| 169 | `gettimeofday` | gettimeofday |

## Network — sockets; port-map (-p) + NET-ns private loopback (18)

| nr | name | notes |
|---:|------|-------|
| 198 | `socket` | socket |
| 199 | `socketpair` | socketpair |
| 200 | `bind` | bind -- port-map: bind the published host port |
| 201 | `listen` |  |
| 202 | `accept` | accept / accept4 |
| 203 | `connect` | connect |
| 204 | `getsockname` | getsockname |
| 205 | `getpeername` | getpeername |
| 206 | `sendto` |  |
| 207 | `recvfrom` |  |
| 208 | `setsockopt` | setsockopt(fd, level, optname, val, len) |
| 209 | `getsockopt` | getsockopt(fd, level, optname, val, len) |
| 210 | `shutdown` | shutdown(fd, how) -- SHUT_RD/WR/RDWR match |
| 211 | `sendmsg` | sendmsg/recvmsg -- translate Linux msghdr -> macOS |
| 212 | `recvmsg` | sendmsg/recvmsg -- translate Linux msghdr -> macOS |
| 242 | `accept4` | accept / accept4 |
| 243 | `recvmmsg` | sendmmsg/recvmmsg(fd, mmsghdr[], vlen, flags, [timeout]) |
| 269 | `sendmmsg` | sendmmsg/recvmmsg(fd, mmsghdr[], vlen, flags, [timeout]) |

## Event loop — epoll/eventfd/timerfd/signalfd/inotify (macOS kqueue) (12)

| nr | name | notes |
|---:|------|-------|
| 19 | `eventfd2` | eventfd2(initval, flags) -> pipe |
| 20 | `epoll_create1` | epoll_create1(flags) -> kqueue |
| 21 | `epoll_ctl` | epoll_ctl(epfd, op, fd, event) -> kevent |
| 22 | `epoll_pwait` | epoll_pwait(epfd, events, max, timeout_ms, sigmask) |
| 26 | `inotify_init1` | inotify_init1(flags) -> kqueue |
| 27 | `inotify_add_watch` | inotify_add_watch(fd, path, mask) -- kqueue EVFILT_VNODE |
| 28 | `inotify_rm_watch` | inotify_rm_watch(fd, wd) |
| 73 | `ppoll` | ppoll -> poll |
| 74 | `signalfd4` | signalfd4(fd, mask, sizemask, flags) |
| 85 | `timerfd_create` | timerfd_create(clockid, flags) -> kqueue |
| 86 | `timerfd_settime` | timerfd_settime(fd, flags, new, old) |
| 87 | `timerfd_gettime` | timerfd_gettime -> best-effort 0 |

## Misc — uname/sysinfo/getrandom/hostname (6)

| nr | name | notes |
|---:|------|-------|
| 160 | `uname` | uname |
| 161 | `sethostname` | sethostname (UTS ns) |
| 162 | `setdomainname` | setdomainname -> ignore |
| 179 | `sysinfo` | sysinfo |
| 278 | `getrandom` | getrandom |
| 293 | `rseq` | rseq -> ENOSYS (glibc falls back) |

