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
    _Atomic uint32_t turn; // 0 = worker owns (build request), 1 = sentry owns (execute)
    _Atomic uint32_t busy; // worker-side producer lock: held across one round-trip (uncontended at <=N threads)
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
static __thread int t_ring = -1; // this worker thread's claimed ring index (claimed lazily on first use)

// Claim (once per worker thread) a ring from the pool. <=N threads => each gets a private lane; beyond
// N, threads share a lane and are serialized by the ring's `busy` producer lock (correctness preserved).
static struct sentry_ring *ring_for_thread(void) {
    if (t_ring < 0)
        t_ring = (int)(atomic_fetch_add_explicit(&g_shm->claim, 1, memory_order_relaxed) % SENTRY_NRINGS);
    return &g_shm->ring[t_ring];
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
        return 1;
    // --- NEXT (need fd-passing / process marshaling; see roadmap at bottom) ---
    // 221 execve, 220 clone(fork), 260 wait4, 203/211 sendmsg/recvmsg (SCM_RIGHTS), ...
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
    "(allow mach-lookup)\n"
    "(allow mach-priv-task-port)\n"
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
        // Production would fail CLOSED (_exit) here; in this first PR we warn and continue so the
        // ring path can be A/B'd even where the profile needs tuning for the host OS revision.
    } else {
        fprintf(stderr, "[sentry] worker confined under deny-default Seatbelt profile\n");
    }
#endif
}

// ------------------------------------------------------------------ sentry process body
// Holds host authority. Services ONE marshaled request on ring R: rebuilds a cpu from the marshaled
// registers, redirects each flagged guest-buffer pointer arg into the shared ring (so service_local()
// never touches worker/guest memory) -- including rebasing the flattened readv/writev iovec offsets to
// ring pointers -- and runs the REAL service_local() -- identical jail/proc/overlay policy, identical
// bytes. NOTE: it MUST call service_local() (the canonical switch), not service() -- service() would
// re-enter syscall_route() in this (g_untrusted) process and recurse onto the ring.
static void sentry_service_one(struct sentry_ring *R) {
    struct cpu tmp;
    memset(&tmp, 0, sizeof tmp);
    G_RAWNR(&tmp) = R->rawnr; // service_local() re-runs G_NORMALIZE on this as a no-op (already *at)
    G_A0(&tmp) = R->a[0];
    G_A1(&tmp) = R->a[1];
    G_A2(&tmp) = R->a[2];
    G_A3(&tmp) = R->a[3];
    G_A4(&tmp) = R->a[4];
    G_A5(&tmp) = R->a[5];
    // Redirect each flagged pointer arg into the ring buffer (THE crossing point: from here on
    // service_local() touches only ring memory). Bounds-check every worker-supplied offset first --
    // an out-of-range offset is a hijacked/buggy worker and faults the call rather than the sentry.
    int bad = 0;
    uint64_t *ta[6] = {&G_A0(&tmp), &G_A1(&tmp), &G_A2(&tmp), &G_A3(&tmp), &G_A4(&tmp), &G_A5(&tmp)};
    for (int i = 0; i < 6; i++) {
        if (R->redir[i] < 0) continue;
        uint32_t off = (uint32_t)R->redir[i];
        if (off >= SENTRY_BUFSZ) { bad = 1; break; }
        *ta[i] = (uint64_t)(R->buf + off);
    }
    if (!bad && R->iovn) { // rebase + bounds-check the flattened iovec at a1 (offsets -> ring ptrs)
        struct iovec *iv = (struct iovec *)(R->buf + (uint32_t)R->redir[1]);
        for (uint32_t k = 0; k < R->iovn; k++) {
            uint64_t boff = (uint64_t)(uintptr_t)iv[k].iov_base, len = iv[k].iov_len;
            if (boff > SENTRY_BUFSZ || len > SENTRY_BUFSZ || boff + len > SENTRY_BUFSZ) {
                iv[k].iov_base = R->buf; // clamp a bad segment to empty rather than escape the ring
                iv[k].iov_len = 0;
            } else
                iv[k].iov_base = R->buf + boff;
        }
    }
    if (bad) {
        R->ret = -EFAULT;
    } else {
        service_local(&tmp); // real host authority + container policy
        R->ret = (int64_t)G_RET(&tmp);
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
    }
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
    if (nr == 93 || nr == 94) { // exit / exit_group: service_local() _exit()s the worker -> reap sentry FIRST
        sentry_shutdown();
        service_local(c);
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
    default: break; // 57 close / 62 lseek / 198 socket / 201 listen / 210 shutdown: no buffer.
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
    default: break; // 56 openat / 57 close / 62 lseek / 64 write / 66 writev / 68 pwrite /
                    // 198 socket / 200 bind / 203 connect / 206 sendto / 208 setsockopt / 210 shutdown: no out bytes
    }
    G_RET(c) = (uint64_t)ret;
    atomic_store_explicit(&R->busy, 0, memory_order_release); // release the producer lock (round-trip done)
}

// ------------------------------------------------------------------ NEXT sentry PR (roadmap)
// 1. Per-context rings -- DONE this PR (PoC pool): SENTRY_NRINGS independent rings, one claimed per
//    worker thread (lazy, monotonic counter mod N), serviced by N sentry THREADS in the one sentry
//    process (one process so the host fd table is shared across servicers). At <=N concurrent worker
//    threads every thread owns a private lane; beyond N, overflow threads share a lane behind the
//    per-ring `busy` producer lock. FOLLOW-UP for a full pool: size/grow the pool or hash by tid so
//    overflow never serializes; key by pid for forked-PROCESS workers (claim counter is already in
//    shared memory so forked workers draw distinct lanes, but the sentry does not yet TRACK a forked
//    child's lifetime -- that lands with execve/clone(fork) below).
// 2. Complete the fs/net/proc forwarded set. DONE this PR: the socket lifecycle -- socket/bind/listen/
//    connect/accept/accept4/getsockname/getpeername/sendto/recvfrom/setsockopt/getsockopt/shutdown
//    (sockaddr in/out + in/out socklen, optval in/out, send/recv data over the tail windows; the
//    sentry owns the real socket fd, the worker holds only the virtual fd number). Plus the prior fs
//    set (two-buffer stat family, iovec readv/writev, getdents64, pread64/pwrite64). STILL LOCAL /
//    NEXT: (a) sendmsg/recvmsg (211/212) -- nested msghdr/iovec + SCM_RIGHTS (item 3); (b) the
//    MULTIPLEXING family over a forwarded socket fd -- poll/ppoll/select/epoll_wait and the
//    nonblocking fcntl(F_SETFL)/ioctl(FIONBIO) -- these block/poll a sentry-owned fd, so they need
//    either fd-passing (item 3) or forwarding the whole poll set; (c) execve/clone(fork)/wait4
//    (process creation -- the sentry must fork+register a new worker lane and proxy the child fd table).
// 3. SCM_RIGHTS fd passing: for a guest fd a LOCAL worker syscall must touch (file-backed mmap), the
//    sentry sendmsg()es the fd to the worker over a control socketpair; the worker's fds stay virtual.
//    Also needed for AF_UNIX SCM_RIGHTS payloads inside guest sendmsg/recvmsg.
// 4. Futex/__ulock wakeup: replace the per-ring spin with a process-shared futex on `turn` to drop
//    idle CPU (N idle servicer threads now spin -- a process-shared futex wake matters more with N).
// 5. Sentry-side policy: add an allow/deny layer (path allowlists, net egress) so the sentry ENFORCES
//    rather than merely executes.
