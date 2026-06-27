// frontend/x86_64/legacy.c -- x86-64 has 58 legacy syscalls aarch64 lacks (open/stat/mkdir/pipe/...).
// The shared os/linux/service.c only knows the canonical *at forms. This rewrites each legacy call into
// its *at-equivalent x86 syscall (number + args) BEFORE service() runs canon_x86() + the canonical
// switch, so one shared service serves both guests.
//
// Some x86-only syscalls have no canonical (aarch64) form at all (arch_prctl = the x86 TLS register);
// those are handled HERE and return 1 ("done, don't run the shared switch"). The rest are rewritten in
// place and return 0 to fall through to the shared service.
//
// x86-64 ABI reg map: rax=r[0], rdi=r[7], rsi=r[6], rdx=r[2], r10=r[10], r8=r[8], r9=r[9].
#define ATFD ((uint64_t) - 100) // AT_FDCWD
static int x86_normalize(struct cpu *c) {
    uint64_t *r = c->r;
    switch (r[0]) {
    case 158: // arch_prctl(code, addr): x86 segment-base TLS register; no aarch64 equivalent
        if (r[7] == 0x1002) { c->fs_base = r[6]; r[0] = 0; return 1; }                 // ARCH_SET_FS
        if (r[7] == 0x1001) { c->gs_base = r[6]; r[0] = 0; return 1; }                 // ARCH_SET_GS
        if (r[7] == 0x1003) { *(uint64_t *)r[6] = c->fs_base; r[0] = 0; return 1; }    // ARCH_GET_FS
        if (r[7] == 0x1004) { *(uint64_t *)r[6] = c->gs_base; r[0] = 0; return 1; }    // ARCH_GET_GS
        r[0] = (uint64_t) - 22; return 1;                                             // EINVAL
    // --- path ops: prepend AT_FDCWD, shift the rest ---
    case 2: // open(path,flags,mode) -> openat(AT_FDCWD,path,flags,mode)
        r[10] = r[2]; r[2] = r[6]; r[6] = r[7]; r[7] = ATFD; r[0] = 257; return 0;
    case 4: // stat(path,buf) -> newfstatat(AT_FDCWD,path,buf,0)
        r[10] = 0; r[2] = r[6]; r[6] = r[7]; r[7] = ATFD; r[0] = 262; return 0;
    case 6: // lstat -> newfstatat(...,AT_SYMLINK_NOFOLLOW)
        r[10] = 0x100; r[2] = r[6]; r[6] = r[7]; r[7] = ATFD; r[0] = 262; return 0;
    case 21: // access(path,mode) -> faccessat(AT_FDCWD,path,mode)
        r[2] = r[6]; r[6] = r[7]; r[7] = ATFD; r[0] = 269; return 0;
    case 83: // mkdir(path,mode) -> mkdirat
        r[2] = r[6]; r[6] = r[7]; r[7] = ATFD; r[0] = 258; return 0;
    case 84: // rmdir(path) -> unlinkat(...,AT_REMOVEDIR)
        r[2] = 0x200; r[6] = r[7]; r[7] = ATFD; r[0] = 263; return 0;
    case 87: // unlink(path) -> unlinkat(...,0)
        r[2] = 0; r[6] = r[7]; r[7] = ATFD; r[0] = 263; return 0;
    case 85: // creat(path,mode) -> openat(...,O_CREAT|O_WRONLY|O_TRUNC,mode)
        r[10] = r[6]; r[2] = 0x241; r[6] = r[7]; r[7] = ATFD; r[0] = 257; return 0;
    case 89: // readlink(path,buf,sz) -> readlinkat(AT_FDCWD,path,buf,sz)
        r[10] = r[2]; r[2] = r[6]; r[6] = r[7]; r[7] = ATFD; r[0] = 267; return 0;
    case 90: // chmod(path,mode) -> fchmodat(AT_FDCWD,path,mode)
        r[2] = r[6]; r[6] = r[7]; r[7] = ATFD; r[0] = 268; return 0;
    case 88: // symlink(target,link) -> symlinkat(target,AT_FDCWD,link)
        r[2] = r[6]; r[6] = ATFD; r[0] = 266; return 0;
    case 92: // chown(path,uid,gid) -> fchownat(AT_FDCWD,path,uid,gid,0)
        r[8] = 0; r[10] = r[2]; r[2] = r[6]; r[6] = r[7]; r[7] = ATFD; r[0] = 260; return 0;
    case 94: // lchown -> fchownat(...,AT_SYMLINK_NOFOLLOW)
        r[8] = 0x100; r[10] = r[2]; r[2] = r[6]; r[6] = r[7]; r[7] = ATFD; r[0] = 260; return 0;
    case 133: // mknod(path,mode,dev) -> mknodat(AT_FDCWD,path,mode,dev)
        r[10] = r[2]; r[2] = r[6]; r[6] = r[7]; r[7] = ATFD; r[0] = 259; return 0;
    case 82: // rename(old,new) -> renameat(AT_FDCWD,old,AT_FDCWD,new)
        r[10] = r[6]; r[2] = ATFD; r[6] = r[7]; r[7] = ATFD; r[0] = 264; return 0;
    case 86: // link(old,new) -> linkat(AT_FDCWD,old,AT_FDCWD,new,0)
        r[8] = 0; r[10] = r[6]; r[2] = ATFD; r[6] = r[7]; r[7] = ATFD; r[0] = 265; return 0;
    // poll(fds, nfds, timeout_ms) -> ppoll(fds, nfds, &timespec | NULL, NULL): the canonical handler reads
    // arg2 as a timespec*, but x86 poll's arg2 is an int ms, so synthesize the timespec here.
    case 7: {
        static __thread struct timespec pts;
        long ms = (long)r[2];
        if (ms < 0) r[2] = 0; // infinite -> NULL timespec
        else { pts.tv_sec = ms / 1000; pts.tv_nsec = (ms % 1000) * 1000000L; r[2] = (uint64_t)&pts; }
        r[10] = 0; r[0] = 271; return 0; // -> x86 ppoll (271), which canon_x86 maps to canonical 73
    }
    case 232: r[8] = 0; r[0] = 281; return 0; // epoll_wait(epfd,ev,max,timeout) -> epoll_pwait(...,NULL sigmask)
    // --- flag-arg variants (append a 0 flags) ---
    case 22: r[6] = 0; r[0] = 293; return 0;  // pipe(fds) -> pipe2(fds,0)
    case 33: r[2] = 0; r[0] = 292; return 0;  // dup2(old,new) -> dup3(old,new,0)
    case 284: r[6] = 0; r[0] = 290; return 0; // eventfd(n) -> eventfd2(n,0)
    case 282: r[10] = 0; r[0] = 289; return 0; // signalfd(fd,mask,sz) -> signalfd4(...,0)
    case 213: r[7] = 0; r[0] = 291; return 0; // epoll_create(size) -> epoll_create1(0)
    case 253: r[7] = 0; r[0] = 294; return 0; // inotify_init() -> inotify_init1(0)
    // --- fork/vfork -> clone(SIGCHLD): the shared clone host-forks when not CLONE_THREAD ---
    case 57:
    case 58:
        r[7] = 17; r[6] = 0; r[2] = 0; r[10] = 0; r[8] = 0; r[0] = 56; return 0;
    default: break;
    }
    return 0;
}
