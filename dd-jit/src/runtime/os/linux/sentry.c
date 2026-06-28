// dd/runtime/os/linux -- sentry process-split: the untrusted-guest isolation trust boundary.
//
// THREAT MODEL. The whole guest->host authority crossing is run_guest()->service(c). A malicious
// translated guest can forge any register state at a syscall, so service() is the one place that
// turns guest intent into real host effects. For UNTRUSTED images we split that authority across a
// process boundary:
//
//   WORKER  (this process)  -- runs the JIT + translated (untrusted) guest. Keeps ONLY compute/
//                              memory authority: brk, anonymous mmap, futex, clocks, the inline
//                              fast-paths. Holds NO real fs/net fds. Under DDJIT_SANDBOX it is also
//                              wrapped in a deny-default macOS Seatbelt profile, so even a fully
//                              hijacked worker cannot reach the host fs/net directly -- only the ring.
//   SENTRY  (forked child)  -- holds host fs/net/proc authority. It owns the real fds and runs the
//                              real service_local() (path-jail, /proc synth, overlay, ...) on the host.
//
// They communicate over a POOL of shared-memory SPSC mailboxes (1-deep rings; guest syscalls are
// synchronous so depth 1 is what each gets exercised at). A guest thread is a host pthread and every
// host thread drives this path, so a SINGLE ring would have multiple producers and stall a threaded/
// forking guest -- instead each worker thread claims its own ring from the pool and the sentry runs one
// servicer thread per ring (see the per-context-ring section + roadmap item 1 below).
// The worker marshals {normalized syscall registers, inline buffer} into its ring; the sentry
// executes and returns {result/errno, out-buffer}. Guest memory is NOT shared -- only the marshaled
// bytes cross -- which IS the isolation: the sentry never dereferences a worker-controlled pointer
// into guest memory, and the worker never sees a real host fd (an openat result is an integer that
// is only meaningful inside the sentry; the subsequent read/write/close on it are ALSO forwarded, so
// the fd lives and dies entirely in the sentry).
//
// PORTABILITY. This file is #included into BOTH target unity TUs (linux_x86_64.c, linux_aarch64.c),
// next to service.c. It uses only the frontend-agnostic G_* ABI macros (G_NR/G_A0..G_A5/G_RET/
// G_NORMALIZE), so the marshaling is identical on either guest arch. The one register without an
// lvalue accessor is the raw syscall-number register (G_NR wraps it in canon_x86() on x86), so we
// add G_RAWNR, discriminated by G_PROF_EXTRA -- the same x86-vs-aarch64 switch service.c already uses.
//
// GATE. Everything here is dormant unless g_untrusted (env DDJIT_UNTRUSTED) is set. With the gate OFF
// (the default + the whole matrix) service() never calls syscall_route(); this TU contributes only a
// statically-predicted-not-taken branch. Byte-identical to baseline by construction.

#include <sched.h>
#include <sys/wait.h>
#include <stdatomic.h>

static int g_untrusted = 0;      // DDJIT_UNTRUSTED: route fs/net/proc syscalls through the sentry
static int g_sentry_sandbox = 0; // DDJIT_SANDBOX:   wrap the worker in a deny-default Seatbelt profile

// The raw syscall-number register. G_NR is not an lvalue on x86 (it wraps the rax read in canon_x86),
// so to RECONSTRUCT the call in the sentry we marshal the raw number register verbatim; the sentry's
// G_NR then re-derives the canonical number identically (canon_x86 on x86, identity on aarch64).
#ifdef G_PROF_EXTRA
#define G_RAWNR(c) ((c)->r[0]) // x86-64 guest: rax (post-normalize: the *at number / native number)
#else
#define G_RAWNR(c) ((c)->x[8]) // aarch64 guest: x8
#endif

// Inline payload per request: write data, read results, path strings. 1 MiB covers typical libc
// block sizes; larger transfers degrade to (legal) short reads/writes and the guest libc loops.
#define SENTRY_BUFSZ (1u << 20)

// Two-buffer (stat-family) layout inside buf[]: the in-path occupies [0, SENTRY_PATHCAP); the out
// struct lands at SENTRY_PATHCAP so the path and the struct never alias. The guest struct stat is
// per-arch (x86-64 = 144B, aarch64 = 128B; same x86-vs-aarch64 discriminator service.c uses for
// G_RAWNR); struct statx is arch-independent (256B).
#define SENTRY_PATHCAP 4096u
#ifdef G_PROF_EXTRA
#define SENTRY_STATSZ 144u // x86-64 guest struct stat
#else
#define SENTRY_STATSZ 128u // aarch64 guest struct stat
#endif
#define SENTRY_STATXSZ 256u
// readv/writev: cap the segment count we flatten (room for the iovec[] header before the data; the
// guest libc loops on a short scatter/gather just as it does on a short read/write).
#define SENTRY_IOVMAX 1024u

// Socket marshaling windows. The send/recv DATA payload lives in [0, SENTRY_DATACAP) just like
// read/write; the small socket-control buffers (sockaddr, the in/out socklen_t, optval) live in the
// buf[] TAIL so a sendto/recvfrom's data and its address never alias. A guest sockaddr is at most
// sizeof(struct sockaddr_storage)==128, but we give it 256B of slack; optval is tiny in practice so a
// 4KiB window covers every real socket option (a larger getsockopt cap is clamped, like the 1MiB data
// cap -- the guest libc never asks for more). All four windows fit below SENTRY_BUFSZ.
#define SENTRY_DATACAP   (SENTRY_BUFSZ - 8192u)               // sendto/recvfrom payload window [0,DATACAP)
#define SENTRY_SADDRCAP  256u                                 // sockaddr in/out window size (>= sockaddr_storage)
#define SENTRY_SADDR_OFF SENTRY_DATACAP                       // sockaddr in/out window offset
#define SENTRY_SLEN_OFF  (SENTRY_SADDR_OFF + SENTRY_SADDRCAP) // socklen_t in/out (addrlen / optlen) window
#define SENTRY_OPT_OFF   (SENTRY_SLEN_OFF + 64u)              // setsockopt/getsockopt optval window
#define SENTRY_OPTCAP    4096u                                // optval cap (real socket options are tiny)

// sendmsg/recvmsg (item 2). The guest msghdr GRAPH (msghdr + msg_name + scatter/gather msg_iov + the
// msg_control cmsg buffer) is flattened into the ring: the 56-byte msghdr COPY lives at [0,64); its
// msg_iov is a `struct iovec[]` at MSGIOV_OFF whose iov_base fields hold buf-relative OFFSETS, followed
// by the gathered (send) / reserved (recv) data; msg_name reuses the sockaddr tail window and msg_control
// reuses the optval tail window (a sendmsg never also does getsockopt, so the windows may alias per-op).
// The sentry rebases every offset to a ring pointer before service_local() runs the real Linux<->macOS
// translating sendmsg/recvmsg, so no guest pointer crosses. SCM_RIGHTS "just works": a guest fd in the
// control buffer is ALREADY a sentry-owned fd (every openat/socket/accept returns a sentry fd), so the
// marshaled cmsg carries fd integers the sentry can use verbatim -- no fd translation needed.
#define SENTRY_MSGHDR_SZ   56u                  // Linux LP64 struct msghdr (both guest arches identical)
#define SENTRY_MSGIOV_OFF  64u                  // iovec[] header start (after the 56B msghdr copy, aligned)
#define SENTRY_MSGNAME_OFF SENTRY_SADDR_OFF     // msg_name window (tail; reuses the sockaddr window)
#define SENTRY_MSGCTL_OFF  SENTRY_OPT_OFF       // msg_control window (tail; reuses the optval window)
#define SENTRY_MSGCTLCAP   SENTRY_OPTCAP        // control buffer cap (real SCM_RIGHTS payloads are tiny)

// Multiplexing windows (item 3): poll/ppoll pollfd array at buf[0] (8B/entry) + its timeout timespec in
// the sockaddr tail window; pselect's three fd_sets at 0/128/256 + timeout at 384 (each fd_set <=128B);
// epoll_pwait out-events at buf[0] (12B/entry). All sentry-owned fds, so the blocking call MUST run in
// the sentry (the fd lives there). fcntl flock / ioctl arg in/out windows reuse buf[0].
#define SENTRY_PSEL_RD  0u
#define SENTRY_PSEL_WR  128u
#define SENTRY_PSEL_EX  256u
#define SENTRY_PSEL_TMO 384u
#define SENTRY_POLL_TMO SENTRY_SADDR_OFF        // ppoll timeout timespec (tail; clear of the pollfd array)
#define SENTRY_EPEV_SZ  12u                     // packed Linux struct epoll_event {u32 events; u64 data}
#define SENTRY_IOCTLCAP 256u                    // ioctl arg in/out window (winsize/int/termios all fit)
#define SENTRY_FLOCKSZ  32u                     // Linux struct flock (fcntl F_GETLK/SETLK/SETLKW)

// Worker<->sentry fd passing (item 3). A sentry-owned fd that a LOCAL worker syscall must touch (a
// file-backed mmap's fd) is lent to the worker via SCM_RIGHTS over a per-ring AF_UNIX control socketpair:
// the worker maps it locally, then drops the borrowed fd -- so the worker's fds stay virtual and memory
// authority stays worker-side. Encoded as a sentinel in `rawnr` so the lend rides the SAME ring round-trip.
#define SENTRY_OP_FDPASS 0xFFFFFFFEu

// ------------------------------------------------------------------ per-context ring pool
// THE SCALING FIX. A single SPSC ring is single-producer: but a guest thread is a HOST pthread
// (os/linux/thread.c spawn_thread -> pthread_create) and EVERY host thread drives run_guest -> service
// -> syscall_route. So a multi-threaded (or forking) guest has SEVERAL host threads all marshaling onto
// one mailbox -- two producers corrupt the ping-pong and the 2nd worker stalls (busybox `sh` stalls at
// its first clone). The fix: a POOL of SENTRY_NRINGS independent rings, one claimed per worker thread,
// serviced by SENTRY_NRINGS sentry THREADS (one per ring) inside the SINGLE sentry process. It must be
// one process (threads, not forked children): the sentry owns the real fds and the guest's fd table is
// SHARED across its threads, so a socket opened while servicing ring A must be usable when servicing
// ring B -- only threads in one process share that fd table. Each worker thread claims a ring once
// (monotonic counter, mod N); at <=N concurrent threads every thread owns a private ring (zero
// contention); beyond N, overflow threads serialize on a per-ring producer lock (`busy`) -- still
// correct, just sharing a lane. (Follow-up: size the pool / hash by tid; wire execve/clone(fork) so a
// forked guest PROCESS gets its own worker registered with the sentry.)
#define SENTRY_NRINGS 8

// The shared-memory mailbox. `turn` is the ownership token: 0 => worker fills a request, 1 => sentry
// executes it. Strict ping-pong (no third state) => deadlock-free and, with release/acquire on turn,
// torn-message-free (all field writes happen-before the token flip the peer acquires).
struct sentry_ring {
    _Atomic uint32_t turn;  // 0 = worker owns (build request), 1 = sentry owns (execute)
    _Atomic uint32_t busy;  // worker-side producer lock: held across one round-trip (uncontended at <=N threads)
    _Atomic uint32_t owner; // ring-pool free-list: 0 = free lane, else the owning worker thread's token
    // request: the post-normalize syscall registers (frontend-agnostic via G_RAWNR / G_A0..G_A5)
    uint64_t rawnr; // raw syscall-number register (so the sentry's G_NR re-derives the canonical nr)
    uint64_t a[6];  // a0..a5 (G_A0..G_A5)
    // Generalized pointer marshaling: redir[i] is the byte offset within buf[] that arg i is redirected
    // to (or -1 to leave the register untouched). The sentry rebases a[i] -> buf+redir[i] AFTER bounds-
    // checking the offset, so service_local() only ever dereferences ring memory -- never a worker-
    // supplied guest pointer. Worker stages inputs (paths, write payloads, gathered iovec data) into
    // buf[] before the round-trip and copies outputs (read bytes, stat structs, scattered iovec data)
    // back into guest memory after it -- guest pointers stay entirely on the worker side.
    int32_t redir[6];
    // iovec ops (readv/writev): a1's redirected region is a `struct iovec[iovn]` whose iov_base fields
    // hold buf-relative OFFSETS (not pointers). The sentry bounds-checks each {offset,len} and rebases
    // iov_base to buf+offset, so even a hijacked worker cannot aim the scatter/gather outside the ring.
    uint32_t iovn;
    uint32_t inlen; // informational: input bytes staged in buf[] (measurement)
    // response
    int64_t ret;      // syscall return value, or -errno
    uint64_t nserved; // sentry-maintained counter (measurement / leak diagnostics)
    uint8_t buf[SENTRY_BUFSZ];
};

// The shared region: one teardown flag, one ring-claim counter (in shared memory so forked-worker
// PROCESSES also draw distinct rings from the pool), and the ring pool itself.
struct sentry_shm {
    _Atomic uint32_t quit;  // worker sets at teardown -> every sentry servicer thread _exit()s the process
    _Atomic uint32_t claim; // monotonic ring-claim counter (shared across worker threads AND forked workers)
    struct sentry_ring ring[SENTRY_NRINGS];
};

static struct sentry_shm *g_shm;
static pid_t g_sentry_pid;
static int g_ctl[SENTRY_NRINGS][2];   // per-ring AF_UNIX control socketpair: [.][0]=worker end, [.][1]=sentry
static pid_t g_worker_pid;            // this worker process's pid (changes in a forked-child worker)
static pid_t g_sentry_owner_pid;      // ONLY the process that forked the sentry may signal-quit + reap it
static _Atomic int g_guest_children;  // live guest-forked child WORKER processes owned by THIS worker proc
static __thread int t_ring = -1;      // this worker thread's claimed ring index (claimed lazily on first use)
static __thread uint32_t t_token = 0; // this worker thread's unique nonzero ring-ownership token

// Claim (once per worker thread) a ring from the pool. A reclaim-aware free-list: scan for a lane whose
// `owner` is 0 and CAS-claim it with this thread's unique token; at <=N concurrent threads every thread
// wins a private lane (zero contention). Beyond N (all lanes owned) overflow threads SHARE a lane keyed by
// token, serialized by the ring's `busy` producer lock (correctness preserved). ring_release() frees a
// lane on thread/process exit so a forking guest doesn't permanently exhaust the pool.
static struct sentry_ring *ring_for_thread(void) {
    if (t_ring < 0) {
        if (!t_token) // unique nonzero token from the shared claim counter (distinct across threads AND forked workers)
            t_token = atomic_fetch_add_explicit(&g_shm->claim, 1, memory_order_relaxed) + 1u;
        for (int i = 0; i < SENTRY_NRINGS; i++) {
            uint32_t expect = 0;
            if (atomic_compare_exchange_strong_explicit(&g_shm->ring[i].owner, &expect, t_token,
                                                        memory_order_acq_rel, memory_order_relaxed)) {
                t_ring = i;
                return &g_shm->ring[i];
            }
        }
        t_ring = (int)(t_token % SENTRY_NRINGS); // pool full: share a lane (busy-serialized)
    }
    return &g_shm->ring[t_ring];
}

// Release this thread's lane back to the pool (thread/process exit). CAS owner==t_token->0 so a SHARED
// (non-owned) lane is left untouched -- we only free a lane we actually own.
static void ring_release(void) {
    if (t_ring >= 0) {
        uint32_t mine = t_token;
        atomic_compare_exchange_strong_explicit(&g_shm->ring[t_ring].owner, &mine, 0, memory_order_acq_rel,
                                                memory_order_relaxed);
        t_ring = -1;
    }
}

// A worker that just forked itself (guest clone/clone3 without CLONE_THREAD) calls this in the CHILD: the
// child is a real process running the JIT, but it inherited the parent's ring lane + sentry-ownership +
// child bookkeeping. Adopt a fresh identity so the child draws its OWN lane (next forwarded syscall),
// never tears down the SHARED sentry, and tracks only its own children. NB: do NOT ring_release() here --
// the inherited owner token still belongs to the PARENT's still-live lane.
static void sentry_fork_child(void) {
    g_worker_pid = getpid(); // != g_sentry_owner_pid now -> sentry_shutdown() becomes a no-op in this child
    t_ring = -1;             // drop the inherited lane index; claim a fresh one lazily
    t_token = 0;             // mint a fresh ownership token on the next claim
    g_guest_children = 0;    // the child starts with no children of its own
}

// SCM_RIGHTS fd passing over a control socketpair. sentry_send_fd lends one fd; sentry_recv_fd borrows it.
// A NULL control message (fd<0) is still sent so the worker's recv stays in lockstep with the round-trip.
static void sentry_send_fd(int sock, int fd) {
    struct msghdr m;
    memset(&m, 0, sizeof m);
    char b = 0;
    struct iovec io = {&b, 1};
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;
    if (fd >= 0) {
        memset(u.buf, 0, sizeof u.buf);
        m.msg_control = u.buf;
        m.msg_controllen = sizeof u.buf;
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    while (sendmsg(sock, &m, 0) < 0 && errno == EINTR) {}
}
static int sentry_recv_fd(int sock) {
    struct msghdr m;
    memset(&m, 0, sizeof m);
    char b = 0;
    struct iovec io = {&b, 1};
    m.msg_iov = &io;
    m.msg_iovlen = 1;
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;
    m.msg_control = u.buf;
    m.msg_controllen = sizeof u.buf;
    ssize_t r;
    while ((r = recvmsg(sock, &m, 0)) < 0 && errno == EINTR) {}
    if (r < 0) return -1;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
        int fd;
        memcpy(&fd, CMSG_DATA(c), sizeof(int));
        return fd;
    }
    return -1;
}

// ioctl arg sizing (item 3). Marshaling a fixed window would clobber guest memory on copy-back (an
// FIONREAD's 4-byte int would get 256 bytes splattered over it). Modern ioctl numbers encode their size +
// direction (_IOC_SIZE/_IOC_DIR); the legacy 0x54xx terminal numbers don't, so table those. Returns the
// exact in (arg->kernel) and out (kernel->arg) byte counts for every request service_local() handles.
static void sentry_ioctl_sizes(unsigned long rq, uint32_t *insz, uint32_t *outsz) {
    uint32_t enc = (uint32_t)((rq >> 16) & 0x3fffu); // _IOC_SIZE
    uint32_t dir = (uint32_t)((rq >> 30) & 0x3u);    // 1=_IOC_WRITE(arg->kernel) 2=_IOC_READ(kernel->arg)
    if (enc) {
        *insz = (dir & 1u) ? enc : 0;
        *outsz = (dir & 2u) ? enc : 0;
        if (!dir) { *insz = enc; *outsz = enc; } // _IOC_NONE w/ a size: be permissive
        return;
    }
    switch (rq) {
    case 0x5401: *insz = 0;  *outsz = 36; return; // TCGETS    (struct termios out)
    case 0x5402:                                  // TCSETS
    case 0x5403:                                  // TCSETSW
    case 0x5404: *insz = 36; *outsz = 0;  return; // TCSETSF   (struct termios in)
    case 0x5413: *insz = 0;  *outsz = 8;  return; // TIOCGWINSZ(struct winsize out)
    case 0x5414: *insz = 8;  *outsz = 0;  return; // TIOCSWINSZ(struct winsize in)
    case 0x5421: *insz = 4;  *outsz = 0;  return; // FIONBIO   (int in)
    case 0x541b:                                  // FIONREAD  (int out)
    case 0x540f: *insz = 0;  *outsz = 4;  return; // TIOCGPGRP (int out)
    default:     *insz = 0;  *outsz = 0;  return; // FIOCLEX/FIONCLEX/TIOCSCTTY/.../unknown: no arg payload
    }
}

// ------------------------------------------------------------------ authority split
// Returns 1 if this CANONICAL syscall number carries fs/net/proc authority and must be executed by
// the sentry; 0 if it is compute/memory-only and stays LOCAL in the worker. This first PR forwards
// the read/write/open family; the comment lists the full set the production split will forward.
static int sentry_forwarded(uint64_t nr) {
    switch (nr) {
    case 56: // openat
    case 57: // close
    case 62: // lseek
    case 63: // read
    case 64: // write
    // --- fs set wired in this PR ---
    case 61:  // getdents64  (out-buffer the dirents)         [NOTE: 78 in the old comment was wrong = readlinkat]
    case 65:  // readv       (flatten guest iovec -> ring)
    case 66:  // writev      (flatten guest iovec -> ring)
    case 67:  // pread64      (read + a3 offset)
    case 68:  // pwrite64     (write + a3 offset)
    case 79:  // newfstatat   (in-path a1 + out-struct a2, two-buffer)
    case 80:  // fstat        (out-struct a1)
    case 291: // statx        (in-path a1 + out-struct a4, two-buffer)
    // --- socket family wired in this PR (the sentry owns the real socket fd; only sockaddr/optval/data
    //     bytes cross the ring, never a guest pointer; all AF/port-map/jail translation stays in
    //     service_local) ---
    case 198: // socket       (no pointer args; returns a sentry-owned fd, virtual to the worker)
    case 201: // listen       (no pointer args; operates on the sentry-owned socket fd)
    case 200: // bind         (in-sockaddr a1, len a2)
    case 203: // connect      (in-sockaddr a1, len a2)
    case 202: // accept       (out-sockaddr a1, in/out socklen a2)
    case 242: // accept4      (out-sockaddr a1, in/out socklen a2, flags a3)
    case 204: // getsockname  (out-sockaddr a1, in/out socklen a2)
    case 205: // getpeername  (out-sockaddr a1, in/out socklen a2)
    case 206: // sendto       (in-data a1/a2 + in-destaddr a4/a5)
    case 207: // recvfrom     (out-data a1/a2 + out-srcaddr a4, in/out socklen a5)
    case 208: // setsockopt   (in-optval a3, len a4)
    case 209: // getsockopt   (out-optval a3, in/out optlen a4)
    case 210: // shutdown     (no pointer args)
    // --- sendmsg/recvmsg + SCM_RIGHTS (item 2): nested msghdr graph marshaled to the ring ---
    case 211: // sendmsg      (msghdr: msg_name + scatter/gather msg_iov + msg_control)
    case 212: // recvmsg      (msghdr out: name + scattered data + control + flags)
    // --- multiplexing over sentry-owned fds (item 3): these BLOCK on an fd that lives in the sentry ---
    case 72:  // pselect6     (three fd_sets in/out + timeout)
    case 73:  // ppoll        (pollfd array in/out + timeout)
    case 20:  // epoll_create1(returns a sentry-owned kqueue fd)
    case 21:  // epoll_ctl    (in epoll_event; operates on sentry fds)
    case 22:  // epoll_pwait  (out events buffer)
    // --- fd-table ops on sentry-owned fds (item 3): keep the guest fd space entirely sentry-side ---
    case 23:  // dup          (returns a sentry fd)
    case 24:  // dup3         (operates on / returns sentry fds)
    case 25:  // fcntl        (F_SETFL nonblock, F_DUPFD, F_GETLK/SETLK flock in/out)
    case 29:  // ioctl        (FIONBIO/FIONREAD/winsize/termios arg in/out)
    case 59:  // pipe2        (out int[2] -- both ends sentry-owned)
    case 199: // socketpair   (out int[2] -- both ends sentry-owned)
        return 1;
    // --- handled SPECIALLY in syscall_route (NOT via this table): 220/435 clone(fork) lane, 221 execve
    //     (stays local -- it reloads the guest image in-process, keeping the worker's ring/sentry), 260
    //     wait4 (reaps child WORKERS), 222 file-backed mmap (SCM_RIGHTS fd-lend). See syscall_route. ---
    default:
        return 0;
    }
}

// ------------------------------------------------------------------ Seatbelt (worker confinement)
// Deny-default profile for the WORKER. Anonymous memory / signals / threads are allowed; ALL file and
// network operations are denied -- the worker can reach the host ONLY through the sentry ring.
//
// SOUNDNESS: enabling this is only correct once the FULL fs/net/proc set is forwarded. With just the
// read/write/open family forwarded (this PR), any still-local fs syscall (uname/readlink/getcwd-on-
// host/...) would be denied and break a general guest. So DDJIT_SANDBOX is OFF by default and is
// currently sound only for guests whose entire syscall surface is the forwarded family.
static const char *k_worker_sbpl =
    "(version 1)\n"
    "(deny default)\n"
    "(allow process-fork)\n"
    "(allow process-info* (target self))\n"
    "(allow signal (target self))\n"
    "(allow sysctl-read)\n"
    // mach-lookup reaches the bootstrap server / WindowServer -- the classic macOS sandbox-escape primitive.
    // Default-deny it explicitly (belt-and-suspenders over (deny default)); the JIT worker is pure compute/
    // memory after the post-fork confinement point and needs no bootstrap services. mach-priv-task-port is
    // scoped to (target self) so a popped worker cannot grab another task's port.
    "(deny mach-lookup (global-name-regex #\".*\"))\n"
    "(allow mach-priv-task-port (target self))\n"
    "(deny file-write*)\n"    // no host writes  -- only via the sentry
    "(deny file-read-data)\n" // no host reads   -- only via the sentry
    "(deny network*)\n";      // no host sockets -- only via the sentry

#ifdef __APPLE__
extern int sandbox_init(const char *profile, uint64_t flags, char **errorbuf);
extern void sandbox_free_error(char *errorbuf);
#endif

static void worker_sandbox(void) {
#ifdef __APPLE__
    char *err = 0;
    if (sandbox_init(k_worker_sbpl, 0 /* literal profile, not SANDBOX_NAMED */, &err) != 0) {
        fprintf(stderr, "[sentry] worker Seatbelt sandbox_init failed: %s\n", err ? err : "(null)");
        if (err) sandbox_free_error(err);
        // FAIL CLOSED: an untrusted worker that could not be confined must NOT run unconfined -- abort it
        // rather than expose the host fs/net directly. (DDJIT_SANDBOX is opt-in; the operator asked for it.)
        _exit(72);
    }
    fprintf(stderr, "[sentry] worker confined under deny-default Seatbelt profile\n");
#endif
}

// ------------------------------------------------------------------ guest fd-ownership set (P1: FDPASS / SCM)
// SCM_RIGHTS fd-lend (SENTRY_OP_FDPASS) must only ever surface an fd the sentry opened ON BEHALF OF THE
// GUEST -- never one of the sentry's own control sockets (g_ctl[]), the daemon stdio, or any other non-guest
// host fd. We record every fd service_local() hands back for this guest (openat/socket/accept*/dup*/pipe2/
// socketpair/fcntl-F_DUPFD/recvmsg-SCM_RIGHTS) in a sentry-PRIVATE bitset and clear it on close. This array
// lives in the sentry process's own memory (NOT the shared ring) so a malicious worker cannot tamper with
// it; it IS shared across the per-ring servicer threads (one sentry process), so mutate with atomic word
// ops. An fd >= the cap is untrackable and therefore never lendable -> rejected -EBADF.
#define SENTRY_FDSET_MAX 65536u
static _Atomic uint64_t g_guest_fds[SENTRY_FDSET_MAX / 64];
static void sentry_fd_track(int fd) {
    if (fd < 0 || (unsigned)fd >= SENTRY_FDSET_MAX) return;
    atomic_fetch_or_explicit(&g_guest_fds[(unsigned)fd >> 6], 1ull << ((unsigned)fd & 63), memory_order_relaxed);
}
static void sentry_fd_untrack(int fd) {
    if (fd < 0 || (unsigned)fd >= SENTRY_FDSET_MAX) return;
    atomic_fetch_and_explicit(&g_guest_fds[(unsigned)fd >> 6], ~(1ull << ((unsigned)fd & 63)), memory_order_relaxed);
}
static int sentry_fd_owned(int fd) {
    if (fd < 0 || (unsigned)fd >= SENTRY_FDSET_MAX) return 0;
    return (int)((atomic_load_explicit(&g_guest_fds[(unsigned)fd >> 6], memory_order_relaxed) >> ((unsigned)fd & 63)) & 1u);
}
// Walk a (sentry-produced, Linux-layout) cmsg buffer and track every SCM_RIGHTS fd a recvmsg received, so a
// later FDPASS of that fd is recognized as guest-owned. Strictly bounded by `len` -- never derefs past it.
static void sentry_track_cmsg_fds(const uint8_t *ctl, size_t len) {
    size_t o = 0;
    while (o + 16u <= len) {                          // Linux struct cmsghdr: {u64 cmsg_len; int level; int type}
        uint64_t clen = *(const uint64_t *)(ctl + o);
        int level = *(const int *)(ctl + o + 8);
        int type = *(const int *)(ctl + o + 12);
        if (clen < 16u || o + clen > len) break;
        if (level == SOL_SOCKET && type == SCM_RIGHTS) {
            size_t nfd = (size_t)(clen - 16u) / sizeof(int);
            for (size_t i = 0; i < nfd; i++) sentry_fd_track(*(const int *)(ctl + o + 16u + i * sizeof(int)));
        }
        o += (size_t)((clen + 7u) & ~(uint64_t)7u);  // CMSG_ALIGN to 8
    }
}

// ------------------------------------------------------------------ sentry process body
// Holds host authority. Services ONE marshaled request on ring R: rebuilds a cpu from the marshaled
// registers, redirects each flagged guest-buffer pointer arg into the shared ring (so service_local()
// never touches worker/guest memory) -- including rebasing the flattened readv/writev iovec offsets to
// ring pointers -- and runs the REAL service_local() -- identical jail/proc/overlay policy, identical
// bytes. NOTE: it MUST call service_local() (the canonical switch), not service() -- service() would
// re-enter syscall_route() in this (g_untrusted) process and recurse onto the ring.
static void sentry_service_one(struct sentry_ring *R) {
    // fd-lend (item 3): not a syscall -- lend a sentry-owned fd to the worker over THIS ring's control
    // socketpair (SCM_RIGHTS) for a file-backed mmap; the worker maps it locally then drops it. OWNERSHIP
    // (P1, finding F): the lendable fd MUST be one the sentry opened ON BEHALF OF THE GUEST (tracked at
    // openat/socket/accept/dup/pipe2/socketpair/...). An arbitrary worker-named integer -- the sentry's own
    // g_ctl[] control socket, the daemon stdio, any non-guest host fd -- is rejected -EBADF. Detected before
    // any cpu reconstruction. We ALWAYS send a control datagram (with the fd, or empty on reject) so the
    // worker's matching recv stays in lockstep with the round-trip and never desyncs the next lend.
    if (R->rawnr == SENTRY_OP_FDPASS) {
        int idx = (int)(R - g_shm->ring);
        int fd = (int)(int64_t)R->a[0];
        if (sentry_fd_owned(fd)) {
            sentry_send_fd(g_ctl[idx][1], fd);
            R->ret = 0;
        } else {
            sentry_send_fd(g_ctl[idx][1], -1); // empty datagram: keep the worker recv in lockstep
            R->ret = -EBADF;
        }
        R->nserved++;
        return;
    }
    struct cpu tmp;
    memset(&tmp, 0, sizeof tmp);
    G_RAWNR(&tmp) = R->rawnr; // service_local() re-runs G_NORMALIZE on this as a no-op (already *at)
    G_A0(&tmp) = R->a[0];
    G_A1(&tmp) = R->a[1];
    G_A2(&tmp) = R->a[2];
    G_A3(&tmp) = R->a[3];
    G_A4(&tmp) = R->a[4];
    G_A5(&tmp) = R->a[5];
    // Snapshot the pointer-redirect metadata into sentry-PRIVATE locals: from here on every validation reads
    // these copies, NEVER the attacker-writable shared ring, so a racing worker thread cannot rewrite a
    // field between our check and the kernel's use of it (the validate-in-place TOCTOU, finding E). Scalars
    // are already snapshotted into `tmp`; this extends the same discipline to the redir table + iovn.
    int32_t redir[6];
    for (int i = 0; i < 6; i++) redir[i] = R->redir[i];
    uint32_t iovn = R->iovn;

    // Redirect each flagged pointer arg into the ring buffer (THE crossing point: from here on
    // service_local() touches only ring/private memory). Bounds-check every worker-supplied offset first --
    // an out-of-range offset is a hijacked/buggy worker and faults the call rather than the sentry.
    int bad = 0;
    uint32_t off[6] = {0, 0, 0, 0, 0, 0};
    int have[6] = {0, 0, 0, 0, 0, 0};
    uint64_t *ta[6] = {&G_A0(&tmp), &G_A1(&tmp), &G_A2(&tmp), &G_A3(&tmp), &G_A4(&tmp), &G_A5(&tmp)};
    for (int i = 0; i < 6; i++) {
        if (redir[i] < 0) continue;
        uint32_t o = (uint32_t)redir[i];
        if (o >= SENTRY_BUFSZ) { bad = 1; break; }
        off[i] = o;
        have[i] = 1;
        *ta[i] = (uint64_t)(R->buf + o);
    }

    uint64_t snr = bad ? 0 : G_NR(&tmp);

    // Per-servicer-thread PRIVATE iovec[] -- the kernel scatters/gathers through THIS, not the shared ring,
    // so a racing worker thread cannot move a segment after we validated it (finding E). 16B/seg * IOVMAX.
    static __thread struct iovec piov[SENTRY_IOVMAX];
    socklen_t pslen = 0; // PRIVATE in/out socklen: the kernel never sources the length from shared memory
    int slen_back = 0;   // after the call, mirror pslen back into the SLEN window for the worker copy-back
    uint8_t ph[64];      // PRIVATE Linux-layout 56-byte msghdr copy (sendmsg/recvmsg graph)
    int msg_built = 0;
    uint64_t coff = 0;   // recvmsg control-window offset (for the SCM_RIGHTS fd-track after the call)

    // ---- P0 finding A/D: clamp EVERY length the kernel will use to read/write buf[] down to the bytes
    //      actually remaining in that ring window (BUFSZ - offset). Correct traffic is already inside its
    //      window, so the min() is a no-op for it; only a hostile over-large length is cut. The worker-side
    //      caps are NOT a security control -- this is the sentry re-deriving the bound from the redir window.
    //      In/out socklen/optlen values are routed through PRIVATE storage (pslen) so the kernel reads the
    //      clamped capacity from sentry memory, race-free, and the output is mirrored back afterwards. ----
    if (!bad) {
        switch (snr) {
        case 61: case 63: case 67: // getdents64 / read / pread64: a2 = byte count through buf+off[1]
        case 64: case 68:          // write / pwrite64
        case 200: case 203:        // bind / connect: a2 = addrlen through buf+off[1]
            if (have[1] && G_A2(&tmp) > (uint64_t)(SENTRY_BUFSZ - off[1])) G_A2(&tmp) = SENTRY_BUFSZ - off[1];
            break;
        case 206:                  // sendto: a2 = data len (off[1]); a5 = destaddr len (off[4])
            if (have[1] && G_A2(&tmp) > (uint64_t)(SENTRY_BUFSZ - off[1])) G_A2(&tmp) = SENTRY_BUFSZ - off[1];
            if (have[4] && G_A5(&tmp) > (uint64_t)(SENTRY_BUFSZ - off[4])) G_A5(&tmp) = SENTRY_BUFSZ - off[4];
            break;
        case 207:                  // recvfrom: a2 = data len; a5 = in/out socklen -> PRIVATE (clamped to window)
            if (have[1] && G_A2(&tmp) > (uint64_t)(SENTRY_BUFSZ - off[1])) G_A2(&tmp) = SENTRY_BUFSZ - off[1];
            if (have[5]) {
                pslen = *(socklen_t *)(R->buf + SENTRY_SLEN_OFF);
                if (pslen > SENTRY_SADDRCAP) pslen = SENTRY_SADDRCAP;
                G_A5(&tmp) = (uint64_t)&pslen;
                slen_back = 1;
            }
            break;
        case 202: case 242:        // accept / accept4
        case 204: case 205:        // getsockname / getpeername: a2 = in/out socklen -> PRIVATE (clamped)
            if (have[2]) {
                pslen = *(socklen_t *)(R->buf + SENTRY_SLEN_OFF);
                if (pslen > SENTRY_SADDRCAP) pslen = SENTRY_SADDRCAP;
                G_A2(&tmp) = (uint64_t)&pslen;
                slen_back = 1;
            }
            break;
        case 208:                  // setsockopt: a4 = optlen through buf+off[3]
            if (have[3] && G_A4(&tmp) > (uint64_t)(SENTRY_BUFSZ - off[3])) G_A4(&tmp) = SENTRY_BUFSZ - off[3];
            break;
        case 209:                  // getsockopt: a4 = in/out optlen -> PRIVATE (clamped to the optval window)
            if (have[4]) {
                pslen = *(socklen_t *)(R->buf + SENTRY_SLEN_OFF);
                if (pslen > SENTRY_OPTCAP) pslen = SENTRY_OPTCAP;
                G_A4(&tmp) = (uint64_t)&pslen;
                slen_back = 1;
            }
            break;
        case 73:                   // ppoll: a1 = nfds (8B/entry) into the pollfd window [0,DATACAP)
            if (G_A1(&tmp) > (uint64_t)(SENTRY_DATACAP / 8u)) G_A1(&tmp) = SENTRY_DATACAP / 8u;
            break;
        case 72:                   // pselect6: a0 = nfds -> (nfds+7)/8 <= 128B fits each fd_set window
            if (G_A0(&tmp) > 1024u) G_A0(&tmp) = 1024u;
            break;
        case 22:                   // epoll_pwait: a2 = maxevents (12B/entry) into the out window [0,BUFSZ)
            if (have[1] && G_A2(&tmp) > (uint64_t)(SENTRY_BUFSZ / SENTRY_EPEV_SZ)) G_A2(&tmp) = SENTRY_BUFSZ / SENTRY_EPEV_SZ;
            break;
        case 56: case 79: case 291: // openat / newfstatat / statx: force the in-path NUL-terminated within
            R->buf[SENTRY_PATHCAP - 1] = 0; // its window so service_local()'s C-string walk can't run off buf
            break;
        default: break;
        }
    }

    // ---- P0 finding B/E: readv/writev -- bound the segment count, reject a wild base, then COPY the iovec[]
    //      descriptor array OUT of the shared ring into private memory, validate the copy, and point the
    //      kernel at it. (We also mirror the validated descriptors back into buf[] for the worker's own
    //      scatter copy-back; that read is worker-side / intra-principal, not a sentry crossing.) ----
    if (!bad && iovn) {
        if (!have[1]) {
            bad = 1; // iovn>0 with no valid a1 redir window would be a wild deref off buf[] -- reject (finding B.2)
        } else {
            uint32_t maxn = (uint32_t)((SENTRY_BUFSZ - off[1]) / sizeof(struct iovec));
            if (iovn > SENTRY_IOVMAX) iovn = SENTRY_IOVMAX;
            if (iovn > maxn) iovn = maxn;
            struct iovec *iv = (struct iovec *)(R->buf + off[1]); // shared (attacker-writable)
            for (uint32_t k = 0; k < iovn; k++) {
                uint64_t boff = (uint64_t)(uintptr_t)iv[k].iov_base, len = iv[k].iov_len; // read ONCE
                if (boff > SENTRY_BUFSZ || len > SENTRY_BUFSZ || boff + len > SENTRY_BUFSZ) {
                    piov[k].iov_base = R->buf;        piov[k].iov_len = 0; // bad seg -> empty (don't escape the ring)
                    iv[k].iov_base = R->buf;          iv[k].iov_len = 0;
                } else {
                    piov[k].iov_base = R->buf + boff; piov[k].iov_len = (size_t)len;
                    iv[k].iov_base = R->buf + boff;   iv[k].iov_len = (size_t)len;
                }
            }
            G_A1(&tmp) = (uint64_t)piov; // kernel reads the PRIVATE iovec[]
            G_A2(&tmp) = iovn;
        }
    }

    // ---- P0 finding C/E: sendmsg/recvmsg -- build the WHOLE msghdr graph in private memory: a Linux-layout
    //      56-byte header pointing at the private iovec[], with msg_namelen/msg_controllen clamped to their
    //      windows. service_local() reads/writes this private header; nothing it touches is re-read by the
    //      kernel from attacker-writable shared memory. (R->iovn stays 0 for these so the block above is skipped.)
    if (!bad && (snr == 211 || snr == 212)) {
        uint8_t *h = R->buf;
        uint64_t noff = *(uint64_t *)(h + 0);
        uint32_t nlen = *(uint32_t *)(h + 8);
        uint64_t ioff = *(uint64_t *)(h + 16);
        uint64_t in = *(uint64_t *)(h + 24);
        uint64_t clen = *(uint64_t *)(h + 40);
        uint32_t mflags = *(uint32_t *)(h + 48);
        coff = *(uint64_t *)(h + 32);
        if (noff >= SENTRY_BUFSZ || ioff >= SENTRY_BUFSZ || coff >= SENTRY_BUFSZ) {
            bad = 1;
        } else {
            memset(ph, 0, sizeof ph);
            if (noff) {
                if (nlen > (uint32_t)(SENTRY_BUFSZ - noff)) nlen = (uint32_t)(SENTRY_BUFSZ - noff);
                *(uint64_t *)(ph + 0) = (uint64_t)(R->buf + noff); // msg_name -> ring ptr
                *(uint32_t *)(ph + 8) = nlen;                      // msg_namelen, clamped to window
            }
            uint32_t n = 0;
            if (ioff) {
                uint32_t maxn = (uint32_t)((SENTRY_BUFSZ - ioff) / sizeof(struct iovec));
                n = (in > SENTRY_IOVMAX) ? SENTRY_IOVMAX : (uint32_t)in; // bound msg_iovlen (finding C)
                if (n > maxn) n = maxn;
                struct iovec *iv = (struct iovec *)(R->buf + ioff);
                for (uint32_t k = 0; k < n; k++) {
                    uint64_t boff = (uint64_t)(uintptr_t)iv[k].iov_base, len = iv[k].iov_len;
                    if (boff > SENTRY_BUFSZ || len > SENTRY_BUFSZ || boff + len > SENTRY_BUFSZ) {
                        piov[k].iov_base = R->buf;        piov[k].iov_len = 0;
                        iv[k].iov_base = R->buf;          iv[k].iov_len = 0;
                    } else {
                        piov[k].iov_base = R->buf + boff; piov[k].iov_len = (size_t)len;
                        iv[k].iov_base = R->buf + boff;   iv[k].iov_len = (size_t)len;
                    }
                }
                *(uint64_t *)(ph + 16) = (uint64_t)piov; // msg_iov -> PRIVATE iovec[]
            }
            *(uint64_t *)(ph + 24) = n;
            if (coff) {
                if (clen > (uint64_t)(SENTRY_BUFSZ - coff)) clen = SENTRY_BUFSZ - coff;
                *(uint64_t *)(ph + 32) = (uint64_t)(R->buf + coff); // msg_control -> ring ptr
                *(uint64_t *)(ph + 40) = clen;                      // msg_controllen, clamped to window
            }
            *(uint32_t *)(ph + 48) = mflags;
            G_A1(&tmp) = (uint64_t)ph; // service_local reads/writes the PRIVATE msghdr
            msg_built = 1;
        }
    }

    if (bad) {
        R->ret = -EFAULT;
        R->nserved++;
        return;
    }

    service_local(&tmp); // real host authority + container policy (touches only ring + private memory now)
    int64_t ret = (int64_t)G_RET(&tmp);
    R->ret = ret;

    // Mirror PRIVATE out-values back into the ring so the worker's copy-back into guest memory sees them.
    if (slen_back) *(socklen_t *)(R->buf + SENTRY_SLEN_OFF) = pslen;
    if (msg_built && snr == 212) {
        *(uint32_t *)(R->buf + 8) = *(uint32_t *)(ph + 8);   // updated msg_namelen
        *(uint64_t *)(R->buf + 40) = *(uint64_t *)(ph + 40); // updated msg_controllen
        *(uint32_t *)(R->buf + 48) = *(uint32_t *)(ph + 48); // updated msg_flags
    }

    // ---- P1 finding F: record every fd the sentry just handed THIS guest, so only such fds are
    //      FDPASS-lendable; drop it on close. (g_ctl[]/stdio/non-guest host fds are never tracked.) ----
    switch (snr) {
    case 56: case 198: case 202: case 242: case 23: case 24: case 20: // openat/socket/accept*/dup*/epoll_create1
        if (ret >= 0) sentry_fd_track((int)ret);
        break;
    case 25: // fcntl F_DUPFD(0) / F_DUPFD_CLOEXEC(1030) return a new fd
        if ((G_A1(&tmp) == 0 || G_A1(&tmp) == 1030) && ret >= 0) sentry_fd_track((int)ret);
        break;
    case 59: case 199: // pipe2 / socketpair: the two new fds landed at buf[0..8)
        if (ret == 0) { sentry_fd_track(*(int *)(R->buf)); sentry_fd_track(*(int *)(R->buf + 4)); }
        break;
    case 57: // close
        if (ret == 0) sentry_fd_untrack((int)R->a[0]);
        break;
    case 212: // recvmsg: track any SCM_RIGHTS fds received in the (sentry-written) control window
        if (ret >= 0 && coff) sentry_track_cmsg_fds(R->buf + coff, (size_t)*(uint64_t *)(R->buf + 40));
        break;
    default: break;
    }
    R->nserved++;
}

// One servicer thread per ring: spin for a request, service it, hand the ring back. The orphan-guard
// and the shared quit flag both _exit() the WHOLE sentry process (killing every servicer thread).
static void sentry_ring_loop(struct sentry_ring *R) {
    for (;;) {
        uint32_t spins = 0;
        while (atomic_load_explicit(&R->turn, memory_order_acquire) != 1) {
            if (atomic_load_explicit(&g_shm->quit, memory_order_acquire)) _exit(0);
            if (++spins > 256) {
                if (getppid() == 1) _exit(0); // orphan-guard: worker died/crashed -> don't spin forever
                sched_yield();
                spins = 0;
            }
        }
        sentry_service_one(R);
        atomic_store_explicit(&R->turn, 0, memory_order_release); // hand back to the worker
    }
}

static void *sentry_ring_thread(void *p) {
    sentry_ring_loop((struct sentry_ring *)p);
    return NULL; // unreachable (loop _exit()s)
}

// The sentry process body: ONE process (so all servicers share the host fd table) running N servicer
// threads -- one per ring. Spawns N-1 threads for ring[1..N-1] and services ring[0] on the main thread.
static void sentry_loop(void) {
    for (int i = 1; i < SENTRY_NRINGS; i++) {
        pthread_t th;
        if (pthread_create(&th, NULL, sentry_ring_thread, &g_shm->ring[i]) == 0)
            pthread_detach(th);
        // a failed servicer spawn just leaves ring[i] unserviced; its worker thread (if any ever claims
        // it) would block -- acceptable for the PoC pool, and never hit at <=i concurrent threads.
    }
    sentry_ring_loop(&g_shm->ring[0]); // main thread services ring 0; never returns
}

// ------------------------------------------------------------------ worker-side init / teardown
static void sentry_init(void) {
    g_shm = mmap(NULL, sizeof(struct sentry_shm), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (g_shm == MAP_FAILED) {
        perror("[sentry] ring mmap");
        _exit(71);
    }
    atomic_store_explicit(&g_shm->quit, 0, memory_order_relaxed);
    atomic_store_explicit(&g_shm->claim, 0, memory_order_relaxed);
    for (int i = 0; i < SENTRY_NRINGS; i++) {
        atomic_store_explicit(&g_shm->ring[i].turn, 0, memory_order_relaxed);
        atomic_store_explicit(&g_shm->ring[i].busy, 0, memory_order_relaxed);
        atomic_store_explicit(&g_shm->ring[i].owner, 0, memory_order_relaxed);
        // Per-ring control socketpair (SCM_RIGHTS fd-lend, item 3). Created BEFORE the fork so BOTH the
        // worker and the sentry inherit both ends at the same fd numbers; each is used point-to-point by
        // the single worker thread + single sentry servicer that own that lane. A failure leaves the lane
        // without fd-lend (only file-backed mmap needs it) -- mark it -1 so we never touch a stale fd.
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, g_ctl[i]) < 0) { g_ctl[i][0] = -1; g_ctl[i][1] = -1; }
    }
    g_worker_pid = getpid();
    g_sentry_owner_pid = getpid(); // only this process may signal-quit + reap the sentry
    g_guest_children = 0;
    pid_t pid = fork(); // sentry forks AFTER load -> inherits the fd table / jail config / auxv / cwd
    if (pid < 0) {
        perror("[sentry] fork");
        _exit(71);
    }
    if (pid == 0) {
        sentry_loop(); // child: spawns the per-ring servicer threads; never returns
        _exit(0);
    }
    g_sentry_pid = pid;
    if (g_sentry_sandbox) worker_sandbox(); // confine the worker (scoped; see k_worker_sbpl note)
}

static void sentry_shutdown(void) {
    if (!g_shm || !g_sentry_pid) return;
    // A forked-CHILD worker inherited g_sentry_pid but does NOT own the shared sentry: it must never set
    // quit (that would tear the sentry down under the still-live parent + sibling workers) or reap it.
    if (getpid() != g_sentry_owner_pid) { g_sentry_pid = 0; return; }
    atomic_store_explicit(&g_shm->quit, 1, memory_order_release);
    int st;
    waitpid(g_sentry_pid, &st, 0);
    uint64_t total = 0;
    for (int i = 0; i < SENTRY_NRINGS; i++) total += g_shm->ring[i].nserved;
    fprintf(stderr, "[sentry] forwarded %llu syscalls; sentry reaped\n", (unsigned long long)total);
    g_sentry_pid = 0; // idempotent: the exit-path shutdown + the post-run_guest teardown must not double-reap
}

// ------------------------------------------------------------------ the routed trust boundary
// Replaces the direct service_local(c) call for untrusted guests. When g_untrusted is off this is a
// transparent pass-through (trusted path byte-identical to baseline -- and service() already gated us
// out before getting here). When on, fs/net/proc syscalls are marshaled to the sentry over the ring;
// everything else stays local in the worker.
static void syscall_route(struct cpu *c) {
    if (!g_untrusted) {
        service_local(c);
        return;
    }
    // Normalize legacy x86 forms (open->openat, ...) in the worker so we classify by canonical number;
    // a return of 1 means it was fully handled locally (arch_prctl/TLS) -- it must stay here.
    if (G_NORMALIZE(c)) return;
    uint64_t nr = G_NR(c);

    // exit(93)/exit_group(94): service_local() _exit()s this worker. exit_group ends the PROCESS so the
    // owner reaps the sentry FIRST (sentry_shutdown is owner-gated -> a forked child just releases its
    // lane). exit(93) ends only THIS THREAD: free its lane but DON'T tear the sentry down (siblings need it).
    if (nr == 93 || nr == 94) {
        if (nr == 94) sentry_shutdown();
        ring_release();
        service_local(c);
        return;
    }

    // --- fork/exec/wait lane (item 1) -------------------------------------------------------------------
    // clone(220)/clone3(435): a guest THREAD is a host pthread (stays this process; gets its own lane
    // lazily). A guest FORK is a real worker fork() (the guest address space is worker-side COW memory the
    // sentry cannot duplicate) done LOCALLY by service_local. The freshly forked CHILD inherited the
    // parent's lane + sentry-ownership, so re-init its bookkeeping; the PARENT counts the new child so a
    // later wait4 with no real children doesn't deadlock on the hidden sentry child.
    if (nr == 220 || nr == 435) {
        int is_thread = (nr == 220) ? ((G_A0(c) & 0x10000) != 0)
                                    : (G_A0(c) ? ((*(uint64_t *)G_A0(c) & 0x10000) != 0) : 0);
        service_local(c); // spawn_thread (CLONE_THREAD) or fork() -- both worker-local
        if (getpid() != g_worker_pid) { sentry_fork_child(); return; } // we are the new child worker
        if (!is_thread && (int64_t)G_RET(c) > 0) atomic_fetch_add(&g_guest_children, 1); // parent: a child appeared
        return;
    }
    // execve(221) stays LOCAL: service_local reloads the guest image IN THIS PROCESS (it is not a host
    // execve), so the worker keeps its pid, ring lane, control sockets, sentry, and confinement across it.
    if (nr == 221) {
        service_local(c);
        return;
    }
    // wait4(260): reap the guest's child WORKER processes locally. The sentry is ALSO a child of the owner,
    // so a blocking wait-any with no GUEST children would hang on it -> short-circuit to -ECHILD; and never
    // surface the sentry's own pid to the guest. A specific-pid wait passes straight through.
    if (nr == 260) {
        int64_t wpid = (int64_t)(int)G_A0(c);
        if (wpid <= 0 && atomic_load(&g_guest_children) <= 0) { G_RET(c) = (uint64_t)(-ECHILD); return; }
        service_local(c);
        int64_t r = (int64_t)G_RET(c);
        if (r > 0) {
            if (g_sentry_pid && r == (int64_t)g_sentry_pid) { G_RET(c) = (uint64_t)(-ECHILD); return; }
            atomic_fetch_sub(&g_guest_children, 1);
        }
        return;
    }
    // file-backed mmap(222): the mapping must live in the WORKER (memory authority) but the fd is
    // sentry-owned and invalid here. Borrow the real fd over this lane's control socket (SCM_RIGHTS), map
    // it locally with the borrowed number, then drop it -- so the worker holds the real fd only for the
    // single mmap. Anonymous mmap (MAP_ANON 0x20) needs no fd and stays fully local below.
    if (nr == 222 && !(G_A3(c) & 0x20) && (int)G_A4(c) >= 0) {
        struct sentry_ring *R = ring_for_thread();
        while (atomic_exchange_explicit(&R->busy, 1, memory_order_acquire)) sched_yield();
        int idx = t_ring;
        R->rawnr = SENTRY_OP_FDPASS;
        R->a[0] = (uint64_t)(uint32_t)(int)G_A4(c);
        R->iovn = 0;
        for (int i = 0; i < 6; i++) R->redir[i] = -1;
        atomic_store_explicit(&R->turn, 1, memory_order_release);
        uint32_t sp = 0;
        while (atomic_load_explicit(&R->turn, memory_order_acquire) != 0)
            if (++sp > 256) { sched_yield(); sp = 0; }
        // The sentry ALWAYS sends a control datagram (with the fd, or empty on -EBADF), so we MUST always
        // receive it -- skipping on failure would leave a stale message to desync the next lend on this lane.
        int lfd = (idx >= 0 && g_ctl[idx][0] >= 0) ? sentry_recv_fd(g_ctl[idx][0]) : -1;
        atomic_store_explicit(&R->busy, 0, memory_order_release);
        uint64_t saved = G_A4(c);
        G_A4(c) = (uint64_t)(int64_t)lfd;       // -1 if the lend failed -> service_local mmap returns EBADF
        service_local(c);                        // real worker-side mmap on the borrowed fd
        G_A4(c) = saved;                         // restore the guest's r8/a4 (preserved across a syscall)
        if (lfd >= 0) close(lfd);                // drop the borrowed fd: worker fds stay virtual
        return;
    }

    if (!sentry_forwarded(nr)) {
        service_local(c); // LOCAL authority (its G_NORMALIZE re-runs as a no-op on already-*at registers)
        return;
    }

    struct sentry_ring *R = ring_for_thread(); // this worker thread's private ring (pool, keyed lazily)
    // Producer lock: at <=N concurrent worker threads each owns a distinct ring and this is an
    // uncontended single TAS; overflow threads (sharing a lane) serialize here, preserving the SPSC
    // ping-pong on the shared ring. Held across the whole round-trip + the output copy-back.
    while (atomic_exchange_explicit(&R->busy, 1, memory_order_acquire)) sched_yield();
    R->rawnr = G_RAWNR(c);
    R->a[0] = G_A0(c);
    R->a[1] = G_A1(c);
    R->a[2] = G_A2(c);
    R->a[3] = G_A3(c);
    R->a[4] = G_A4(c);
    R->a[5] = G_A5(c);
    for (int i = 0; i < 6; i++) R->redir[i] = -1;
    R->iovn = 0;
    R->inlen = 0;

    switch (nr) {
    case 56: { // openat(dfd, a1=path, ...): in-path
        const char *p = (const char *)G_A1(c);
        uint32_t n = 0;
        if (p)
            while (n < SENTRY_PATHCAP - 1 && p[n]) n++;
        if (p) memcpy(R->buf, p, n);
        R->buf[n] = 0;
        R->inlen = n + 1;
        R->redir[1] = 0;
        break;
    }
    case 64:   // write(fd, a1=buf, a2=len)
    case 68: { // pwrite64(fd, a1=buf, a2=len, a3=off): copy the payload into the ring; cap to BUFSZ
        uint32_t n = G_A2(c) > SENTRY_BUFSZ ? SENTRY_BUFSZ : (uint32_t)G_A2(c);
        if (n) memcpy(R->buf, (const void *)G_A1(c), n);
        R->inlen = n;
        R->redir[1] = 0;
        R->a[2] = n; // ship exactly n bytes; a short (p)write is legal -> guest loops
        break;
    }
    case 63:   // read(fd, a1=buf, a2=len)
    case 67:   // pread64(fd, a1=buf, a2=len, a3=off)
    case 61: { // getdents64(fd, a1=buf, a2=count): reserve the out window; cap to BUFSZ
        uint32_t n = G_A2(c) > SENTRY_BUFSZ ? SENTRY_BUFSZ : (uint32_t)G_A2(c);
        R->redir[1] = 0;
        R->a[2] = n; // short read / partial getdents is legal -> guest loops
        break;
    }
    case 80: // fstat(fd, a1=statbuf): out-struct only
        R->redir[1] = 0;
        break;
    case 79: { // newfstatat(dfd, a1=path, a2=statbuf, flags): in-path + out-struct (two-buffer)
        const char *p = (const char *)G_A1(c);
        uint32_t n = 0;
        if (p)
            while (n < SENTRY_PATHCAP - 1 && p[n]) n++;
        if (p) memcpy(R->buf, p, n);
        R->buf[n] = 0;
        R->inlen = n + 1;
        R->redir[1] = 0;            // path     -> buf[0]
        R->redir[2] = SENTRY_PATHCAP; // statbuf -> buf[SENTRY_PATHCAP]; copied back below on success
        break;
    }
    case 291: { // statx(dfd, a1=path, a2=flags, a3=mask, a4=statxbuf): in-path + out-struct
        const char *p = (const char *)G_A1(c);
        uint32_t n = 0;
        if (p)
            while (n < SENTRY_PATHCAP - 1 && p[n]) n++;
        if (p) memcpy(R->buf, p, n);
        R->buf[n] = 0;
        R->inlen = n + 1;
        R->redir[1] = 0;            // path      -> buf[0]
        R->redir[4] = SENTRY_PATHCAP; // statxbuf -> buf[SENTRY_PATHCAP]
        break;
    }
    case 65:   // readv(fd, a1=iov, a2=iovcnt)
    case 66: { // writev(fd, a1=iov, a2=iovcnt): flatten the guest iovec into the ring
        // Layout in buf[]: a `struct iovec[n]` header (iov_base = buf-relative OFFSET) followed by the
        // scatter/gather data. For writev we gather the guest segments now; for readv we just reserve
        // the windows and scatter back after the round-trip. iov_base offsets are bounds-checked and
        // rebased to ring pointers by the sentry, so no guest pointer ever crosses.
        const struct iovec *giov = (const struct iovec *)G_A1(c);
        uint32_t n = (uint32_t)G_A2(c);
        if (n > SENTRY_IOVMAX) n = SENTRY_IOVMAX; // partial scatter/gather is legal -> guest loops
        if (!giov) n = 0;                         // malformed (NULL iov, cnt>0): forward an empty vector
        struct iovec *biov = (struct iovec *)R->buf;
        uint32_t cur = n * (uint32_t)sizeof(struct iovec); // data region starts after the iovec header
        for (uint32_t i = 0; i < n; i++) {
            uint32_t room = SENTRY_BUFSZ - cur;
            uint32_t want = (giov && giov[i].iov_len < room) ? (uint32_t)giov[i].iov_len : room;
            if (nr == 66 && want && giov) memcpy(R->buf + cur, giov[i].iov_base, want); // gather (writev)
            biov[i].iov_base = (void *)(uintptr_t)cur; // buf-relative offset; sentry rebases + checks
            biov[i].iov_len = want;
            cur += want;
        }
        R->inlen = cur;
        R->redir[1] = 0;
        R->iovn = n;
        R->a[2] = n; // sentry runs the (possibly clamped) segment count
        break;
    }
    // ---- socket family ---- (sentry owns the real socket fd; only sockaddr/optval/data bytes cross,
    // never a guest pointer; all AF/port-map/jail translation runs inside service_local on the sentry)
    case 200:   // bind(fd, a1=addr, a2=addrlen)
    case 203: { // connect(fd, a1=addr, a2=addrlen): in-sockaddr -> tail window
        const uint8_t *sa = (const uint8_t *)G_A1(c);
        if (sa) {
            uint32_t n = (uint32_t)G_A2(c);
            if (n > SENTRY_SADDRCAP) n = SENTRY_SADDRCAP; // real sockaddrs are <=128; cap defensively
            memcpy(R->buf + SENTRY_SADDR_OFF, sa, n);
            R->redir[1] = SENTRY_SADDR_OFF;
            R->a[2] = n;
            R->inlen = n;
        } // NULL addr: leave register/len as-is (service_local handles it / errors identically)
        break;
    }
    case 202:   // accept(fd, a1=addr_out, a2=addrlen_inout)
    case 242:   // accept4(fd, a1=addr_out, a2=addrlen_inout, a3=flags)
    case 204:   // getsockname(fd, a1=addr_out, a2=addrlen_inout)
    case 205: { // getpeername(fd, a1=addr_out, a2=addrlen_inout): out-sockaddr + in/out socklen
        if (G_A1(c)) R->redir[1] = SENTRY_SADDR_OFF;                 // out sockaddr -> tail window
        if (G_A2(c)) {                                              // in/out socklen: ship the guest cap
            *(socklen_t *)(R->buf + SENTRY_SLEN_OFF) = *(socklen_t *)G_A2(c);
            R->redir[2] = SENTRY_SLEN_OFF;
        }
        break;
    }
    case 206: { // sendto(fd, a1=buf, a2=len, a3=flags, a4=destaddr, a5=addrlen): in-data + in-destaddr
        uint32_t n = G_A2(c) > SENTRY_DATACAP ? SENTRY_DATACAP : (uint32_t)G_A2(c);
        if (n) memcpy(R->buf, (const void *)G_A1(c), n);
        R->redir[1] = 0;
        R->a[2] = n; // short send is legal -> guest loops
        R->inlen = n;
        if (G_A4(c)) { // optional dest addr (UDP) -> tail window
            uint32_t dl = (uint32_t)G_A5(c);
            if (dl > SENTRY_SADDRCAP) dl = SENTRY_SADDRCAP;
            memcpy(R->buf + SENTRY_SADDR_OFF, (const void *)G_A4(c), dl);
            R->redir[4] = SENTRY_SADDR_OFF;
            R->a[5] = dl;
        }
        break;
    }
    case 207: { // recvfrom(fd, a1=buf, a2=len, a3=flags, a4=srcaddr_out, a5=addrlen_inout)
        uint32_t n = G_A2(c) > SENTRY_DATACAP ? SENTRY_DATACAP : (uint32_t)G_A2(c);
        R->redir[1] = 0;
        R->a[2] = n; // short recv is legal -> guest loops
        if (G_A4(c)) R->redir[4] = SENTRY_SADDR_OFF;                 // out src sockaddr -> tail window
        if (G_A5(c)) {                                              // in/out socklen: ship the guest cap
            *(socklen_t *)(R->buf + SENTRY_SLEN_OFF) = *(socklen_t *)G_A5(c);
            R->redir[5] = SENTRY_SLEN_OFF;
        }
        break;
    }
    case 208: { // setsockopt(fd, a1=level, a2=optname, a3=optval, a4=optlen): in-optval -> opt window
        if (G_A3(c)) {
            uint32_t n = (uint32_t)G_A4(c);
            if (n > SENTRY_OPTCAP) n = SENTRY_OPTCAP;
            if (n) memcpy(R->buf + SENTRY_OPT_OFF, (const void *)G_A3(c), n);
            R->redir[3] = SENTRY_OPT_OFF;
            R->a[4] = n;
            R->inlen = n;
        }
        break;
    }
    case 209: { // getsockopt(fd, a1=level, a2=optname, a3=optval_out, a4=optlen_inout)
        if (G_A4(c)) { // in/out optlen: ship the guest cap (clamped so the kernel can't overrun the window)
            socklen_t cap = *(socklen_t *)G_A4(c);
            if (cap > SENTRY_OPTCAP) cap = SENTRY_OPTCAP;
            *(socklen_t *)(R->buf + SENTRY_SLEN_OFF) = cap;
            R->redir[4] = SENTRY_SLEN_OFF;
        }
        if (G_A3(c)) R->redir[3] = SENTRY_OPT_OFF;                   // out optval -> opt window
        break;
    }
    // ---- sendmsg/recvmsg (item 2): flatten the guest msghdr GRAPH into the ring ----
    case 211:   // sendmsg(fd, a1=msghdr, flags)
    case 212: { // recvmsg(fd, a1=msghdr, flags)
        uint8_t *g = (uint8_t *)G_A1(c);
        if (!g) { memset(R->buf, 0, SENTRY_MSGHDR_SZ); R->redir[1] = 0; break; } // NULL msghdr: ship a clean
                                                                                 // zero hdr so the sentry never
                                                                                 // derefs NULL (no crash)
        uint64_t g_name = *(uint64_t *)(g + 0);
        uint32_t g_namelen = *(uint32_t *)(g + 8);
        uint64_t g_iov = *(uint64_t *)(g + 16), g_iovlen = *(uint64_t *)(g + 24);
        uint64_t g_ctl = *(uint64_t *)(g + 32), g_ctllen = *(uint64_t *)(g + 40);
        uint32_t g_flags = *(uint32_t *)(g + 48);
        uint8_t *h = R->buf; // the 56-byte msghdr COPY at [0,56)
        memset(h, 0, SENTRY_MSGHDR_SZ);
        // msg_name: offset into the sockaddr tail window (send copies the addr; recv just reserves it).
        if (g_name && g_namelen) {
            uint32_t nl = g_namelen > SENTRY_SADDRCAP ? SENTRY_SADDRCAP : g_namelen;
            if (nr == 211) memcpy(R->buf + SENTRY_MSGNAME_OFF, (const void *)g_name, nl);
            *(uint64_t *)(h + 0) = SENTRY_MSGNAME_OFF;               // nonzero offset == present
            *(uint32_t *)(h + 8) = nl;                              // capped to the ring window (real addrs fit)
        }
        // msg_iov: iovec[] header (iov_base = OFFSET) + data, flattened like readv/writev, capped to DATACAP.
        const struct iovec *giov = (const struct iovec *)g_iov;
        uint32_t n = g_iovlen > SENTRY_IOVMAX ? SENTRY_IOVMAX : (uint32_t)g_iovlen;
        if (!giov) n = 0;
        struct iovec *biov = (struct iovec *)(R->buf + SENTRY_MSGIOV_OFF);
        uint32_t cur = SENTRY_MSGIOV_OFF + n * (uint32_t)sizeof(struct iovec);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t room = (cur < SENTRY_DATACAP) ? (SENTRY_DATACAP - cur) : 0; // keep data clear of the tail
            uint32_t want = (giov && giov[i].iov_len < room) ? (uint32_t)giov[i].iov_len : room;
            if (nr == 211 && want && giov) memcpy(R->buf + cur, giov[i].iov_base, want); // gather (send)
            biov[i].iov_base = (void *)(uintptr_t)cur;
            biov[i].iov_len = want;
            cur += want;
        }
        *(uint64_t *)(h + 16) = SENTRY_MSGIOV_OFF;
        *(uint64_t *)(h + 24) = n;
        // msg_control: offset into the optval tail window (send copies the cmsg; recv reserves it). SCM_RIGHTS
        // fds inside are sentry fds, so the bytes cross verbatim.
        if (g_ctl && g_ctllen) {
            uint32_t cl = g_ctllen > SENTRY_MSGCTLCAP ? SENTRY_MSGCTLCAP : (uint32_t)g_ctllen;
            if (nr == 211) memcpy(R->buf + SENTRY_MSGCTL_OFF, (const void *)g_ctl, cl);
            *(uint64_t *)(h + 32) = SENTRY_MSGCTL_OFF;               // nonzero offset == present
            *(uint64_t *)(h + 40) = cl;                             // controllen (send: actual; recv: cap)
        }
        *(uint32_t *)(h + 48) = g_flags;
        R->redir[1] = 0; // a1 -> msghdr copy; the sentry rebases the inner offsets (snr 211/212)
        R->inlen = cur;
        break;
    }
    // ---- multiplexing over sentry-owned fds (item 3) ----
    case 73: { // ppoll(fds, nfds, timeout_ts, sigmask, sigsetsz)
        uint32_t nfds = (uint32_t)G_A1(c);
        uint32_t bytes = nfds * 8u;                                  // sizeof(struct pollfd) == 8
        if (bytes > SENTRY_DATACAP) { bytes = SENTRY_DATACAP; nfds = bytes / 8u; }
        if (G_A0(c) && bytes) memcpy(R->buf, (const void *)G_A0(c), bytes);
        R->redir[0] = 0;
        R->a[1] = nfds;
        if (G_A2(c)) { memcpy(R->buf + SENTRY_POLL_TMO, (const void *)G_A2(c), 16); R->redir[2] = SENTRY_POLL_TMO; }
        else R->a[2] = 0;                                            // NULL timeout == block forever
        R->a[3] = 0; R->a[4] = 0;                                    // sigmask ignored by service_local
        break;
    }
    case 72: { // pselect6(nfds, rd, wr, ex, timeout_ts, sigmask)
        uint32_t nfds = (uint32_t)G_A0(c);
        uint32_t fb = (nfds + 7u) / 8u;
        if (fb > 128u) fb = 128u;                                    // fd_set caps at FD_SETSIZE/8 == 128
        if (G_A1(c)) { memcpy(R->buf + SENTRY_PSEL_RD, (const void *)G_A1(c), fb); R->redir[1] = SENTRY_PSEL_RD; } else R->a[1] = 0;
        if (G_A2(c)) { memcpy(R->buf + SENTRY_PSEL_WR, (const void *)G_A2(c), fb); R->redir[2] = SENTRY_PSEL_WR; } else R->a[2] = 0;
        if (G_A3(c)) { memcpy(R->buf + SENTRY_PSEL_EX, (const void *)G_A3(c), fb); R->redir[3] = SENTRY_PSEL_EX; } else R->a[3] = 0;
        if (G_A4(c)) { memcpy(R->buf + SENTRY_PSEL_TMO, (const void *)G_A4(c), 16); R->redir[4] = SENTRY_PSEL_TMO; } else R->a[4] = 0;
        R->a[5] = 0;
        break;
    }
    case 21: // epoll_ctl(epfd, op, fd, a3=event): in epoll_event (12B)
        if (G_A3(c)) { memcpy(R->buf, (const void *)G_A3(c), SENTRY_EPEV_SZ); R->redir[3] = 0; }
        break;
    case 22: // epoll_pwait(epfd, a1=events_out, maxevents, timeout, sigmask): reserve out window, drop sigmask
        R->redir[1] = 0;
        R->a[4] = 0;
        break;
    // ---- fd-table ops on sentry-owned fds (item 3) ----
    case 25: // fcntl(fd, cmd, arg): only F_GETLK/SETLK/SETLKW (5/6/7) carry a flock* in a2. Always redir a2
             // to the ring for those (so the sentry's flock deref can never hit a guest/NULL pointer); copy
             // the inbound lock only if the guest pointer is real.
        if ((int)G_A1(c) >= 5 && (int)G_A1(c) <= 7) {
            if (G_A2(c)) memcpy(R->buf, (const void *)G_A2(c), SENTRY_FLOCKSZ);
            R->redir[2] = 0;
        }
        break;
    case 29: { // ioctl(fd, req, arg): always redir arg to the ring so the sentry never derefs a guest/NULL
               // pointer; copy in exactly the _IOC_SIZE/table byte count (unsized/unknown -> nothing -> ENOTTY)
        if (G_A2(c)) {
            uint32_t isz, osz;
            sentry_ioctl_sizes((unsigned long)G_A1(c), &isz, &osz);
            if (isz > SENTRY_IOCTLCAP) isz = SENTRY_IOCTLCAP;
            if (isz) memcpy(R->buf, (const void *)G_A2(c), isz);
        }
        R->redir[2] = 0;
        break;
    }
    case 59:  // pipe2(a0=int[2], flags): out fd pair
        R->redir[0] = 0;
        break;
    case 199: // socketpair(domain, type, proto, a3=int[2]): out fd pair
        R->redir[3] = 0;
        break;
    default: break; // 57 close / 62 lseek / 198 socket / 201 listen / 210 shutdown / 20 epoll_create1 /
                    // 23 dup / 24 dup3: no buffer.
    }

    // ---- ring round-trip ----
    atomic_store_explicit(&R->turn, 1, memory_order_release); // publish request -> sentry
    uint32_t spins = 0;
    while (atomic_load_explicit(&R->turn, memory_order_acquire) != 0) { // await response
        if (++spins > 256) {
            sched_yield();
            spins = 0;
        }
    }

    // ---- copy outputs back into guest memory (guest pointers only ever touched here, on the worker) ----
    int64_t ret = R->ret;
    switch (nr) {
    case 63:   // read
    case 67:   // pread64
    case 61:   // getdents64: the sentry landed ret bytes at buf[0]
        if (ret > 0) {
            uint32_t n = (uint32_t)ret;
            if (n > (uint32_t)R->a[2]) n = (uint32_t)R->a[2]; // never exceed the window we shipped
            memcpy((void *)G_A1(c), R->buf, n);
        }
        break;
    case 80: // fstat: struct landed at buf[0]
        if (ret == 0) memcpy((void *)G_A1(c), R->buf, SENTRY_STATSZ);
        break;
    case 79: // newfstatat: struct landed at buf[SENTRY_PATHCAP]
        if (ret == 0) memcpy((void *)G_A2(c), R->buf + SENTRY_PATHCAP, SENTRY_STATSZ);
        break;
    case 291: // statx: struct landed at buf[SENTRY_PATHCAP]
        if (ret == 0) memcpy((void *)G_A4(c), R->buf + SENTRY_PATHCAP, SENTRY_STATXSZ);
        break;
    case 65: // readv: scatter the ret bytes the sentry fetched back into the guest iovecs
        if (ret > 0) {
            const struct iovec *giov = (const struct iovec *)G_A1(c);
            const struct iovec *biov = (const struct iovec *)R->buf;
            uint32_t n = (uint32_t)R->a[2], remaining = (uint32_t)ret;
            for (uint32_t i = 0; i < n && remaining; i++) {
                uint32_t seg = (uint32_t)biov[i].iov_len; // window length the sentry scattered into
                if (seg > remaining) seg = remaining;
                // the sentry rebased iov_base to a pointer into buf[] (shared at the same VA -> usable here)
                memcpy(giov[i].iov_base, biov[i].iov_base, seg);
                remaining -= seg;
            }
        }
        break;
    // ---- socket family: scatter the out-sockaddr / its length / out-optval / recv data back ----
    case 202: // accept
    case 242: // accept4
    case 204: // getsockname
    case 205: // getpeername: sentry wrote the translated sockaddr to the tail window + the length to SLEN
        // accept/accept4 succeed with ret>=0 (the new fd); getsockname/getpeername with ret==0.
        if (ret >= 0 && G_A2(c)) {
            socklen_t outlen = *(socklen_t *)(R->buf + SENTRY_SLEN_OFF); // length service_local reported
            socklen_t gcap = *(socklen_t *)G_A2(c);                      // guest buffer capacity
            *(socklen_t *)G_A2(c) = outlen;                              // return the (possibly larger) length
            if (G_A1(c)) {
                socklen_t cpy = outlen < gcap ? outlen : gcap;          // truncate to the guest buffer
                if (cpy > SENTRY_SADDRCAP) cpy = SENTRY_SADDRCAP;
                memcpy((void *)G_A1(c), R->buf + SENTRY_SADDR_OFF, cpy);
            }
        }
        break;
    case 207: // recvfrom: recv data landed at buf[0]; src sockaddr + its length in the tail windows
        if (ret > 0) {
            uint32_t n = (uint32_t)ret;
            if (n > (uint32_t)R->a[2]) n = (uint32_t)R->a[2]; // never exceed the window we shipped
            memcpy((void *)G_A1(c), R->buf, n);
        }
        if (ret >= 0 && G_A5(c)) {
            socklen_t outlen = *(socklen_t *)(R->buf + SENTRY_SLEN_OFF);
            socklen_t gcap = *(socklen_t *)G_A5(c);
            *(socklen_t *)G_A5(c) = outlen;
            if (G_A4(c)) {
                socklen_t cpy = outlen < gcap ? outlen : gcap;
                if (cpy > SENTRY_SADDRCAP) cpy = SENTRY_SADDRCAP;
                memcpy((void *)G_A4(c), R->buf + SENTRY_SADDR_OFF, cpy);
            }
        }
        break;
    case 209: // getsockopt: optval landed at the opt window; its length at SLEN
        if (ret == 0 && G_A4(c)) {
            socklen_t outlen = *(socklen_t *)(R->buf + SENTRY_SLEN_OFF);
            socklen_t gcap = *(socklen_t *)G_A4(c);
            socklen_t eff = gcap < SENTRY_OPTCAP ? gcap : SENTRY_OPTCAP; // we shipped at most OPTCAP
            *(socklen_t *)G_A4(c) = outlen;
            if (G_A3(c)) {
                socklen_t cpy = outlen < eff ? outlen : eff;
                memcpy((void *)G_A3(c), R->buf + SENTRY_OPT_OFF, cpy);
            }
        }
        break;
    // ---- recvmsg (item 2): scatter received data + write back name/control/flags into the guest msghdr ----
    case 212:
        if (ret >= 0 && G_A1(c)) {
            uint8_t *g = (uint8_t *)G_A1(c);                          // the original guest msghdr
            uint8_t *h = R->buf;                                      // the ring msghdr copy service_local filled
            uint64_t g_name = *(uint64_t *)(g + 0);
            uint32_t g_namecap = *(uint32_t *)(g + 8);               // guest-supplied name capacity (unmodified yet)
            uint32_t outnl = *(uint32_t *)(h + 8);                   // length the sentry reported
            if (g_name && g_namecap) {
                uint32_t cpy = outnl < g_namecap ? outnl : g_namecap;
                if (cpy > SENTRY_SADDRCAP) cpy = SENTRY_SADDRCAP;
                memcpy((void *)g_name, R->buf + SENTRY_MSGNAME_OFF, cpy);
            }
            *(uint32_t *)(g + 8) = outnl;                            // updated msg_namelen
            uint64_t g_ctl = *(uint64_t *)(g + 32), g_ctlcap = *(uint64_t *)(g + 40);
            uint64_t outcl = *(uint64_t *)(h + 40);                  // control length the sentry wrote
            if (g_ctl && g_ctlcap) {
                uint64_t cpy = outcl < g_ctlcap ? outcl : g_ctlcap;
                if (cpy > SENTRY_MSGCTLCAP) cpy = SENTRY_MSGCTLCAP;
                memcpy((void *)g_ctl, R->buf + SENTRY_MSGCTL_OFF, cpy); // SCM_RIGHTS fds here are sentry fds == guest fds
            }
            *(uint64_t *)(g + 40) = outcl;                           // updated msg_controllen
            *(uint32_t *)(g + 48) = *(uint32_t *)(h + 48);           // updated msg_flags (MSG_TRUNC/CTRUNC/...)
            // scatter the ret payload bytes back into the guest's iovec segments
            if (ret > 0) {
                const struct iovec *giov = (const struct iovec *)*(uint64_t *)(g + 16);
                const struct iovec *biov = (const struct iovec *)(R->buf + SENTRY_MSGIOV_OFF);
                uint32_t n = (uint32_t)*(uint64_t *)(h + 24), remaining = (uint32_t)ret;
                for (uint32_t i = 0; i < n && remaining && giov; i++) {
                    uint32_t seg = (uint32_t)biov[i].iov_len;
                    if (seg > remaining) seg = remaining;
                    memcpy(giov[i].iov_base, biov[i].iov_base, seg); // biov[i].iov_base rebased to ring VA (shared)
                    remaining -= seg;
                }
            }
        }
        break;
    // ---- multiplexing copy-back (item 3) ----
    case 73: // ppoll: revents updated in place in the ring pollfd array
        if (ret >= 0 && G_A0(c)) memcpy((void *)G_A0(c), R->buf, (uint32_t)R->a[1] * 8u);
        break;
    case 72: // pselect6: the three fd_sets were narrowed in place
        if (ret >= 0) {
            uint32_t fb = ((uint32_t)G_A0(c) + 7u) / 8u;
            if (fb > 128u) fb = 128u;
            if (G_A1(c)) memcpy((void *)G_A1(c), R->buf + SENTRY_PSEL_RD, fb);
            if (G_A2(c)) memcpy((void *)G_A2(c), R->buf + SENTRY_PSEL_WR, fb);
            if (G_A3(c)) memcpy((void *)G_A3(c), R->buf + SENTRY_PSEL_EX, fb);
        }
        break;
    case 22: // epoll_pwait: ret ready events (12B each) landed at buf[0]
        if (ret > 0 && G_A1(c)) {
            uint32_t mx = (uint32_t)G_A2(c);
            uint32_t got = (uint32_t)ret < mx ? (uint32_t)ret : mx;
            memcpy((void *)G_A1(c), R->buf, got * SENTRY_EPEV_SZ);
        }
        break;
    case 25: // fcntl F_GETLK: the conflicting lock was written back into the ring flock
        if ((int)G_A1(c) == 5 && ret >= 0 && G_A2(c)) memcpy((void *)G_A2(c), R->buf, SENTRY_FLOCKSZ);
        break;
    case 29: // ioctl: write back exactly the out bytes the request defines (never clobber past them)
        if (ret >= 0 && G_A2(c)) {
            uint32_t isz, osz;
            sentry_ioctl_sizes((unsigned long)G_A1(c), &isz, &osz);
            if (osz > SENTRY_IOCTLCAP) osz = SENTRY_IOCTLCAP;
            if (osz) memcpy((void *)G_A2(c), R->buf, osz);
        }
        break;
    case 59:  // pipe2: out fd pair (both ends sentry fds, virtual to the guest)
        if (ret == 0 && G_A0(c)) memcpy((void *)G_A0(c), R->buf, 8);
        break;
    case 199: // socketpair: out fd pair
        if (ret == 0 && G_A3(c)) memcpy((void *)G_A3(c), R->buf, 8);
        break;
    default: break; // 56 openat / 57 close / 62 lseek / 64 write / 66 writev / 68 pwrite / 198 socket /
                    // 200 bind / 203 connect / 206 sendto / 208 setsockopt / 210 shutdown / 211 sendmsg /
                    // 20 epoll_create1 / 21 epoll_ctl / 23 dup / 24 dup3: no out bytes
    }
    G_RET(c) = (uint64_t)ret;
    atomic_store_explicit(&R->busy, 0, memory_order_release); // release the producer lock (round-trip done)
}

// ------------------------------------------------------------------ NEXT sentry PR (roadmap)
// 1. Ring pool + fork/exec/wait lane -- DONE: SENTRY_NRINGS rings, RECLAIM-AWARE free-list (ring_for_thread
//    CAS-claims a free lane by per-thread token; ring_release frees it on thread/process exit), serviced by
//    N sentry THREADS in the one sentry process (shared host fd table). FORK: a guest clone/clone3 forks the
//    WORKER (the guest address space is worker-side COW the sentry cannot duplicate); the child adopts a
//    fresh identity (sentry_fork_child: drops the inherited lane, mints a new token, is NOT the sentry owner)
//    so it draws its own lane and never tears the shared sentry down. EXECVE stays local (it reloads the
//    image in-process, keeping ring/sentry/confinement). WAIT4 reaps child WORKERS, short-circuits a
//    wait-any with no guest children to -ECHILD (so it can't block on the hidden sentry child) and never
//    surfaces the sentry's pid. exit_group reap is OWNER-GATED.  FOLLOW-UP: PER-PROCESS sentry fd tables --
//    forked workers currently SHARE the sentry's fd table (correct for the fork+exec inherit pattern that
//    sh/popen use, but two long-lived post-fork processes that independently mutate fds would alias).
// 2. fs/net/proc forwarded set -- DONE this PR: sendmsg/recvmsg (211/212) with the full nested msghdr graph
//    (msg_name + scatter/gather msg_iov + msg_control flattened to the ring, rebased by the sentry) and
//    SCM_RIGHTS that "just works" -- a guest fd in the cmsg is ALREADY a sentry fd, so the control bytes
//    cross verbatim and a recvmsg lands a usable virtual fd. Prior PRs: socket lifecycle + two-buffer stat
//    family + iovec readv/writev + getdents64 + pread/pwrite.
// 3. Multiplexing + fd passing -- DONE this PR: ppoll/pselect6/epoll_create1/epoll_ctl/epoll_pwait forwarded
//    (they block on a sentry-owned fd, so they MUST run in the sentry; pollfd/fd_set/epoll_event marshaled
//    in+out); fd-table ops dup/dup3/pipe2/socketpair/fcntl(F_SETFL,F_DUPFD,F_GETLK flock)/ioctl(FIONBIO/
//    FIONREAD/winsize/termios, exact-size in/out) forwarded so the guest fd space stays entirely sentry-side.
//    SCM_RIGHTS worker<->sentry fd LEND: a sentry-owned fd a LOCAL worker syscall must touch (file-backed
//    mmap) is sent over a per-ring AF_UNIX control socketpair, mapped locally, then dropped.  FOLLOW-UP for
//    full soundness: forward eventfd2/timerfd/signalfd4/inotify (today they make WORKER-local fds that a
//    forwarded read/epoll_ctl then can't see); select(non-pselect)/epoll_pwait2; sendmmsg/recvmmsg (243/269).
// 4. Futex/__ulock wakeup -- STILL A SPIN (perf only, not correctness): N idle servicer threads + the worker
//    busy-wait `turn`; a process-shared futex/os_sync wake would drop idle CPU. Deferred.
// 5. Sentry-side policy: add an allow/deny layer (path allowlists, net egress) so the sentry ENFORCES
//    rather than merely executes.
