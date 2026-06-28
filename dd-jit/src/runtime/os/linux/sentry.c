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

// The shared-memory mailbox. `turn` is the ownership token: 0 => worker fills a request, 1 => sentry
// executes it. Strict ping-pong (no third state) => deadlock-free and, with release/acquire on turn,
// torn-message-free (all field writes happen-before the token flip the peer acquires).
struct sentry_ring {
    _Atomic uint32_t turn; // 0 = worker owns (build request), 1 = sentry owns (execute)
    _Atomic uint32_t quit; // worker sets at teardown -> sentry _exit()s
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
    // --- fs set wired in this PR ---
    case 61:  // getdents64  (out-buffer the dirents)         [NOTE: 78 in the old comment was wrong = readlinkat]
    case 65:  // readv       (flatten guest iovec -> ring)
    case 66:  // writev      (flatten guest iovec -> ring)
    case 67:  // pread64      (read + a3 offset)
    case 68:  // pwrite64     (write + a3 offset)
    case 79:  // newfstatat   (in-path a1 + out-struct a2, two-buffer)
    case 80:  // fstat        (out-struct a1)
    case 291: // statx        (in-path a1 + out-struct a4, two-buffer)
        return 1;
    // --- NEXT (need fd-passing / socket / process marshaling; see roadmap at bottom) ---
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
// each flagged guest-buffer pointer arg into the shared ring (so service_local() never touches worker/
// guest memory) -- including rebasing the flattened readv/writev iovec offsets to ring pointers -- and
// runs the REAL service_local() -- identical jail/proc/overlay policy, identical bytes. Never returns. NOTE: it MUST call service_local() (the canonical switch), not service() --
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
    default: break; // 57 close / 62 lseek: no buffer.
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
    default: break; // 56 openat / 57 close / 62 lseek / 64 write / 66 writev / 68 pwrite: no output bytes
    }
    G_RET(c) = (uint64_t)R->ret;
}

// ------------------------------------------------------------------ NEXT sentry PR (roadmap)
// 1. Per-context rings: this ring is single-producer. A forked/threaded guest spawns a second worker
//    that contends on the one mailbox and stalls (busybox sh stalls at its first clone). Give each
//    worker thread/process its own ring + a sentry servicing N rings (one sentry thread per ring).
// 2. Complete the fs/net/proc forwarded set. DONE this PR: two-buffer marshaling for newfstatat/
//    fstat/statx, iovec flatten for readv/writev, getdents64, pread64/pwrite64 (all over the generic
//    redir[] + iovn protocol). STILL LOCAL (fall through to service_local in the worker, where they
//    fail under Seatbelt -> forward next): socket/bind/connect/listen/accept/send/recv (sockets need a
//    sentry-owned fd + SCM_RIGHTS) and execve/clone(fork)/wait4 (need per-context rings, item 1). Once
//    the socket + process set lands, DDJIT_SANDBOX becomes sound for ANY guest.
// 3. SCM_RIGHTS fd passing: for a guest fd a LOCAL worker syscall must touch (file-backed mmap), the
//    sentry sendmsg()es the fd to the worker over a control socketpair; the worker's fds stay virtual.
// 4. Futex/__ulock wakeup: replace the spin with a process-shared futex on `turn` to drop idle CPU.
// 5. Sentry-side policy: add an allow/deny layer (path allowlists, net egress) so the sentry ENFORCES
//    rather than merely executes.
