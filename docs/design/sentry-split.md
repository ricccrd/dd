# DESIGN — Untrusted-guest isolation: the sentry process-split (subsystem #3)

Status: design. No code yet. Tracks PLAN.md "Next work" item **#3** ("Untrusted-guest
isolation — the sentry process-split. Seccomp/Seatbelt-locked sandbox task + trusted sentry
over a syscall ring. Required only for untrusted images.").

This document is meant to be **executable**: it names the exact functions and files that change,
defines the wire protocol, and ends with a first PR that is a small, mergeable slice.

---

## 0. The problem — there is no trust boundary today

`dd` runs a Linux guest by translating its code to host arm64 and servicing its syscalls
**in-process**. The JIT engine, the guest's translated code, the guest's memory, and the host's
real file descriptors all live in **one address space**, in one host process. Concretely:

- The dispatcher loop `run_guest()` (`dd-jit/src/runtime/jit/dispatch.c:40`) runs translated guest
  code via `run_block()` and, when a block exits with `c->reason == R_SYSCALL`, calls
  **`service(c)`** directly (`dispatch.c:122`). That is the *entire* kernel boundary — a normal C
  function call, same stack, same address space.
- `service()` (`dd-jit/src/runtime/os/linux/service.c:72`) is a ~2800-line `switch (nr)` that calls
  the **real host libc/syscalls** with guest-supplied arguments: `openat`/`read`/`write`
  (cases 56/63/64), `socket`/`connect`/`bind` (198/203/200), `execve` (221), `ioctl` (29), etc.
- Guest memory is **identity-mapped** (guest VA == host VA; see the agent finding on
  `targets/linux_aarch64.c` heap/stack mmap and the `g_gmap[]` registry in
  `os/linux/container/vfs.c`). The guest's translated code *cannot* reach into the JIT's own data
  by construction of the translation, **but** a guest that finds a JIT bug, or a malicious image
  that exploits any of the 178 hand-written syscall handlers, runs with the **full ambient
  authority of the host process**: every fd the process holds, the whole host filesystem reachable
  through `confine()` bugs, the host network, and `execve` of host binaries.

The container model (`os/linux/container/{vfs,netns,state}.c`) is **cooperative confinement**, not
a sandbox: `confine()` rewrites guest paths into the rootfs overlay, `netns` swaps `AF_INET` for
per-container `AF_UNIX`, rlimits cap mem/pids. All of that is enforced *by the same trusted code
that the guest is trying to subvert*. There is no second principal. For **trusted** images
(our own base images, the test matrix) this is fine and fast, and we keep it. For **untrusted**
images (a user's `docker run some/random-image`) we need a real boundary: a hostile guest that
fully compromises the JIT must still be unable to touch the host.

**Goal:** split the runtime into two OS processes with different privilege:

- a **SANDBOX** task — the JIT + the guest's translated code, locked down by macOS Seatbelt
  (`sandbox_init`) plus a minimal syscall surface, holding **no** host fds, **no** filesystem
  access, **no** network. If the guest pops the JIT, it lands in an empty, sealed process.
- a **SENTRY** — the trusted parent that owns the real resources (the host fd table, the rootfs
  mounts, the network, the ability to `execve`) and **services** the sandbox's syscall requests.
- a **RING** — a shared-memory request/reply transport between them. Every syscall the sandbox
  cannot safely perform itself becomes an IPC round-trip to the sentry.

This is the gVisor shape, with our own naming: in gVisor the *Sentry* is the trusted user-space
kernel and a *Gofer* owns the filesystem; here our **sentry** plays both the trusted-kernel and
gofer role, and the **sandbox** holds only the untrusted JIT + guest.

---

## 1. Process topology + trust boundary

```
                          ddockerd / dd-daemon  (runtime.rs: spawn_live)
                                   │ fork/exec, untrusted image → --untrusted
                                   ▼
   ┌─────────────────────────────────────────── SENTRY (trusted) ───────────────────────────────┐
   │  owns: host fd table, rootfs overlay + binds (confine/vfs.c), netns sockets (netns.c),       │
   │        port-map, real execve, container state (state.c), the daemon stdio pipes              │
   │  runs: service_remote() — the EXTERNAL half of today's service() switch                      │
   │  locked down for ITS OWN syscalls too (seccomp on linux host / a looser Seatbelt on macOS)   │
   │                                                                                              │
   │     ┌──────────── shared memory segment (the RING) ────────────┐                             │
   │     │  control: N per-thread channels (req/reply slots)        │   ← only shared surface      │
   │     │  data:    bounce-buffer arena (read/write payloads)      │                             │
   │     └──────────────────────────────────────────────────────────┘                             │
   │                                   ▲ reads requests / writes replies                          │
   └───────────────────────────────────┼──────────────────────────────────────────────────────────┘
                                        │  shm fd + mach reply port passed at spawn; THEN sealed
   ┌───────────────────────────────────┼────────────── SANDBOX (untrusted) ─────────────────────┐
   │  Seatbelt: deny file* / network* / process-exec / mach-lookup; allow only anon vm + JIT     │
   │  holds: NO host fds (except the ring shm, already mapped) ; NO rootfs ; NO sockets           │
   │  runs:  run_guest() → translated guest code (64MB MAP_JIT cache) → service_local()          │
   │         local fast-path for compute/memory; everything else marshalled onto the RING        │
   └──────────────────────────────────────────────────────────────────────────────────────────────┘
```

**The trust boundary is the process boundary.** The sandbox's only channel to the world is the
ring. The sentry never dereferences a guest pointer it was handed; it only touches the shared
bounce arena (and, in the optimized variant §2.4, a guest-memory mapping it set up itself). A full
compromise of the sandbox yields a process with no fds, no fs, no net, and a single pipe to a
trusted broker that validates every request.

**Who keeps what:**

| Resource | Today (in-process) | After the split |
|---|---|---|
| Host fd table | the one process | **sentry only**; sandbox sees virtual guest-fd integers |
| rootfs overlay / `confine()` | `vfs.c` in-process | **sentry**; path resolution happens server-side |
| netns sockets / port-map | `netns.c` in-process | **sentry** |
| `execve` of host binary | `service.c:221` in-process | **sentry** (spawns a *new sandbox* for the new image) |
| 64MB MAP_JIT code cache | in-process | **sandbox** (stays; never crosses the boundary) |
| guest memory (heap/stack/mmaps) | in-process | **sandbox**; sentry sees it only via bounce buffers |
| container state (hostname/uid/pids) | `state.c` | **sentry** (authoritative) + cached read-only in sandbox |

---

## 2. The split of `service()` — local vs. remote

Today `service(c)` is one switch. We split it by **what authority the case needs**:

- **`service_local(c)` (runs in the SANDBOX):** cases that touch *only guest memory or
  process-local CPU state*. No host fd, no fs, no net. These stay in-process — zero IPC.
- **`service_remote(c)` (runs in the SENTRY):** cases that touch a host fd, the filesystem, the
  network, the container's authoritative state, or spawn a process. The sandbox marshals these
  onto the ring and blocks until the reply lands.

The classification below is derived by reading every `case` in `service.c`.

### 2.1 Stays LOCAL in the sandbox (no round-trip)

These either mutate `struct cpu` (the per-thread register file in
`include/cpu_aarch64.h` / `cpu_x86_64.h`) or anonymous guest memory, both of which live in the
sandbox:

- **Memory:** `brk` (214), `mmap` of **anonymous** memory (222 with `MAP_ANON`), `munmap` (215),
  `mremap` (216), `mprotect` (226 — today a no-op), `madvise` (233/28), `mlock*` (228/229/230/231).
  These are `mach_vm`/`mmap(MAP_ANON)` calls and are explicitly **allowed** by the Seatbelt
  profile. (File-backed `mmap`, where the fd is sentry-owned, is the exception — see §2.3.)
- **Threading & futex:** `clone`/`CLONE_THREAD` → `spawn_thread()` (`thread.c:70`), `futex` (98 →
  `futex_op`, `thread.c:13`), `set_tid_address` (96), `set_robust_list` (99), `sched_yield` (124).
  The futex queue is a process-local pthread condvar — threads share the sandbox address space, so
  this is correct unchanged.
- **Signals (state only):** `rt_sigaction` (134), `rt_sigprocmask` (135), `sigaltstack` (132),
  `rt_sigreturn` via `do_sigreturn`, the pending-bit machinery in `signal.c`
  (`g_sigact`/`g_pending`/`maybe_deliver_signal`). Handlers and masks are guest state. (Cross-
  process signal *delivery* — kill to another container pid — is remote; see §2.3.)
- **Clocks/identity (pure compute or container-synthesized):** `clock_gettime` (113),
  `gettimeofday` (169), `getpid`/`getppid`/`gettid` (172/173/178 → `container_pid`),
  `getuid`/`geteuid`/`getgid` (174/175/176/177 → `cuid`/`cgid` from `state.c`), `uname`,
  `getrandom` (278 — `arc4random`, no host fd), `prctl PR_SET/GET_NAME` (`g_procname`),
  `arch_prctl`/TLS writes, the `sched_*` stubs (118–127), `getcpu` (168).
- **Exit:** `exit`/`exit_group` (93/94) set `c->exited` (still notify the sentry, §3.6).
- **SysV IPC (in-process backing):** `shmget/at/dt` (194/196/197), `semget/op/ctl` (190–193),
  `msgget/snd/rcv` (186–189). These are backed by host SysV objects today, **but** they are
  namespaced by `DD_NETNS` and are a host-OS surface — Seatbelt does not gate SysV IPC cleanly, and
  a shared SysV segment is a potential escape vector. **Decision: route SysV IPC to the sentry**
  (§2.3). Anonymous + POSIX-shm-via-file paths used by `shm_open` (which opens a host file) are
  already fd-based and therefore remote.

### 2.2 The data-movement subtlety

A `read(fd, buf, n)` is *remote* (the fd is the sentry's) but it must land bytes into **guest**
memory (`buf`), which is the sandbox's. The sentry cannot dereference `buf` — different address
space. Two mechanisms (§2.4) solve this: a **bounce arena** in the ring (copy in/out), or a
**shared guest-memory mapping** (sentry reads/writes the guest VA directly). The first PR uses the
bounce arena; the shared mapping is the perf optimization.

### 2.3 Round-trips to the SENTRY (`service_remote`)

Everything that needs real host authority. Grouped by family (the families are the natural unit for
the phased rollout in §6):

- **File I/O & metadata:** `openat` (56), `read`/`write` (63/64), `pread`/`pwrite` (67/68),
  `preadv`/`pwritev` (69/70/286/287), `close` (57), `lseek` (62), `fstat`/`newfstatat`/`statx`
  (80/79/291), `getdents64` (61), `readlinkat` (78), `faccessat` (48/439), `fcntl` (25),
  `ioctl` (29), `dup`/`dup3` (23/24), `pipe2` (59), `ftruncate` (46), `fallocate` (285), `flock`
  (32), `fsync`/`fdatasync` (82/83), `unlinkat`/`renameat2`/`mkdirat`/`symlinkat`/`linkat`
  (35/38/34/36/37), `chdir`/`getcwd` (49/17), `chmod`/`chown`/`umask` (52/53/55/90/91/166),
  `utimensat` (88), `statfs`/`fstatfs` (43/44), `memfd_create` (279), `shm_open` path,
  `epoll`/`eventfd`/`signalfd`/`inotify`/`timerfd` fd creation (the fd objects are sentry-owned).
- **Networking:** `socket` (198), `connect` (203), `accept`/`accept4` (202/242), `bind` (200),
  `listen` (201), `getsockname`/`getpeername` (204/205), `setsockopt`/`getsockopt` (208/209),
  `sendto`/`recvfrom` (206/207), `sendmsg`/`recvmsg` (211/212/269/270), `shutdown` (210),
  `socketpair` (199), `select`/`poll`/`ppoll`/`epoll_wait` (72/73/22/7/232/21). All of these go
  through the sentry's `netns` translation and port-map.
- **Process lifecycle:** `execve` (221) → the sentry tears down and **respawns a fresh sandbox**
  for the new image (see §6 Phase 4; this is the hardest case and also the one most worth
  isolating). `wait4`/`waitid` (260/95), `kill`/`tgkill` (129/131), cross-container signal
  delivery. `fork` (`clone` without `CLONE_THREAD`, `service.c:220`) — **see §7 (Risks); fork of a
  sandboxed JIT is the thorniest part.**
- **Container-authoritative state:** `sethostname`, `setuid`/`setgid` drops, `prlimit64` (261),
  SysV IPC (per §2.1 decision).

### 2.4 Moving bytes across the boundary — two transports

**(A) Bounce arena (first PR, simple, always correct).** The ring's shared segment carries a data
region. For a `write`, the sandbox `memcpy`s up to the payload into the arena, sends the request
with an (offset,len) into the arena; the sentry `write()`s from the arena to the real fd. For a
`read`, the sentry `read()`s into the arena, replies with the count, and the sandbox `memcpy`s out
into guest `buf`. Cost: one extra copy each way; correctness is trivial because no cross-process
pointer is ever dereferenced. Large transfers are chunked to the arena size.

**(B) Shared guest-memory mapping (optimization).** Allocate the guest's mmap arena (heap/stack/
anon) as a single `MAP_SHARED` region backed by a memfd/`shm_open` object created by the **sentry**
before spawn, and have the sandbox map it `MAP_FIXED` at the same VA (the runtime already needs
fixed placement for the identity map; see the `g_gmap[]` registry and the `__PAGEZERO`/bias notes
in PLAN.md "Platform limitations"). Then the sentry can `read()`/`write()` *directly* into the
guest buffer by VA — zero bounce copy — because the same physical pages are mapped in both. This is
how we recover near-native bulk I/O throughput, but it widens the shared surface (the sentry now
maps guest memory) and interacts with the non-PIE/`__PAGEZERO` placement bug, so it is **not** in
the first PR. Document the tradeoff; gate it behind a second flag.

---

## 3. The RING protocol

### 3.1 Segment layout

One POSIX shared-memory object (`shm_open`, created by the sentry, fd inherited by the sandbox at
spawn, then unlinked). Three regions:

```
 ┌────────────────────────────────────────────────────────────────────┐
 │ header      : magic, version, n_channels, arena_off, arena_len      │
 │ channels[N] : one cache-line-aligned control slot per guest thread  │
 │ arena       : bounce-buffer data region (e.g. 2 MB, chunked)        │
 └────────────────────────────────────────────────────────────────────┘
```

`N` = max concurrent guest threads (start at, say, 256; a guest thread that can't get a channel
falls back to a global lock + a shared channel). One channel **per guest thread** is the key design
choice: a blocking remote syscall on thread A must not stall thread B. Since each guest thread is a
host pthread running its own `run_guest()` (`thread.c:58` `thread_trampoline`), each gets its own
channel and blocks independently.

### 3.2 Channel control slot (fixed 64-byte header + inline args)

```c
struct ring_chan {                 // one per guest thread, cache-line aligned
    _Atomic uint32_t seq;          // request sequence; odd = request posted, even = reply ready
    _Atomic uint32_t state;        // EMPTY / REQ / RUNNING / REPLY
    uint32_t  futex;               // sandbox blocks here (FUTEX_WAIT); sentry futex-wakes
    uint32_t  nr;                  // Linux syscall number
    uint64_t  a[6];                // a0..a5 (registers; pointers are guest VAs, see below)
    int64_t   ret;                 // reply: kernel return (negative errno on error)
    uint32_t  in_off,  in_len;     // request payload slice in the arena (e.g. write() buffer)
    uint32_t  out_off, out_len;    // reply payload slice in the arena (e.g. read() result)
    uint32_t  flags;               // INLINE_DONE, CANCELLED, SIGNAL_PENDING, ...
};
```

Pointer arguments are **not** sent as raw guest VAs to be dereferenced by the sentry. The sandbox's
marshalling layer (`syscall_route()`, §3.5) knows each syscall's shape (it already does — that is
what the current `service()` cases encode) and copies the *pointed-to* bytes into the arena,
replacing the pointer slot with an `(in_off,in_len)`. The sentry reads only the arena. This keeps
"never dereference a guest pointer" as an invariant.

### 3.3 Request / reply handshake (lock-free SPSC per channel)

Single-producer (one guest thread) / single-consumer (one sentry worker) per channel, so no lock on
the fast path:

1. **Sandbox** fills `nr`, `a[]`, copies any input buffers to the arena, sets `in_*`/`out_*`,
   stores `state = REQ`, bumps `seq` to odd with a release barrier, then signals the sentry
   (§3.4) and **blocks** (§3.6).
2. **Sentry** worker observes `state == REQ` (acquire), runs `service_remote()` against real
   resources, writes `ret` and any `out_*` payload into the arena, stores `state = REPLY`, bumps
   `seq` to even (release), and wakes the sandbox thread (§3.4).
3. **Sandbox** wakes, reads `ret` (acquire), copies arena `out_*` bytes back into the guest buffer,
   writes `c->x[0] = ret` via `G_RET(c)`, returns to `run_guest()` which advances `c->pc += 4`
   exactly as it does after the in-process `service(c)` today (`dispatch.c:124-127`).

### 3.4 Wakeups

Two-tier, to keep the common case cheap and avoid busy-spin:

- **Notify sentry:** a small adaptive spin on `state` by the sentry worker(s); if idle past a
  threshold, the worker blocks on an eventfd/`kqueue`-readable doorbell that the sandbox pokes
  (one byte). On macOS the sandbox→sentry doorbell is the writable end of a pipe handed in at spawn
  (Seatbelt allows writing an already-open fd; see `signal.c`'s self-pipe pattern, `g_sigfd_pipe`).
- **Notify sandbox:** the sandbox blocks on the `futex` word (Linux host) or on a per-channel
  Mach semaphore / `__ulock_wait` (macOS host). The sentry sets `state = REPLY` then wakes it.
  Reuse the futex condvar machinery already in `thread.c` if a portable primitive is wanted.

### 3.5 Where the seam goes in the code

`run_guest()` (`dispatch.c:122`) changes from:

```c
if (c->reason == R_SYSCALL) { service(c); ... }
```

to a one-line redirection through a router:

```c
if (c->reason == R_SYSCALL) { syscall_route(c); ... }   // was: service(c)
```

`syscall_route(c)` is new (sandbox side):

```c
static void syscall_route(struct cpu *c) {
    if (!g_untrusted)            { service(c); return; }       // trusted image: unchanged path
    uint32_t nr = (uint32_t)G_NR(c);
    if (is_local_syscall(nr))    { service_local(c); return; } // §2.1 fast-path, in sandbox
    ring_call(c);                                              // §3.3 marshal → sentry → reply
}
```

- `g_untrusted` is the runtime flag (§4). When false, **nothing changes** — `service(c)` is called
  exactly as today, so the trusted/test path is byte-for-byte the current behavior.
- `is_local_syscall(nr)` is a static bitmap built from §2.1.
- `service_local(c)` is the existing `service()` with the remote cases compiled out (or simply
  `service()` itself for the first PR — see §6 Phase 1; correctness first, then carve).
- `service_remote(c)` on the sentry side is **the same `service.c` translation unit** compiled into
  the sentry binary, entered only for remote `nr`. We do **not** rewrite the 178 handlers; we
  relocate where they run. The handler body is unchanged; only its *arguments* arrive via the ring
  instead of the register file, and `G_A*/G_RET` are re-pointed at the channel slot.

### 3.6 How a blocking syscall suspends the guest thread

The guest thread is a host pthread executing `run_guest()`. After `ring_call()` posts the request it
calls the blocking primitive (§3.4) on its channel's `futex`. That **parks the pthread** — which is
exactly the right semantics: only *that* guest thread is suspended; sibling guest threads keep
executing translated code and can post their own ring requests on their own channels. When the
sentry replies and wakes the futex, the pthread resumes inside `ring_call()`, copies results out,
and returns up into `run_guest()`. From the guest's point of view the `svc` instruction simply took
a while — identical to a real kernel blocking it on I/O.

**Async signals while blocked:** the sentry can set `flags |= SIGNAL_PENDING` and wake the channel
with `ret = -EINTR` so the guest sees `EINTR` and `run_guest()`'s `maybe_deliver_signal(c)`
(`dispatch.c:132`) runs on resume — preserving the existing signal-on-syscall-return behavior in
`signal.c`.

---

## 4. macOS Seatbelt profile (the sandbox lockdown)

macOS has **no seccomp-bpf** — there is no per-syscall-number filter for third-party processes. The
closest host primitive is **Seatbelt** (`sandbox_init` + an SBPL profile), which gates by
*operation class* (file, network, mach, process-exec, sysctl, ...), not by syscall number. We
already use it: `os/linux/.../darwinjail.c:47-57` calls `sandbox_init` with a write-confinement
profile for the **darwin-guest** path. **That profile is `(allow default)` — write-confinement
only — which is the opposite of what an untrusted sandbox needs.** The sandbox profile must be
**deny-by-default**. Sketch:

```scheme
(version 1)
(deny default)                          ; nothing is allowed unless named below

;; --- memory & JIT: the whole point of dd ---
(allow process-info-pidinfo (target self))
(allow sysctl-read)                     ; pthreads/malloc read a few sysctls at init
;; MAP_JIT / pthread_jit_write_protect_np needs the JIT entitlement at the binary level
;; (com.apple.security.cs.allow-jit) — Seatbelt then permits the W^X toggle. No fs/net needed.

;; --- threads ---
(allow mach-priv-task-port (target self))   ; pthread_create / thread bootstrap, self only

;; --- the ONE channel out: the ring + doorbell, already-open fds inherited at spawn ---
;; no (allow file*) — the sandbox opens nothing. The shm + pipe fds are already mapped/open.

;; --- HARD DENIES (explicit for auditing even though deny-default covers them) ---
(deny file-read*  (subpath "/"))        ; no filesystem, at all
(deny file-write* (subpath "/"))
(deny network*)                         ; no sockets of any kind
(deny process-exec*)                    ; no execve — exec is brokered to the sentry
(deny mach-lookup (global-name-regex #".*"))   ; no bootstrap lookups → can't reach window server, etc.
```

What it must **deny**, and why it matters for an escape:

- **`file-read*` / `file-write*`** — the sandbox must have *zero* fs reach. Even read is denied so a
  JIT-popped guest can't exfiltrate host files; all file I/O is the sentry's.
- **`network*`** — no `socket()` succeeds in the sandbox; all net is brokered (and goes through the
  existing `netns` isolation in the sentry).
- **`process-exec*`** — no spawning host binaries; `execve` is a ring request that the sentry
  satisfies by launching a *new sandbox*.
- **`mach-lookup`** — critical on macOS: without it, a compromised process can reach the
  bootstrap server, WindowServer, etc., which is a classic local-privilege surface. Deny all global
  names; the ring needs none (it's an inherited fd, not a looked-up port).
- **`sysctl-write`, `iokit-*`, `system-*`** — denied by `deny default`.

**Entitlements / signing:** the sandbox binary needs `com.apple.security.cs.allow-jit` (for
`MAP_JIT` + `pthread_jit_write_protect_np`, already required by the engine) and must **not** carry
`allow-unsigned-executable-memory` or `disable-library-validation` more broadly than needed. The
sentry binary is a *separate* executable with its own (broad) entitlements; the sandbox executable
is minimal. This is why the split is also a **codesigning** boundary: the two roles get different
entitlement sets, which a single-process design cannot express.

**Linux host (the eventual linux→linux build):** here we *do* get seccomp-bpf. The PLAN portability
note (`hal/<os>`: "linux = mprotect/SIGSEGV-ucontext/**seccomp**") already anticipates this. On
Linux the sandbox installs a seccomp-bpf allowlist (only `futex`, `mmap`/`mprotect`/`munmap` of
anon, `rt_sigreturn`, `exit`, `read`/`write` on the *ring fds only*, `clock_gettime`, `nanosleep`)
with `SECCOMP_RET_KILL_PROCESS` as the default — a true syscall-number filter, strictly stronger
than Seatbelt's operation classes. The ring protocol is identical; only the lockdown primitive
differs. Put both behind the HAL seam: `hal_sandbox_lockdown()` → `sandbox_init` on darwin,
`seccomp(...)` on linux.

---

## 5. Composition with the in-process model (the flag)

The split is **opt-in per container** and defaults off, so trusted images and the whole existing
test matrix are untouched.

- **Flag plumbing.** Add `untrusted: bool` to `SpawnConfig` (`dd-jit/src/lib.rs`, alongside
  `rootfs`/`netns`/`mem_max`). `SpawnConfig::command()` adds `--untrusted` (or exports
  `DD_UNTRUSTED=1`, matching the existing env-handoff style — `DD_NETNS`, `DD_MEM_MAX`, etc., parsed
  in `targets/linux_aarch64.c`). The daemon sets it in `dd-daemon/src/runtime.rs` `spawn_cfg()` from
  an image-trust policy (e.g. images the user built locally / our base images = trusted; pulled
  third-party = untrusted; overridable by a `docker run` security-opt).
- **Runtime branch.** `g_untrusted` (read once at startup) selects the topology:
  - **trusted:** today's path. `main()` loads the ELF and calls `run_guest()`; `syscall_route()`
    calls `service(c)` directly. The sentry/ring code is never touched. **Zero overhead, zero
    behavior change** — this is what keeps `make test` green.
  - **untrusted:** the entry point becomes the **sentry**. It creates the ring shm, sets up the
    container resources (the `vfs`/`netns`/`state` setup it does today, but now *kept* in the
    sentry), `fork`/`posix_spawn`s the **sandbox** child with the shm + doorbell fds inherited and
    `DD_ROLE=sandbox`, the child applies `hal_sandbox_lockdown()` then loads the ELF and runs
    `run_guest()` with `g_untrusted` routing remote syscalls to the ring; the parent loops in the
    sentry servicing requests.
- **One binary, two roles** (recommended): the same `dd-jit` runtime binary, role chosen by
  `DD_ROLE` — keeps the build simple and lets `service_remote` literally be the same compiled
  `service.c`. (Two binaries is the alternative if entitlement separation needs distinct Mach-O
  signatures; start with one + a re-exec-self for the entitlement boundary if required.)

Because the seam is a single `if (g_untrusted)` in `syscall_route()`, **subsystem #3 never
regresses the in-process engine** — the JIT, the code cache, the frontends, and `service.c`'s
handler bodies are reused verbatim in both roles.

---

## 6. Performance cost + mitigations

**The cost is real and structural:** every external syscall is now an IPC round-trip — two cache-
line writes, a futex/semaphore park+wake, and (mechanism A) up to two `memcpy`s through the bounce
arena, versus today's direct function call. A round-trip is on the order of a few microseconds vs.
tens of nanoseconds. I/O-bound and chatty-syscall guests (a shell doing `stat` storms, `getdents`
loops) pay the most; compute-bound guests pay nothing (their hot loop never leaves the sandbox).

Mitigations, in rough order of payoff:

1. **Keep compute/memory local (§2.1).** The biggest win: `futex`, `mmap(ANON)`, `brk`, clocks,
   TLS, signals, thread spawn — the high-frequency syscalls in real workloads — never round-trip.
   The translated hot path (block chaining / IBTC in `cache.c`/`dispatch.c`) is untouched.
2. **Shared guest-memory mapping (§2.4 B).** Eliminates the bounce copy for bulk `read`/`write`,
   recovering near-native throughput on large transfers. Gated behind a second flag because it
   widens the shared surface and touches the `__PAGEZERO` placement constraints.
3. **Batching / vectoring.** `writev`/`readv`/`sendmsg` already carry an iovec — marshal the whole
   vector in one request. For `getdents64` and directory walks, return a large arena fill per call.
   A future `io_submit`-style multi-request enqueue lets a guest post several independent requests
   before parking once.
4. **Adaptive spin before park (§3.4).** When the sentry is warm, the reply often lands within a
   short spin, avoiding the futex syscall entirely on both sides. Park only after a spin budget.
5. **Sentry worker pool sized to guest-thread concurrency.** One worker per active channel (or a
   small pool with work-stealing) so N guest threads get N concurrent in-flight syscalls — the
   round-trip latency overlaps across threads.
6. **Local stat/metadata cache.** The sentry already has the `g_mc[]` read-only-layer stat cache
   (`vfs.c`); a tiny read-through cache *in the sandbox* for immutable lookups (e.g. `statx` on
   lower-layer paths, `readlink` of stable symlinks) can satisfy repeats without a round-trip.
   Must be invalidated conservatively; only for provably-immutable overlay-lower entries.

Set a perf budget up front: the regression on an I/O-heavy benchmark (e.g. `busybox find`, a
`python` import storm from `make test-realsw`) is the headline number to track; compute benchmarks
(`heavy/bigmem`) should show ~0% regression and are the guardrail that local-fast-path is working.

---

## 7. Phased roadmap + the FIRST PR

### Phase 0 — design (this doc). ✅

### Phase 1 — FIRST PR: ring transport + ONE family behind the flag

**Smallest mergeable slice that proves the boundary end-to-end.** Scope:

1. **The flag.** Add `untrusted`/`DD_UNTRUSTED` + `DD_ROLE` plumbing (`lib.rs` `SpawnConfig`,
   `runtime.rs` `spawn_cfg`, env parse in `targets/linux_aarch64.c`). Default off ⇒ `make test`
   unchanged.
2. **The ring.** New `dd-jit/src/runtime/os/linux/ring.c` (+ `ring.h`): shm segment, `struct
   ring_chan`, `ring_post`/`ring_wait` (sandbox) and `ring_serve` (sentry), the bounce arena, the
   doorbell pipe, the futex/`__ulock` wakeups. Pure transport — no syscall logic.
3. **The router.** Replace `service(c)` at `dispatch.c:122` with `syscall_route(c)`; implement
   `is_local_syscall()` (start permissive: route **only the file-I/O read/write/open/close/lseek/
   fstat family** to the ring; everything else still calls `service(c)` locally — yes, that means
   the untrusted sandbox isn't fully sealed yet, but the *transport and the carve are proven*).
4. **The sentry side.** `main()` gains the `DD_ROLE=sentry` branch: create ring, `posix_spawn` the
   sandbox child (`DD_ROLE=sandbox`, ring fds inherited), then loop `ring_serve()` → dispatch the
   file-I/O family into the existing `service.c` handler bodies (refactored so the openat/read/
   write/close/lseek/fstat cases can be called with args from a channel slot rather than `c`).
5. **Lockdown stub.** `hal_sandbox_lockdown()` present but **gated to a no-op + a log line** in
   Phase 1 (so we can A/B the round-trip without fighting Seatbelt denials yet). Wire the real
   `sandbox_init` deny-default profile (§4) in Phase 3.
6. **Test.** One new `dd-tests` scenario: run a trivial guest (`cat /etc/os-release`, a small
   `python` print) under `--untrusted` and assert identical output to the trusted path. Prove the
   `read`/`write`/`open` round-trip works and bytes survive the bounce arena. Compute tests
   (`heavy/bigmem`) under `--untrusted` must show ~0% regression (they never round-trip).

Deliverable: a guest whose **file reads/writes physically execute in a different process**, behind
a flag, matrix still green. ~600–900 LOC, no handler rewrites.

### Phase 2 — carve the rest of the remote families
Route the remaining `service_remote` families (§2.3) one PR per family: metadata/dirs → network →
SysV IPC → process lifecycle (`wait`/`kill`/signals). Each PR moves a family from "called locally"
to "marshalled to sentry," guarded by the family's own tests. After this, the sandbox makes **no**
external syscalls itself.

### Phase 3 — seal the sandbox
Turn on the real deny-default Seatbelt profile (§4) + JIT entitlement on the sandbox binary; on the
linux host, the seccomp-bpf allowlist. Add the negative tests: assert that a guest's attempt to
`open("/etc/passwd")` *directly* (i.e. bypassing the ring) **fails**, that `socket()` in the
sandbox is denied, that `execve` of a host path is denied. This is the PR where the boundary
becomes load-bearing.

### Phase 4 — fork/exec under the split (the hard one)
`execve` (`service.c:221`): the sentry tears down the sandbox and launches a fresh sandbox for the
new image (clean address space — also sidesteps the dense-post-fork non-PIE crash documented in
PLAN.md). `fork` (`clone` w/o `CLONE_THREAD`, `service.c:220`): the sentry forks a **new sandbox**
and a paired sentry-side fd/state copy; the shared guest-memory mapping (§2.4 B) and COW semantics
make this the riskiest piece — sequence it last and lean on the existing fork+exec diagnosis work.

### Phase 5 — performance
Land the shared guest-memory mapping (§2.4 B), batching/vectoring, adaptive spin, worker pool, and
the sandbox-side metadata cache. Track the I/O-benchmark regression to a budget.

---

## 8. Risks

- **No seccomp on macOS.** Seatbelt gates *operations*, not syscall numbers, so the macOS sandbox
  is coarser than a Linux seccomp allowlist. Mitigation: the sandbox holds **no fds/fs/net by
  construction** (defense-in-depth) so the operation-class denies are sufficient; the true
  syscall-number filter arrives for free on the linux→linux build via the HAL seam. **Honest
  limitation to call out in the doc and to users.**
- **Data-copy cost / correctness of marshalling.** The bounce arena requires the router to know
  each syscall's pointer shape exactly (sizes of `struct stat`, iovecs, `msghdr`, `sockaddr`,
  `cmsg`/`SCM_RIGHTS`). The current `service.c` already encodes these, but moving them across a
  process boundary turns a latent bug (wrong size) into a data-corruption bug. `SCM_RIGHTS` fd
  passing (already broken per PLAN edge-cases) is *especially* hard: passing a real fd from sentry
  to sandbox is impossible if the sandbox can't hold fds — the sandbox's fds must be **virtual**,
  resolved by the sentry, so `SCM_RIGHTS` becomes "register a new virtual fd," not "install a host
  fd." Design for virtual guest-fd numbers from day one.
- **fork() of a JIT.** Forking a process that holds a 64MB MAP_JIT cache, multiple guest threads,
  and a shared ring is fraught (the existing flaky-fork+exec bug in PLAN.md is a warning). Phase 4,
  last; prefer "fork = spawn a fresh paired sandbox/sentry" over literal `fork()` of the sandbox.
- **Threads × channels.** A guest with more live threads than ring channels must degrade correctly
  (shared channel under a lock). Get the SPSC fast path and the fallback both right, or a thread
  storm deadlocks.
- **Signal/EINTR semantics across the boundary.** A signal arriving while a thread is parked on the
  ring must produce the right `EINTR`/restart behavior (`signal.c` `maybe_deliver_signal`). Easy to
  get subtly wrong; cover with the existing `edge/msgflags`-style differential tests.
- **Deadlock / liveness.** Sentry worker crash or a dropped doorbell must not hang a guest forever;
  need a watchdog and a "sentry died ⇒ SIGKILL the sandbox" supervision link (and vice-versa).
- **Perf regression scaring off the default.** If the I/O round-trip cost is too high, untrusted
  mode is unusable and everyone runs trusted (no isolation). The §6 mitigations — especially the
  shared mapping and keeping the hot syscalls local — must land before untrusted is a sane default
  for pulled images.
- **Entitlement/signing complexity.** Splitting into two entitlement sets (minimal sandbox + broad
  sentry) complicates the DMG packaging/notarization. Coordinate with the existing
  `packaging/`/`dd-app` signing flow.

---

## 9. Files this touches (map)

| File | Change |
|---|---|
| `dd-jit/src/runtime/jit/dispatch.c:122` | `service(c)` → `syscall_route(c)` (the one seam) |
| `dd-jit/src/runtime/os/linux/ring.c` (new) | ring transport: shm, channels, bounce arena, wakeups |
| `dd-jit/src/runtime/os/linux/service.c` | factor handler bodies so remote cases run from a channel slot; add `service_local`/`is_local_syscall`; **no handler logic rewritten** |
| `dd-jit/src/runtime/hal` seam (per PLAN) | `hal_sandbox_lockdown()`: `sandbox_init` (darwin) / `seccomp` (linux) |
| `dd-jit/src/runtime/os/darwin/darwinjail.c` | reference for the `sandbox_init` call pattern (but a **deny-default** profile, not `allow default`) |
| `dd-jit/src/runtime/targets/linux_aarch64.c` / `linux_x86_64.c` | `main()` gains `DD_ROLE` sentry/sandbox branch; parse `DD_UNTRUSTED` |
| `dd-jit/src/runtime/os/linux/{container/vfs.c,netns.c,state.c}` | unchanged logic, but now **owned by the sentry** in untrusted mode |
| `dd-jit/src/lib.rs` (`SpawnConfig`) | add `untrusted` field + `--untrusted`/`DD_UNTRUSTED` emit |
| `dd-daemon/src/runtime.rs` (`spawn_cfg`/`spawn_live`) | set `untrusted` from image-trust policy |
| `dd-tests/scenarios/` | `--untrusted` parity scenario (Phase 1) + sandbox-escape negative tests (Phase 3) |
