// brk arena
static uint64_t brk_lo, brk_cur, brk_hi;
// W3D fork-server prewarm/worker: when set, the guest's exit_group UNWINDS run_guest (sets c->exited
// + c->exit_code) instead of _exit()ing, so the resident ddjitd parent survives pre-translating a
// binary into the COW arena and a worker can report its exit code before dying. 0 on every normal
// (standalone) run -> exit_group behaves exactly as before.
int g_noexit;
// W6A item 3: set the first time a guest requests a PROT_EXEC (RWX) anonymous mapping -- i.e. a
// guest with its own in-process JIT (JVM/V8/LuaJIT/.NET/PyPy). Normal guests never set it, so the
// SMC write-fault invalidation path (frontend/x86_64) stays completely inert for the whole existing
// test matrix (g_rwx_guest==0 -> smc_protect()/smc_on_write() are no-ops -> bit-exact).
int g_rwx_guest;
// dd/runtime/os/linux -- service(): the Linux syscall layer (the "kernel" the guest talks to).
// Dispatches the guest syscall number to the host, translating the ABI (errno, struct layouts, flags,
// fd semantics); every path argument is resolved through the container VFS jail. One sorted switch,
// grouped by category. See docs/SYSCALLS.md for the per-syscall table.

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <dirent.h>
#include <stdlib.h>
#include <sys/times.h> // times(2): CPU accounting (struct tms is layout-compatible with Linux)
#include <sys/mount.h> // host struct statfs -> translated to the Linux statfs layout
// macOS renamex_np/renameatx_np flags (Linux renameat2 flags map onto these)
#ifndef RENAME_SWAP
#define RENAME_SWAP 0x00000002 // atomic swap  <- Linux RENAME_EXCHANGE(2)
#endif
#ifndef RENAME_EXCL
#define RENAME_EXCL 0x00000004 // fail if dst exists <- Linux RENAME_NOREPLACE(1)
#endif
#include "service/helpers.c"
#include "service/sysv.c"
#include "service/mem.c"
#include "service/signal.c"
#include "service/time.c"
#include "service/io.c"
#include "service/net.c"
#include "service/event.c"
#include "service/misc.c"
// --- untrusted-guest isolation seam (subsystem #3: the sentry process-split) --------------------
// The dispatcher's syscall boundary (run_guest -> service(c)) is the entire guest->host authority
// crossing. We interpose a one-branch router so an UNTRUSTED guest's fs/net/proc syscalls can be
// executed in a separate, deny-default-sandboxed SENTRY process instead of in this (JIT-hosting)
// worker. The gate `g_untrusted` and the router `syscall_route()` live in os/linux/sentry.c, which
// the target TU #includes immediately AFTER this file (next to it). When the gate is OFF -- the
// default, and the ENTIRE test matrix -- service() tail-calls service_local(c) (the canonical switch
// below, renamed) through a single statically-predicted-not-taken branch: byte-identical to the
// pre-split engine, no behavior change, no measurable cost. The fork/ring/Seatbelt machinery only
// exists under the gate. This is the ONLY edit to this file.
static int g_untrusted;                   // gate (defined + env-parsed in os/linux/sentry.c)
static void syscall_route(struct cpu *c); // sentry router (defined in os/linux/sentry.c)
static void service_local(struct cpu *c); // fwd: the canonical syscall switch (this file)
// g2h-style redirect for non-PIE ET_EXEC pointer args. A non-PIE links at a fixed low vaddr but is biased
// HIGH by load_elf (__PAGEZERO forbids the low 4 GB); an un-relocated pointer baked at the low link vaddr
// (e.g. a global .rodata string handed to open()/write()) still names the low range, where nothing is
// mapped. The real bytes live at addr+g_nonpie_bias in the high-mapped image, so any *pointer* syscall arg
// that lands in [g_nonpie_lo,g_nonpie_hi) must be rebased before the host syscall dereferences it. Inert
// for PIE/static-PIE (g_nonpie_lo==0, the only state the test matrix ever sees) and for any pointer that is
// already high (stack/heap/bss-above-bias) -> byte-identical there. Apply ONLY to pointer positions.
static inline uint64_t nonpie_p(uint64_t a) {
    return (g_nonpie_lo && a >= g_nonpie_lo && a < g_nonpie_hi) ? a + g_nonpie_bias : a;
}
// Overlay: a metadata/rename syscall (chmod/chown/utimensat/rename) confines to the writable upper via
// jail_at, but a target that still lives only in a read-only lower (the image) is absent from the upper
// -> the op ENOENTs. Copy the target up first (same write-path pattern openat uses) so jail_at finds it.
// No-op when not in overlay mode (g_nlower==0) or when the file is already in the upper. (dirfd,raw) is
// the syscall's AT_FDCWD/dir-fd-relative path; overlay_copyup leaves a genuinely missing path untouched
// so a real bad path still ENOENTs in the upper as before.
static void overlay_copyup_at(int dirfd, const char *raw) {
    if (!g_nlower || !raw) return;
    char gp[4200], host[4300];
    abs_guest(dirfd, raw, gp, sizeof gp);
    overlay_copyup(gp, host, sizeof host);
}
// Overlay: does a read-only lower still provide `guest` (so it would re-surface once the upper copy is moved
// away)? Mirrors overlay_copyup's lower scan; rootfs-routed paths only (a volume has its own backing dir).
// Used by rename to decide whether the source needs a whiteout. False outside overlay mode (g_nlower==0).
static int overlay_lower_has(const char *guest) {
    if (!g_nlower || !guest || guest[0] != '/') return 0;
    const char *canon;
    size_t clen;
    const char *rel;
    if (jail_pick(guest, &canon, &clen, &rel) != g_root_fd) return 0;
    for (int i = 0; i < g_nlower; i++) {
        char lp[4300];
        struct stat st;
        layer_follow(g_lower[i].canon, g_lower[i].clen, guest, lp, sizeof lp, 1);
        if (lstat(lp, &st) == 0) return 1;
        if (wh_exists(g_lower[i].canon, g_lower[i].clen, guest)) return 0; // hidden below this layer
    }
    return 0;
}
// adjtimex/clock_adjtime read-only query: macOS has no adjtimex, so report an OK-but-unsynchronised
// kernel clock and fill the Linux struct timex the caller passed. Setting the clock (modes != 0) needs
// CAP_SYS_TIME, which the container lacks -> EPERM (mirrors clock_settime). Returns the clock state
// (TIME_OK) or a negative errno. Offsets match the LP64 Linux struct timex.
static int svc_adjtimex(uint8_t *tx) {
    if (!tx) return -EFAULT;
    uint32_t modes = *(uint32_t *)(tx + 0);
    if (modes != 0) return -EPERM; // any clock-adjusting call -> EPERM (no CAP_SYS_TIME)
    struct timeval now;
    gettimeofday(&now, NULL);
    *(int64_t *)(tx + 8) = 0;          // offset (us)
    *(int64_t *)(tx + 16) = 0;         // freq (scaled ppm)
    *(int64_t *)(tx + 24) = 16384;     // maxerror (us)
    *(int64_t *)(tx + 32) = 16384;     // esterror (us)
    *(int32_t *)(tx + 40) = 0x0040;    // status = STA_UNSYNC
    *(int64_t *)(tx + 48) = 2;         // constant
    *(int64_t *)(tx + 56) = 1;         // precision (us)
    *(int64_t *)(tx + 64) = 32768000;  // tolerance (default)
    *(int64_t *)(tx + 72) = now.tv_sec;
    *(int64_t *)(tx + 80) = now.tv_usec;
    *(int64_t *)(tx + 88) = 10000;     // tick (us)
    return 0;                          // TIME_OK
}
// pidfd support: macOS has no pidfd, so pidfd_open() hands back a real (/dev/null) fd and we remember
// which guest pid it stands for, so pidfd_send_signal() can resolve the fd back to its target pid.
#define PIDFD_MAX 64
static struct {
    int fd;
    pid_t pid;
} g_pidfd[PIDFD_MAX];
static void pidfd_register(int fd, pid_t pid) {
    for (int i = 0; i < PIDFD_MAX; i++)
        if (g_pidfd[i].fd == 0 || g_pidfd[i].fd == fd) {
            g_pidfd[i].fd = fd;
            g_pidfd[i].pid = pid;
            return;
        }
}
static int pidfd_lookup(int fd, pid_t *pid) {
    for (int i = 0; i < PIDFD_MAX; i++)
        if (g_pidfd[i].fd == fd) {
            *pid = g_pidfd[i].pid;
            return 0;
        }
    return -1;
}
// POSIX message queues (mq_*): macOS has no POSIX mqueue, so emulate an in-process named priority queue.
// Each queue keeps messages highest-priority-first (FIFO within a priority); descriptors are real
// (/dev/null-backed) fds so close()/poll() stay valid, with an fd->queue table to map them back. This
// covers single-process producers/consumers; it is not shared across fork and does not block (full/empty
// return EAGAIN), which is sufficient for the queue depths POSIX mqueue programs exercise here.
#define MQ_MAXQ 16
#define MQ_MAXMSG 64
struct mq_qmsg {
    unsigned prio;
    size_t len;
    char *data;
};
struct mq_queue {
    int used, unlinked, refs, n;
    char name[80];
    long maxmsg, msgsize;
    struct mq_qmsg msg[MQ_MAXMSG];
};
static struct mq_queue g_mqq[MQ_MAXQ];
static struct {
    int fd, qi;
} g_mqfd[64];
static int mq_find(const char *name) {
    for (int i = 0; i < MQ_MAXQ; i++)
        if (g_mqq[i].used && !g_mqq[i].unlinked && !strcmp(g_mqq[i].name, name)) return i;
    return -1;
}
static int mq_qof(int fd) {
    for (int i = 0; i < 64; i++)
        if (g_mqfd[i].fd == fd) return g_mqfd[i].qi;
    return -1;
}
static void mq_bind(int fd, int qi) {
    for (int i = 0; i < 64; i++)
        if (g_mqfd[i].fd == 0 || g_mqfd[i].fd == fd) {
            g_mqfd[i].fd = fd;
            g_mqfd[i].qi = qi;
            return;
        }
}
static void mq_maybe_free(int qi) {
    struct mq_queue *q = &g_mqq[qi];
    if (q->refs <= 0 && q->unlinked) {
        for (int j = 0; j < q->n; j++)
            free(q->msg[j].data);
        memset(q, 0, sizeof *q);
    }
}
// CPU topology: the number of CPUs to advertise to the guest (the host's online count, capped). glibc
// and tcmalloc enumerate CPUs via sched_getaffinity and /sys/devices/system/cpu/{online,possible};
// reporting only CPU 0 makes tcmalloc's NumPossibleCPUs() assert (`cpus.has_value()`) and mongod abort.
static int dd_online_cpus(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 64) n = 64; // matches /proc/cpuinfo's cap; one CPU bit fits in 8 bytes of mask
    return (int)n;
}
// Build the "all online CPUs" bitmask into the caller's buffer (CPU i -> bit i, little-endian bytes).
static void cpu_online_mask(uint8_t *m, size_t n) {
    memset(m, 0, n);
    int nc = dd_online_cpus();
    for (int cpu = 0; cpu < nc; cpu++)
        if ((size_t)(cpu / 8) < n) m[cpu / 8] |= (uint8_t)(1u << (cpu % 8));
}
// Current CPU-affinity mask (process-global; default = all online CPUs). sched_setaffinity records the
// guest's chosen mask so sched_getaffinity round-trips it (pin-to-CPU0 then read back), while a fresh
// process still advertises every online CPU so glibc/tcmalloc size their per-CPU tables correctly.
static uint8_t g_affinity[128];
static int g_affinity_set;
static const uint8_t *affinity_mask(void) {
    if (!g_affinity_set) {
        cpu_online_mask(g_affinity, sizeof g_affinity);
        g_affinity_set = 1;
    }
    return g_affinity;
}
// Back a short synthesized sysfs string with an anonymous temp fd (the same trick proc_open uses for
// the macOS-has-no-/proc case). Returns a readable fd positioned at offset 0, or -1 on error.
static int synth_str_fd(const char *s) {
    char tn[] = "/tmp/.ddcpuXXXXXX";
    int fd = mkstemp(tn);
    if (fd < 0) return -1;
    unlink(tn);
    size_t len = strlen(s);
    if (write(fd, s, len) < 0) {}
    lseek(fd, 0, SEEK_SET);
    return fd;
}
// Render the kernel's CPU-range format ("0" for a single CPU, else "0-N\n") for the cpu/{online,
// possible,present} sysfs files that glibc __get_nprocs / tcmalloc NumPossibleCPUs parse.
static void cpu_range_str(char *buf, size_t n) {
    int nc = dd_online_cpus();
    if (nc <= 1)
        snprintf(buf, n, "0\n");
    else
        snprintf(buf, n, "0-%d\n", nc - 1);
}
// /proc/self/exe and /proc/<pid>/exe (where <pid> is the guest's own pid) are magic kernel symlinks
// to the running executable. macOS has no /proc, so synthesize them: the link target is the guest
// path that was exec'd (g_exe_path). Many programs (Go, the JVM, boost::filesystem, mongod) readlink
// or stat this to locate their own binary. Returns 1 and fills tgt[] with the guest-visible target
// path when `p` names this link, else 0.
// Backing storage for g_exe_path after an execve (case 221) updates it: the initial g_exe_path points at
// the launcher's argv buffer, but a post-exec /proc/self/exe must name the NEWLY exec'd image. We copy the
// guest-absolute exec path here and repoint g_exe_path at it (the prior image's address space is torn down).
static char g_exe_path_store[4200];
static int proc_self_exe(const char *p, char *tgt, size_t cap) {
    if (!p || strncmp(p, "/proc/", 6)) return 0;
    const char *rest = p + 6;
    if (!strncmp(rest, "self/", 5)) {
        rest += 5;
    } else {
        char *end;
        long pid = strtol(rest, &end, 10);
        if (end == rest || *end != '/' || (int)pid != container_pid()) return 0;
        rest = end + 1;
    }
    if (strcmp(rest, "exe")) return 0;
    const char *src = (g_exe_path && g_exe_path[0]) ? g_exe_path : "/";
    // g_exe_path is already a guest-absolute path; strip any rootfs prefix that may have leaked in.
    if (g_rootfs && !strncmp(src, g_rootfs_canon, g_rootfs_canon_len)) src += g_rootfs_canon_len;
    if (!src[0]) src = "/";
    size_t l = strlen(src);
    if (l >= cap) l = cap - 1;
    memcpy(tgt, src, l);
    tgt[l] = 0;
    return 1;
}
// svc_fs and svc_rare live here (not with the other family includes at the top): their cases call this
// file's local helpers (overlay_*/proc_self_exe/synth_str_fd for fs; svc_adjtimex/pidfd_*/mq_* for rare)
// defined just above, so they must be included AFTER them.
#include "service/fs.c"
#include "service/rare.c"
static void service(struct cpu *c) {
    if (__builtin_expect(g_untrusted, 0)) {
        syscall_route(c);
        return;
    } // untrusted: route via sentry
    service_local(c); // trusted: byte-identical path
}
static void service_local(struct cpu *c) {
    // Frontends whose guest has legacy syscalls without a canonical (aarch64) equivalent rewrite them
    // into their *at form here (x86: open->openat, ...); a no-op where the guest is already canonical.
    if (G_NORMALIZE(c)) return;
    uint64_t nr = G_NR(c), a0 = G_A0(c), a1 = G_A1(c), a2 = G_A2(c), a3 = G_A3(c), a4 = G_A4(c), a5 = G_A5(c);
    if (g_trace)
        fprintf(stderr, "[sys] %llu (%llx,%llx,%llx)\n", (unsigned long long)nr, (unsigned long long)a0,
                (unsigned long long)a1, (unsigned long long)a2);
    // --- non-PIE ET_EXEC pointer-arg redirect (g2h) --------------------------------------------------
    // Rebase ONLY the pointer-typed args of each syscall a non-PIE realistically hands a low-image
    // (.rodata/.data/.bss) pointer to, so the host syscall reads/writes the SAME bytes a native run would.
    // Per-syscall + per-position: size/flag/fd/count args are NEVER touched (a blanket a0..a5 rebase would
    // corrupt a count/fd that happened to fall in the link range). Whole block is inert unless g_nonpie_lo
    // is set (ET_EXEC only) -> PIE/static-PIE and the entire test matrix are byte-identical. Numbers are
    // the canonical (aarch64) syscall numbers G_NR() maps the x86 guest's calls onto. Runs BEFORE the
    // res_bump switch below, which itself dereferences a2 (open_how*) for openat2.
    if (g_nonpie_lo) {
        switch (nr) {
        case 56:  // openat(dfd, PATH, flags, mode)
        case 33:  // mknodat(dfd, PATH, ...)
        case 34:  // mkdirat(dfd, PATH, mode)
        case 35:  // unlinkat(dfd, PATH, flags)
        case 48:  // faccessat(dfd, PATH, mode)
        case 439: // faccessat2(dfd, PATH, mode, flags)
        case 53:  // fchmodat(dfd, PATH, mode, flags)
        case 54:  // fchownat(dfd, PATH, uid, gid, flags)
            a1 = nonpie_p(a1);
            break; //   path is a1 for the whole *at family
        case 437:
            a1 = nonpie_p(a1);
            a2 = nonpie_p(a2);
            break; // openat2(dfd, PATH, open_how*, size)
        case 79:   // newfstatat(dfd, PATH, STATBUF, flags)
        case 78:
            a1 = nonpie_p(a1);
            a2 = nonpie_p(a2);
            break; // readlinkat(dfd, PATH, BUF, sz)
        case 291:
            a1 = nonpie_p(a1);
            a4 = nonpie_p(a4);
            break; // statx(dfd, PATH, flags, mask, STATXBUF)
        case 36:
            a0 = nonpie_p(a0);
            a2 = nonpie_p(a2);
            break; // symlinkat(TARGET, newdfd, LINKPATH)
        case 37:   // linkat(odfd, OLD, ndfd, NEW, flags)
        case 38:   // renameat(odfd, OLD, ndfd, NEW)
        case 276:
            a1 = nonpie_p(a1);
            a3 = nonpie_p(a3);
            break;                         // renameat2(odfd, OLD, ndfd, NEW, flags)
        case 80:                           // fstat(fd, STATBUF)
        case 63:                           // read(fd, BUF, count)
        case 64:                           // write(fd, BUF, count)
        case 67:                           // pread64(fd, BUF, count, off)
        case 68:                           // pwrite64(fd, BUF, count, off)
        case 65:                           // readv(fd, IOVEC, n)  -- array base only
        case 200:                          // bind(fd, SOCKADDR, alen)
        case 203:                          // connect(fd, SOCKADDR, alen)
        case 204:                          // getsockname(fd, ADDR, alen)
        case 205:                          // getpeername(fd, ADDR, alen)
        case 202:                          // accept(fd, ADDR, alen)
        case 242:                          // accept4(fd, ADDR, alen, flags)
        case 61:                           // getdents64(fd, DIRENT_BUF, count)
        case 113:                          // clock_gettime(clkid, TIMESPEC)
        case 66: a1 = nonpie_p(a1); break; // writev(fd, IOVEC, n) -- array base only
        case 17:                           // getcwd(BUF, size)
        case 160:                          // uname(UTSBUF)
        case 73: a0 = nonpie_p(a0); break; // ppoll(FDS, n, tmo, sigmask, sz)
        case 207:                          // recvfrom(fd, BUF, len, fl, SRCADDR, alen)
        case 206:
            a1 = nonpie_p(a1);
            a4 = nonpie_p(a4);
            break;                          // sendto(fd, BUF, len, fl, SOCKADDR, alen)
        case 211:                           // sendmsg(fd, MSGHDR, flags) -- top only
        case 212: a1 = nonpie_p(a1); break; // recvmsg(fd, MSGHDR, flags) -- top only
        case 221:
            a0 = nonpie_p(a0);
            a1 = nonpie_p(a1);
            break; // execve(PATH, ARGV, envp); argv base here,
                   //   each argv[] element rebased at case 221
        default: break;
        }
    }
    // S2 path-resolution-cache invalidation: bump the epoch BEFORE dispatch on any syscall that mutates
    // the FS namespace, so no cached guest->host string mapping can survive a create/unlink/rename/mkdir/
    // symlink (over-invalidates, never under -- when in doubt, the next lookup MISSES and re-resolves).
    // Legacy x86 forms (open/mkdir/rename/...) were already normalized to these *at numbers by G_NORMALIZE.
    switch (nr) {
    case 33:  // mknodat
    case 34:  // mkdirat
    case 35:  // unlinkat (covers rmdir via AT_REMOVEDIR)
    case 36:  // symlinkat
    case 37:  // linkat
    case 38:  // renameat
    case 39:  // umount2
    case 40:  // mount
    case 276: // renameat2
        res_bump();
        break;
    case 56: // openat: a2 = Linux flags. O_CREAT (0x40) adds a name. In OVERLAY mode a write-open
             // (O_WRONLY/O_RDWR, a2&3) copies the file lower->upper, RELOCATING its resolved host path
             // -- so it must invalidate too, or a cached lower path goes stale (flat rootfs: no copy-up).
        if ((a2 & 0x40) || (g_nlower && (a2 & 3))) res_bump();
        break;
    case 437: { // openat2: flags live in open_how.flags (a2 -> struct open_how *), before its case rewrites a2
        uint64_t *how = (uint64_t *)a2;
        if (how && ((how[0] & 0x40) || (g_nlower && (how[0] & 3)))) res_bump();
        break;
    }
    default: break;
    }
    if (svc_sysv(c, nr, a0, a1, a2, a3, a4, a5)) return;
    if (svc_mem(c, nr, a0, a1, a2, a3, a4, a5)) return;
    if (svc_signal(c, nr, a0, a1, a2, a3, a4, a5)) return;
    if (svc_time(c, nr, a0, a1, a2, a3, a4, a5)) return;
    if (svc_io(c, nr, a0, a1, a2, a3, a4, a5)) return;
    if (svc_fs(c, nr, a0, a1, a2, a3, a4, a5)) return;
    if (svc_net(c, nr, a0, a1, a2, a3, a4, a5)) return;
    if (svc_event(c, nr, a0, a1, a2, a3, a4, a5)) return;
    if (svc_misc(c, nr, a0, a1, a2, a3, a4, a5)) return;
    if (svc_rare(c, nr, a0, a1, a2, a3, a4, a5)) return;
    switch (nr) {

    // ===================== Process & scheduling — clone/exec/wait/ids/prctl/futex/caps/sched =====================
    case 90: {
        if (a1) memset((void *)a1, 0xff, 12);
        G_RET(c) = 0;
        break;
        // capget -> all caps present
    }
    // capset -> ok
    case 91: G_RET(c) = 0; break;
    case 93:
        c->exited = 1;
        c->exit_code = (int)a0;
        // exit: end THIS thread
        break;
    // exit_group: end the whole process
    case 94:
        if (getenv("PROF"))
            fprintf(stderr,
                    "[prof] crossings=%llu syscalls=%llu ibtc_miss=%llu branch_cross=%llu translations=%llu lse=%llu "
                    "wx_toggles=%llu dualmap=%d xlate_ms=%.3f mtibtc=%d mtfill=%llu futexq=%d "
                    "fwake_fast=%llu fwake_slow=%llu fwait=%llu\n",
                    (unsigned long long)g_prof_cross, (unsigned long long)g_prof_sys, (unsigned long long)g_prof_miss,
                    (unsigned long long)(g_prof_cross - g_prof_sys - g_prof_miss), (unsigned long long)g_prof_xlate,
                    (unsigned long long)g_lse_n, (unsigned long long)g_wx_toggles, g_dualmap, g_xlate_ns / 1e6,
                    g_mtibtc, (unsigned long long)g_mtfill, g_futexq, (unsigned long long)g_futex_wake_fast,
                    (unsigned long long)g_futex_wake_slow, (unsigned long long)g_futex_wait_n);
        // A3: §B shadow-return coverage. hit-rate = shret_hit / (shret_hit + shret_fb). bl_shadow /
        // bl_leaf show how the depth-gate split call sites at translate time. PROF-only (keep dark).
        if (getenv("PROF")) {
            unsigned long long h = (unsigned long long)g_prof_shret_hit, f = (unsigned long long)g_prof_shret_fb;
            double hr = (h + f) ? 100.0 * (double)h / (double)(h + f) : 0.0;
            fprintf(
                stderr,
                "[prof] shadow_push=%llu shret_hit=%llu shret_fb=%llu hit_rate=%.1f%% bl_shadow=%llu bl_leaf=%llu\n",
                (unsigned long long)g_prof_shpush, h, f, hr, (unsigned long long)g_prof_bl_shadow,
                (unsigned long long)g_prof_bl_leaf);
        }
#ifdef R_REPSTR // W4-C: x86-only rep cmps/scas idiom firing counts
        if (getenv("PROF"))
            fprintf(stderr, "[prof] repstr=%llu repstr_elems=%llu\n", (unsigned long long)g_repstr_n,
                    (unsigned long long)g_repstr_elems);
#endif
#ifdef G_PROF_EXTRA
        G_PROF_EXTRA; // W5B: x86 tier-2 promotion counters
#endif
        ep_prof_dump(); // w3e: flush epoll kevent-syscall counter (atexit is bypassed by _exit)
        ib_dump();      // ARM-B1 IBPROF: indirect-branch traffic + stability report (no-op unless IBPROF)
        vt_dump();      // ARM-B1 VDBETRACE: threading prototype counters (no-op unless VDBETRACE)
        if (g_noexit) { // W3D fork-server prewarm: don't kill the resident parent; unwind run_guest instead
            c->exited = 1;
            c->exit_code = (int)a0;
            break;
        }
#ifdef PCACHE_SAVE_HOOK
        PCACHE_SAVE_HOOK; // opt8: persist the translated arena before the one-shot _exit (DDJIT_PCACHE only)
#endif
        _exit((int)a0);
    case 96:
        G_RET(c) = (uint64_t)getpid();
        // set_tid_address -> returns caller's TID (musl stores it; 0 -> a_crash())
        break;
    case 97:
    // unshare / setns -> ok (no real ns)
    case 268: G_RET(c) = 0; break;
    // futex
    case 98: G_RET(c) = (uint64_t)futex_op((int *)a0, (int)a1 & 0x7f, (int)a2, (struct timespec *)a3); break;
    // set_robust_list
    case 99: G_RET(c) = 0; break;
    // syslog
    case 116: G_RET(c) = 0; break;
    // sched_setaffinity(pid, size, MASK=a2) -- record the requested mask (intersected with the online
    // set) so a later getaffinity reflects the pin; -EINVAL if it selects no online CPU, as on Linux.
    case 122: {
        size_t n = (size_t)a1;
        if (n > sizeof g_affinity) n = sizeof g_affinity;
        if (a2 && n) {
            uint8_t online[sizeof g_affinity];
            cpu_online_mask(online, sizeof online);
            uint8_t want[sizeof g_affinity];
            int any = 0;
            for (size_t i = 0; i < n; i++) {
                want[i] = ((const uint8_t *)a2)[i] & online[i];
                if (want[i]) any = 1;
            }
            if (!any) {
                G_RET(c) = (uint64_t)(-EINVAL);
                break;
            }
            memset(g_affinity, 0, sizeof g_affinity);
            memcpy(g_affinity, want, n);
            g_affinity_set = 1;
        }
        G_RET(c) = 0;
        break;
    }
    case 123: {
        size_t n = (size_t)a1;
        // sched_getaffinity(pid,size,MASK=a2!) -- return the current mask (all online CPUs by default),
        // not just CPU 0, so CPU_COUNT() and tcmalloc's enumeration see the real width (mongod aborts).
        if (n > 128) n = 128;
        if (a2 && n) memcpy((void *)a2, affinity_mask(), n);
        // Return the number of bytes the mask spans (glibc zeroes the remainder); 8 covers <=64 CPUs.
        G_RET(c) = n < 8 ? (uint64_t)n : 8;
        break;
    }
    // sched_yield
    case 124: G_RET(c) = 0; break;
    case 140:
        setpriority((int)a0, (int)a1, (int)a2);
        G_RET(c) = 0;
        // setpriority (best-effort)
        break;
    case 141: {
        errno = 0;
        // getpriority -> Linux raw (20-nice)
        int r = getpriority((int)a0, (int)a1);
        G_RET(c) = (r == -1 && errno) ? (uint64_t)(-errno) : (uint64_t)(20 - r);
        break;
    }
    case 144:
    case 146:
    case 147:
    // setgid/setfsuid/setresuid/setresgid -> ok
    case 149: G_RET(c) = 0; break;
    // getpgid
    case 145: G_RET(c) = (uint64_t)getpgrp(); break;
    case 148: {
        // getresuid(r,e,s)
        if (a0) *(uint32_t *)a0 = cuid();
        if (a1) *(uint32_t *)a1 = cuid();
        if (a2) *(uint32_t *)a2 = cuid();
        G_RET(c) = 0;
        break;
    }
    case 150: {
        // getresgid(r,e,s)
        if (a0) *(uint32_t *)a0 = cgid();
        if (a1) *(uint32_t *)a1 = cgid();
        if (a2) *(uint32_t *)a2 = cgid();
        G_RET(c) = 0;
        break;
    }
    // setpgid -- bash job control. The container init has getpid()==1 (container_pid), so bash issues
    // setpgid(0, 1); forwarded verbatim that names launchd (host pid 1) -> EPERM ("initialize_job_control:
    // setpgid: Operation not permitted"). Map the faked PID1 self-reference to the host's own process, and
    // treat a residual EPERM as success -- a container is its own session, so guest process groups are virtual.
    case 154: {
        // Map the guest's view of the init (pid/pgid 1) to its real host pid/group, then do the REAL setpgid.
        // Children already carry real host pids, so they pass straight through and get real process groups.
        // EPERM is benign (the init is a session leader, already its own group leader) -> report success.
        pid_t pid = ((pid_t)a0 == 1 && g_init_hostpid) ? g_init_hostpid : (pid_t)a0;
        pid_t pgid = ((pid_t)a1 == 1 && g_init_hostpid) ? g_init_hostpid : (pid_t)a1;
        int r = setpgid(pid, pgid);
        G_RET(c) = (r < 0 && errno != EPERM) ? (uint64_t)(-errno) : 0;
        break;
    }
    // getpgid / getsid -- translate the init's real host group/session id to the guest's pgid 1 so the guest's
    // identity is self-consistent (getpid 1 == getpgrp 1 == getsid 1). bash then sees itself as session+group
    // leader and initializes job control WITHOUT the setpgid EPERM / "cannot set terminal process group"
    // warning -- it enables job control cleanly, and the real terminal handoff works (see TIOCSPGRP above +
    // the rt_sigprocmask stop-signal mirroring).
    case 155: {
        pid_t r = getpgid((pid_t)a0);
        if (g_init_hostpid && r == g_init_hostpid) r = 1;
        G_RET(c) = (uint64_t)r;
        break;
    }
    case 156: {
        pid_t r = getsid((pid_t)a0);
        if (g_init_hostpid && r == g_init_hostpid) r = 1;
        G_RET(c) = (uint64_t)r;
        break;
    }
    case 158: {
        if (g_gid >= 0) {
            if ((int)a0 >= 1 && a1) *(gid_t *)a1 = (gid_t)cgid();
            G_RET(c) = 1;
            break;
            // getgroups -> [container gid]
        }
        int r = getgroups((int)a0, (gid_t *)a1);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // setgroups (privileged; ignore)
    case 159: G_RET(c) = 0; break;
    // getrusage(who, *usage) -- a1 is the buffer, not a0!
    case 165: {
        struct rusage ru;
        // Linux RUSAGE_THREAD(1) -> SELF
        int who = ((int)a0 == -1) ? RUSAGE_CHILDREN : RUSAGE_SELF;
        if (a1) {
            uint8_t *d = (uint8_t *)a1;
            // Linux struct rusage layout (18 longs)
            memset(d, 0, 144);
            if (getrusage(who, &ru) == 0) {
                *(int64_t *)(d + 0) = ru.ru_utime.tv_sec;
                *(int64_t *)(d + 8) = ru.ru_utime.tv_usec;
                *(int64_t *)(d + 16) = ru.ru_stime.tv_sec;
                *(int64_t *)(d + 24) = ru.ru_stime.tv_usec;
                // macOS bytes -> Linux KB
                *(int64_t *)(d + 32) = ru.ru_maxrss / 1024;
                *(int64_t *)(d + 64) = ru.ru_minflt;
                *(int64_t *)(d + 72) = ru.ru_majflt;
                *(int64_t *)(d + 88) = ru.ru_inblock;
                *(int64_t *)(d + 96) = ru.ru_oublock;
                *(int64_t *)(d + 120) = ru.ru_nsignals;
                *(int64_t *)(d + 128) = ru.ru_nvcsw;
                *(int64_t *)(d + 136) = ru.ru_nivcsw;
            }
        }
        G_RET(c) = 0;
        break;
    }
    // prctl(option,...)
    case 167: {
        if ((int)a0 == 15) {
            snprintf(g_procname, sizeof g_procname, "%.15s", (const char *)a1);
            G_RET(c) = 0;
            break;
        } // PR_SET_NAME
        if ((int)a0 == 16) {
            snprintf((char *)a1, 16, "%s", g_procname);
            G_RET(c) = 0;
            break;
        } // PR_GET_NAME
        // 0 for known no-ops; EINVAL for unknown (kernel does)
        switch ((int)a0) {
        case 1:
        case 3:
        case 4:
        case 8:
        case 15:
        case 35:
        case 36:
        case 38:
        case 53:
        case 55:
        // PDEATHSIG/DUMPABLE/NAME/SECCOMP/TIMERSLACK/THP/SPECCTRL...
        case 59: G_RET(c) = 0; break;
        // EINVAL -- so feature probes (e.g. magic "AUXV") fail as on Linux
        default: G_RET(c) = (uint64_t)(-22); break;
        }
        break;
    }
    // getpid (PID ns: init -> 1)
    case 172: G_RET(c) = (uint64_t)container_pid(); break;
    case 173:
        G_RET(c) = (container_pid() == 1) ? 0 : (uint64_t)getppid();
        // getppid (init's parent is 0 in the ns)
        break;
    case 174:
    // getuid/geteuid -> container uid (0=root by default)
    case 175: G_RET(c) = (uint64_t)cuid(); break;
    case 176:
    // getgid/getegid
    case 177: G_RET(c) = (uint64_t)cgid(); break;
    // gettid
    case 178: G_RET(c) = (uint64_t)container_pid(); break;
    // clone(flags,stack,ptid,tls,ctid)
    case 220: {
        // CLONE_THREAD: stack arg IS the top
        if (a0 & 0x10000) {
            G_RET(c) = (uint64_t)spawn_thread(c, a0, a1, a3, a2, a4);
            break;
        }
        // fork/vfork: COW copy; child continues. Flush RAM-backed scratch into the real (shared) fds so
        // parent and child see one coherent file via the inherited description, exactly as POSIX requires
        // (the heap-resident buffers would otherwise COW-diverge while the fd stays shared).
        memf_materialize_all();
        pid_t pid = fork();
        if (pid == 0) {
            // clone(CLONE_VM, child_stack): glibc posix_spawn/popen/vfork pass a separate child stack in a1
            // and seed the clone trampoline (fn ptr + args) at its top. We fork() (COW) instead of sharing
            // the VM, but the child MUST run on a1 or glibc reads the trampoline off the parent's SP ->
            // garbage branch (SIGILL — broke initdb). a1==0 for a plain fork (bash), keeping the inherited SP.
            if ((a0 & 0x100) && a1) G_SP(c) = a1;
            // Re-assert MAP_JIT execute mode: the per-thread W^X/APRR state isn't reliable across fork(),
            // so the child's first run_block can instruction-abort fetching from the (non-executable) code
            // cache -> the intermittent fork+exec SIGBUS. pthread_jit_write_protect_np(1) = RX (executable).
            pthread_jit_write_protect_np(1);
            jit_after_fork();  // dual map: COW split the RW/RX aliases -> rebuild a fresh aliased cache
            G_SHADOW_RESET(c); // §B: child's pre-fork host_rets crossed run_block -> drop, use IBTC
            rc_reset();        // S2: drop the inherited (COW) path-resolution cache so the child can never
                               // serve a guest->host mapping that the parent populated before the FS diverged
            g_ndirs = 0;       // the getdents DIR* cache is the PARENT's -- closedir'ing inherited handles
                               // (on the child's close) crashes; drop it so the child re-fdopendir's fresh
#ifdef DD_HAS_MACH_EXC
            // The CRASHDBG Mach exception port + its receiver thread do NOT survive fork, so a crash in the
            // child silently dies. Clear the inherited task exception port so a fault falls through to the
            // POSIX diag_crash handler (which IS inherited) and reports fault=/pc=.
            if (getenv("CRASHDBG"))
                task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION,
                                         MACH_PORT_NULL, EXCEPTION_DEFAULT, 0);
#endif
        }
        // parent: pid, child: 0
        G_RET(c) = pid < 0 ? (uint64_t)(-errno) : (uint64_t)pid;
        break;
    }
    // execve(path, argv, envp)
    case 221: {
        memf_materialize_all(); // non-CLOEXEC scratch fds survive exec -> flush RAM into the real files
        char pb[4200];
        const char *p =
            // follow symlink rootfs-relative (busybox applets), through the overlay (upper then lowers)
            xresolve_overlay((const char *)a0, pb, sizeof pb);
        if (access(p, F_OK) != 0) {
            G_RET(c) = (uint64_t)(-2);
            break;
            // ENOENT
        }
        char *argv[256];
        int ac = 0;
        uint64_t *gv = (uint64_t *)a1; // a1 (argv array base) already nonpie_p()'d at the top redirect
        while (gv && gv[ac] && ac < 255) {
            argv[ac] = (char *)nonpie_p(gv[ac]); // each argv[] element may itself be a low-image pointer
            ac++;
        }
        argv[ac] = NULL;
        // Capture the guest-absolute exec path NOW (a0 is still mapped) so /proc/self/exe can name the new
        // image after the teardown below. ld.so resolves a binary's $ORIGIN (DT_RUNPATH) via readlink of
        // /proc/self/exe; a stale value makes an exec'd dynamic binary fail to find its own libraries (e.g.
        // rustup's proxy execs the real rustc, whose RUNPATH $ORIGIN/../lib must point into the toolchain).
        char gexe[4200];
        abs_guest(-100, (const char *)a0, gexe, sizeof gexe);
        // shebang: exec the #! interpreter instead (parse_shebang is shared with the initial loader)
        char sh_interp[256], sh_arg[256], shpb[4200];
        if (parse_shebang(p, sh_interp, sizeof sh_interp, sh_arg, sizeof sh_arg) == 1) {
            snprintf(gexe, sizeof gexe, "%s", sh_interp); // a script exec: /proc/self/exe names the interpreter
            char *na[258];
            int ni = 0;
            // [interp, (optarg), scriptpath, args...]
            na[ni++] = sh_interp;
            if (sh_arg[0]) na[ni++] = sh_arg;
            // the guest script path (interp re-opens it)
            na[ni++] = (char *)a0;
            for (int i = 1; i < ac && ni < 256; i++)
                na[ni++] = argv[i];
            na[ni] = NULL;
            // load the interpreter, not the script -- through the overlay (the #! interp, e.g. /bin/sh, may
            // live only in a read-only lower in a fresh container; xresolve_exec sees the upper alone -> ENOENT)
            p = xresolve_overlay(sh_interp, shpb, sizeof shpb);
            if (access(p, F_OK) != 0) {
                G_RET(c) = (uint64_t)(-2);
                break;
            }
            for (int i = 0; i <= ni; i++)
                argv[i] = na[i];
            ac = ni;
        }
        // Tear down the inherited guest address space before loading the new image: a post-fork exec
        // otherwise keeps the parent's DENSE layout, and load_elf must bias a non-PIE ET_EXEC off its
        // fixed vaddr (__PAGEZERO blocks the low 4 GB) -> its baked absolute refs collide -> SIGSEGV.
        // argv + path live in guest memory we're about to munmap, so copy them to the host heap first.
        char *xpath = strdup(p);
        char *xargv[256];
        for (int i = 0; i < ac && i < 255; i++)
            xargv[i] = strdup(argv[i]);
        xargv[ac < 255 ? ac : 255] = NULL;
        gmap_reset_all();
        g_nonpie_lo = g_nonpie_hi = 0; // reset; load_elf re-sets it iff the new main image is non-PIE
        p = xpath;
        for (int i = 0; i < ac && i < 255; i++)
            argv[i] = xargv[i];
        argv[ac < 255 ? ac : 255] = NULL;
        struct loaded lm;
        load_elf(p, &lm);
        uint64_t jump = lm.entry, at_base = 0;
        char interp[256];
        if (elf_interp(p, interp, sizeof interp) == 0) {
            char ib[4200];
            // follow+confine ld.so symlink (through the overlay)
            const char *ih = xresolve_overlay(interp, ib, sizeof ib);
            struct loaded li;
            load_elf(ih, &li);
            jump = li.entry;
            at_base = li.base;
        }
        g_cp = g_cache;
        memset(g_map, 0, sizeof g_map);
        // flush old translations
        g_npend = 0;
        memset(g_ibtc, 0, sizeof g_ibtc);
        // execve: drop IBTC + §B shadow (old image)
        G_SHADOW_RESET(c);
        uint8_t *heap = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        gmap_add((uint64_t)heap, 256u << 20);
        brk_lo = brk_cur = (uint64_t)heap;
        brk_hi = brk_lo + (256u << 20);
        uint64_t sp = build_stack(ac, argv, &lm, at_base);
        free(xpath);
        for (int i = 0; i < ac && i < 255; i++)
            free(xargv[i]);
        snprintf(g_exe_path_store, sizeof g_exe_path_store, "%s", gexe); // /proc/self/exe -> the new image
        g_exe_path = g_exe_path_store;
        G_RESET_REGS(c);
        c->nzcv = 0;
        G_TLS(c) = 0;
        G_SP(c) = sp;
        G_PC(c) = jump;
        // jump to new program; don't advance pc
        c->redirect = 1;
        break;
    }
    // wait4(pid, *status, opts, *rusage)
    case 260: {
        int st = 0;
        pid_t r;
        // SA_RESTART: a wait interrupted by a handler that asked to restart (e.g. a SIGCHLD reaper, or
        // gcc's driver) must transparently retry instead of failing the guest with EINTR.
        do { r = wait4((pid_t)(int)a0, &st, (int)a2, (struct rusage *)a3); } while (r < 0 && SVC_EINTR_RESTART(c));
        if (r < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        // WIFSIGNALED: macOS termsig -> Linux
        if ((st & 0x7f) != 0 && (st & 0x7f) != 0x7f) st = (st & ~0x7f) | (sig_m2l(st & 0x7f) & 0x7f);
        // WIFSTOPPED: macOS stopsig -> Linux
        else if ((st & 0xff) == 0x7f)
            st = (st & ~0xff00) | ((sig_m2l((st >> 8) & 0xff) & 0xff) << 8);
        if (a1) *(int *)a1 = st;
        G_RET(c) = (uint64_t)r;
        break;
    }
    case 261: {
        if (a3) {
            // prlimit64(pid,res,new,OLD): old=a3!
            uint64_t *o = (uint64_t *)a3;
            // RLIMIT_STACK=8MB, else unlimited
            o[0] = ((int)a1 == 3) ? (8ull << 20) : ~0ull;
            o[1] = ~0ull;
        }
        G_RET(c) = 0;
        break;
    }
    // clone3(clone_args*, size)
    case 435: {
        uint64_t *ca = (uint64_t *)a0;
        uint64_t flags = ca[0];
        // CLONE_THREAD: sp = stack + stack_size
        if (flags & 0x10000) {
            G_RET(c) = (uint64_t)spawn_thread(c, flags, ca[5] + ca[6], ca[7], ca[3], ca[2]);
            break;
        }
        pid_t pid = fork();
        // §B: same -- child drops the inherited shadow; S2: and the inherited path-resolution cache
        if (pid == 0) {
            jit_after_fork(); // dual map: rebuild the child's aliased cache (COW split RW/RX)
            G_SHADOW_RESET(c);
            rc_reset();
        }
        G_RET(c) = pid < 0 ? (uint64_t)(-errno) : (uint64_t)pid;
        break;
    }

    // ===================== unhandled =====================
    default:
        fprintf(stderr, "[jit] unhandled syscall %llu (a0=%llx a1=%llx) at pc=%llx\n", (unsigned long long)nr,
                (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)G_PC(c));
        G_RET(c) = (uint64_t)(-ENOSYS);
        // ENOSYS, keep going so we can see what's next
        break;
    }
    // Boundary errno translation: every case sets G_RET(c) to a host(macOS) errno on error
    // (-errno, saved e, helper returns, or a macOS E* constant). Map to the Linux errno the guest
    // expects. Skip redirect (sigreturn restored an already-Linux x0 from the signal frame).
    if (!c->redirect) {
        int64_t rv = (int64_t)G_RET(c);
        if (rv < 0 && rv >= -4095) G_RET(c) = (uint64_t)(-(int64_t)m2l_errno((int)(-rv)));
    }
}
