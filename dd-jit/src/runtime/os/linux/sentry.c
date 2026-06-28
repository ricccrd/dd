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
// They communicate over a shared-memory SPSC mailbox (a 1-deep ring; guest syscalls are synchronous
// so depth 1 is what gets exercised -- see the per-thread/multi-slot extension notes at the bottom).
// The worker marshals {normalized syscall registers, inline buffer} into the ring; the sentry
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

// The shared-memory mailbox. `turn` is the ownership token: 0 => worker fills a request, 1 => sentry
// executes it. Strict ping-pong (no third state) => deadlock-free and, with release/acquire on turn,
// torn-message-free (all field writes happen-before the token flip the peer acquires).
struct sentry_ring {
    _Atomic uint32_t turn; // 0 = worker owns (build request), 1 = sentry owns (execute)
    _Atomic uint32_t quit; // worker sets at teardown -> sentry _exit()s
    // request: the post-normalize syscall registers (frontend-agnostic via G_RAWNR / G_A0..G_A5)
    uint64_t rawnr;  // raw syscall-number register (so the sentry's G_NR re-derives the canonical nr)
    uint64_t a[6];   // a0..a5 (G_A0..G_A5)
    int32_t bufarg;  // which arg index (1 = a1) was redirected into buf[], or -1 if none
    uint32_t inlen;  // valid input bytes in buf[] (write payload / NUL-terminated path)
    uint32_t outcap; // max output bytes the worker will copy back out of buf[] (read)
    // response
    int64_t ret;      // syscall return value, or -errno
    uint32_t outlen;  // valid output bytes in buf[]
    uint64_t nserved; // sentry-maintained counter (measurement / leak diagnostics)
    uint8_t buf[SENTRY_BUFSZ];
};

static struct sentry_ring *g_ring;
static pid_t g_sentry_pid;

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
        return 1;
    // --- NEXT (not yet wired; need 2-buffer / iovec / fd-passing marshaling) ---
    // 65 readv, 66 writev, 67 pread, 68 pwrite, 78 getdents64, 79 newfstatat, 80 fstat, 291 statx,
    // 198 socket, 200 bind, 203 connect, 201 listen, 202 accept, 206 sendto, 207 recvfrom,
    // 221 execve, 220 clone(fork), 260 wait4, ...
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
// Holds host authority. Spins for a request, rebuilds a cpu from the marshaled registers, redirects
// the one guest-buffer pointer arg into the shared ring (so service_local() never touches worker/
// guest memory), and runs the REAL service_local() -- identical jail/proc/overlay policy, identical
// bytes. Never returns. NOTE: it MUST call service_local() (the canonical switch), not service() --
// service() would re-enter syscall_route() in this (g_untrusted) process and recurse onto the ring.
static void sentry_loop(void) {
    struct sentry_ring *R = g_ring;
    struct cpu tmp;
    for (;;) {
        uint32_t spins = 0;
        while (atomic_load_explicit(&R->turn, memory_order_acquire) != 1) {
            if (atomic_load_explicit(&R->quit, memory_order_acquire)) _exit(0);
            if (++spins > 256) {
                if (getppid() == 1) _exit(0); // orphan-guard: worker died/crashed -> don't spin forever
                sched_yield();
                spins = 0;
            }
        }
        memset(&tmp, 0, sizeof tmp);
        G_RAWNR(&tmp) = R->rawnr; // service_local() re-runs G_NORMALIZE on this as a no-op (already *at)
        G_A0(&tmp) = R->a[0];
        G_A1(&tmp) = R->a[1];
        G_A2(&tmp) = R->a[2];
        G_A3(&tmp) = R->a[3];
        G_A4(&tmp) = R->a[4];
        G_A5(&tmp) = R->a[5];
        if (R->bufarg == 1) G_A1(&tmp) = (uint64_t)R->buf; // redirect a1 -> ring buf (THE crossing point)
        service_local(&tmp);                               // real host authority + container policy
        int64_t ret = (int64_t)G_RET(&tmp);
        R->ret = ret;
        R->outlen = (R->outcap && ret > 0) ? (uint32_t)ret : 0; // read: bytes already landed in R->buf
        R->nserved++;
        atomic_store_explicit(&R->turn, 0, memory_order_release); // hand back to the worker
    }
}

// ------------------------------------------------------------------ worker-side init / teardown
static void sentry_init(void) {
    g_ring = mmap(NULL, sizeof(struct sentry_ring), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
    if (g_ring == MAP_FAILED) {
        perror("[sentry] ring mmap");
        _exit(71);
    }
    atomic_store_explicit(&g_ring->turn, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ring->quit, 0, memory_order_relaxed);
    pid_t pid = fork(); // sentry forks AFTER load -> inherits the fd table / jail config / auxv / cwd
    if (pid < 0) {
        perror("[sentry] fork");
        _exit(71);
    }
    if (pid == 0) {
        sentry_loop(); // child: never returns
        _exit(0);
    }
    g_sentry_pid = pid;
    if (g_sentry_sandbox) worker_sandbox(); // confine the worker (scoped; see k_worker_sbpl note)
}

static void sentry_shutdown(void) {
    if (!g_ring || !g_sentry_pid) return;
    atomic_store_explicit(&g_ring->quit, 1, memory_order_release);
    int st;
    waitpid(g_sentry_pid, &st, 0);
    fprintf(stderr, "[sentry] forwarded %llu syscalls; sentry reaped\n", (unsigned long long)g_ring->nserved);
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

    struct sentry_ring *R = g_ring;
    R->rawnr = G_RAWNR(c);
    R->a[0] = G_A0(c);
    R->a[1] = G_A1(c);
    R->a[2] = G_A2(c);
    R->a[3] = G_A3(c);
    R->a[4] = G_A4(c);
    R->a[5] = G_A5(c);
    R->bufarg = -1;
    R->inlen = 0;
    R->outcap = 0;

    if (nr == 56) { // openat: copy the NUL-terminated path (a1) into the ring
        const char *p = (const char *)G_A1(c);
        uint32_t n = 0;
        if (p)
            while (n < SENTRY_BUFSZ - 1 && p[n]) n++;
        if (p) memcpy(R->buf, p, n);
        R->buf[n] = 0;
        R->inlen = n + 1;
        R->bufarg = 1;
    } else if (nr == 64) { // write: copy the payload (a1, len a2) into the ring; cap to BUFSZ
        uint32_t n = G_A2(c) > SENTRY_BUFSZ ? SENTRY_BUFSZ : (uint32_t)G_A2(c);
        memcpy(R->buf, (const void *)G_A1(c), n);
        R->inlen = n;
        R->bufarg = 1;
        R->a[2] = n; // sentry writes exactly the bytes we shipped (a short write is legal -> guest loops)
    } else if (nr == 63) { // read: reserve the out window (a1 dest, len a2); cap to BUFSZ
        uint32_t n = G_A2(c) > SENTRY_BUFSZ ? SENTRY_BUFSZ : (uint32_t)G_A2(c);
        R->outcap = n;
        R->bufarg = 1;
        R->a[2] = n; // short read is legal -> guest loops
    }
    // 57 close / 62 lseek: no buffer.

    // ---- ring round-trip ----
    atomic_store_explicit(&R->turn, 1, memory_order_release); // publish request -> sentry
    uint32_t spins = 0;
    while (atomic_load_explicit(&R->turn, memory_order_acquire) != 0) { // await response
        if (++spins > 256) {
            sched_yield();
            spins = 0;
        }
    }

    if (nr == 63 && R->ret > 0) { // read: copy the bytes the sentry fetched back into guest memory
        uint32_t n = R->outlen > R->outcap ? R->outcap : R->outlen;
        memcpy((void *)G_A1(c), R->buf, n);
    }
    G_RET(c) = (uint64_t)R->ret;
}

// ------------------------------------------------------------------ NEXT sentry PR (roadmap)
// 1. Per-context rings: this ring is single-producer. A forked/threaded guest spawns a second worker
//    that contends on the one mailbox and stalls (busybox sh stalls at its first clone). Give each
//    worker thread/process its own ring + a sentry servicing N rings (one sentry thread per ring).
// 2. Complete the fs/net/proc forwarded set: two-buffer marshaling for newfstatat/fstat/statx, iovec
//    for readv/writev, getdents64, pread/pwrite, then socket/bind/connect/send/recv and
//    execve/clone(fork)/wait4. Once complete, DDJIT_SANDBOX becomes sound for ANY guest.
// 3. SCM_RIGHTS fd passing: for a guest fd a LOCAL worker syscall must touch (file-backed mmap), the
//    sentry sendmsg()es the fd to the worker over a control socketpair; the worker's fds stay virtual.
// 4. Futex/__ulock wakeup: replace the spin with a process-shared futex on `turn` to drop idle CPU.
// 5. Sentry-side policy: add an allow/deny layer (path allowlists, net egress) so the sentry ENFORCES
//    rather than merely executes.
