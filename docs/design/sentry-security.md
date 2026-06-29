# Sentry security review — threat model for the untrusted-guest split

Status: security review (read-only over `dd-jit/src/runtime/os/linux/sentry.c` — the now-shipped
sentry process-split — and the daemon gate in `dd-daemon/src/runtime.rs`). The split's original design +
PoC docs have been removed now that the code has landed; this review tracks the **remaining** hardening
(seccomp, guard page, mach-lookup filter — see [`../PLAN.md` A.3](../PLAN.md)).
Scope: the **untrusted-guest sentry process-split**, whose purpose is to safely run *untrusted*
container images. This document is the security basis for deciding whether the sandbox can be
trusted with real untrusted images. **Short answer today: not yet — see the GAPs in §3.**

All line/function references are to `dd-jit/src/runtime/os/linux/sentry.c` unless noted.

---

## 1. Trust boundary

```
   daemon (dd-daemon/src/runtime.rs:127-134)  --security-opt sandbox|untrusted / DD_SANDBOX=1
        │   exports DDJIT_UNTRUSTED=1 + DDJIT_SANDBOX=1
        ▼
   ddjit process  ── sentry_init() forks ──►  SENTRY (trusted)
   ───────────────────────────────────────    ────────────────
   WORKER (untrusted)                          - holds the REAL host fd table, sockets, fs,
   - runs the JIT + translated guest             rootfs overlay/jail, /proc synth, netns
   - compute/memory only: brk, anon mmap,      - runs service_local() = the real authority
     futex, clocks, TLS, signals              - one servicer THREAD per ring (shared fd table)
   - holds NO real fs/net fds
   - (optionally) under a deny-default
     Seatbelt profile (DDJIT_SANDBOX)
        │
        └── shared surface = struct sentry_shm (one MAP_SHARED|MAP_ANON region) + g_ctl[N] socketpairs
```

The **process boundary is the trust boundary**. The worker is assumed *fully malicious*: the entire
point of the split (per `sentry-split.md` §0 and the threat-model header of `sentry.c`) is to
contain a worker in which the guest has found a JIT bug and is executing **arbitrary native code**.

Critical consequence, and the lens for the whole review:

> **All worker-side marshaling code in `syscall_route()` is part of the untrusted worker.** A
> compromised worker does not call the "nice" marshaling helpers — it writes the ring fields
> (`rawnr`, `a[6]`, `redir[6]`, `iovn`, `buf[]`) and the `g_shm->{quit,claim,ring[i].owner/turn}`
> control words **directly**, then flips `turn`. Every length cap, NUL-termination, and offset the
> worker-side code computes is therefore **not a security control**. The *only* line of defense is
> the sentry-side validation in `sentry_service_one()` (and what `service_local()` itself enforces).
> The sentry must re-validate **every** ring field as hostile, in **sentry-private** memory.

The shared region (`struct sentry_shm`, lines 166-170): `quit`, `claim`, and `ring[SENTRY_NRINGS]`,
each ring being `{turn, busy, owner, rawnr, a[6], redir[6], iovn, inlen, ret, nserved, buf[1MiB]}`
(lines 139-162). Plus the per-ring `g_ctl[i]` AF_UNIX socketpairs for SCM_RIGHTS fd-lend.

Note also that one `sentry_shm` belongs to **one container** (one `ddjit` process tree). All workers
sharing a ring pool — threads and `fork()`ed children of the same guest — are the **same untrusted
principal**. There is no cross-*tenant* surface inside one shm; the escalation target is always the
**sentry** (the trusted authority), not a sibling worker.

---

## 2. Attack surface (enumerated)

| # | Surface | Entry point |
|---|---|---|
| A | Ring scalar marshaling: `redir[i]` offset + `a[i]` length used by `service_local()` | `sentry_service_one()` 434-441, 486 |
| B | Ring vectored marshaling: `iovn` + flattened `iovec[]` in `buf[]` | `sentry_service_one()` 442-452 |
| C | Ring nested graph: `sendmsg`/`recvmsg` msghdr (name/iov/control offsets + lengths) | `sentry_service_one()` 458-482 |
| D | Socket length windows: SLEN/optlen/addrlen read from `buf[]` | service_local via redir; setup 808-839 |
| E | TOCTOU: validate-in-place then re-read from attacker-writable shared memory | `sentry_service_one()` 442-482 |
| F | SCM_RIGHTS fd-lend (worker names an fd to receive) | `SENTRY_OP_FDPASS` 414-421 |
| G | SCM_RIGHTS fd-send inside sendmsg control buffer | flatten 877-884, `service_local` |
| H | Seatbelt worker confinement | `worker_sandbox()` 390-402, `k_worker_sbpl` 372-383 |
| I | Lane/ownership control words (`owner`, `turn`, `quit`, `claim`) | 186-224, 563-575 |
| J | fork/exec/wait lanes | `syscall_route()` 608-662 |
| K | Residual gaps: shared sentry fd table, in-worker execve image read, futex spin | roadmap 1124-1153 |

---

## 3. Per-item findings

### A. Scalar offset bounds-checked, but length NOT clamped — **GAP (critical)**
`sentry_service_one()` 436-441 validates each redirected offset:

```c
uint32_t off = (uint32_t)R->redir[i];
if (off >= SENTRY_BUFSZ) { bad = 1; break; }
*ta[i] = (uint64_t)(R->buf + off);
```

This checks the **start pointer** is inside `buf[]`. It does **not** clamp the associated
length/count register (`a[2]` for read/write/recvfrom, `a[4]` for sendto destaddr, etc.) against the
*remaining window* `SENTRY_BUFSZ - off`. The length is copied verbatim into `tmp.a[]` (424-430) and
`service_local()` performs the real syscall with it. So a worker that sets, e.g.,
`redir[1] = SENTRY_BUFSZ - 16`, `a[2] = 0xFFFFFFFF`:

- **write/sendto/sendmsg** → `write(fd, buf+BUFSZ-16, 4 GiB)` reads ~off-the-end of the ring and
  exfiltrates **sentry memory** (the adjacent ring of another worker, or, for `ring[7]`, sentry
  private heap) out to a guest-controlled file/socket — **information disclosure**.
- **read/recvfrom/getdents** → `read(fd, buf+BUFSZ-16, 4 GiB)` lets the kernel **write** that many
  bytes past the window — **sentry memory corruption** (clobbers the next ring's `turn/busy/owner`
  and `buf`, or runs off the mapping → sentry SIGSEGV / DoS). A worker that controls the file
  contents (a file it `openat`'d) has a controlled OOB **write primitive in the trusted sentry**.

The worker-side code does cap these (`R->a[2] = n` with `n <= SENTRY_BUFSZ`, e.g. 699, 709), but per
§1 that cap is attacker-removable. **The sentry never re-derives or clamps the length.** This single
finding defeats the isolation guarantee. There is **no** `offset + len <= SENTRY_BUFSZ` check on the
scalar path.

### B. `iovn` (segment count) unbounded sentry-side — **GAP (critical)**
`sentry_service_one()` 442-452:

```c
if (!bad && R->iovn) {
    struct iovec *iv = (struct iovec *)(R->buf + (uint32_t)R->redir[1]);
    for (uint32_t k = 0; k < R->iovn; k++) { ... }
}
```

Two problems:
1. **`R->iovn` is never bounded** (the worker caps it to `SENTRY_IOVMAX` at 749, but that is removed
   by a malicious worker). With `iovn = 0xFFFFFFFF` the loop walks `iv[0..iovn]` off the end of
   `buf[]` — an OOB **read** (to test the clamp) *and* OOB **write** (`iv[k].iov_base = ...` writes
   the rebased pointer back into shared/over-the-end memory). Sentry crash or corruption.
2. **`redir[1]` is not required to be valid here.** The block only gates on `!bad && R->iovn`. If the
   worker sets `iovn > 0` but `redir[1] = -1` (skipped by the 437 `continue`, so `a[1]` is never
   rebased), then `(uint32_t)R->redir[1]` is `0xFFFFFFFF` and `iv = buf + ~4 GiB` — a **wild pointer
   deref** in the sentry on the first `iv[0]` access. DoS at minimum.

The per-segment check `boff > BUFSZ || len > BUFSZ || boff+len > BUFSZ` (446) is itself sound (no
uint64 overflow because each operand is pre-bounded), but it never runs safely because the loop bound
and base pointer are unchecked.

### C. msghdr `in` (iovlen) unbounded; lengths unclamped — **GAP (high)**
`sentry_service_one()` 458-482 reads `noff/ioff/in/coff` from `buf[]` and checks
`noff/ioff/coff >= SENTRY_BUFSZ` (464). But:
- **`in` (msg_iovlen, offset 24) is unbounded** — same OOB walk as §B over `iv[0..in]` (470-477).
- **`msg_namelen` (h+8) and `msg_controllen` (h+40) are not clamped** to `BUFSZ - noff` / `BUFSZ -
  coff`. After rebasing `msg_name`/`msg_control` to ring pointers, `service_local()`'s `sendmsg`
  passes those lengths to the kernel, which reads `controllen` bytes from `buf+coff` — an OOB read of
  the ring if the worker sets a large `controllen`. (The kernel caps `msg_namelen` at
  `sizeof(sockaddr_storage)`, softening the name case, but not the control case.)

### D. Socket length windows trusted from `buf[]` — **GAP (high)**
For `getsockopt` (830-839) the worker writes the capped optlen into the SLEN window and the sentry
reads it back via the rebased pointer; `accept`/`getsockname`/`recvfrom` similarly source the
in/out socklen from a `buf[]` window. The sentry does **not** re-clamp the value it reads from the
window before `service_local()` uses it as a kernel out-length. A worker that writes a large
socklen/optlen into the SLEN/OPT window makes the kernel write past the `SENTRY_OPT_OFF` /
`SENTRY_SADDR_OFF` window into the rest of `buf[]` (and, for windows near the tail, off the end).
Same class as §A — out-length sourced from attacker memory without a sentry-side bound.

### E. Validate-in-place TOCTOU against a multi-threaded worker — **GAP (high)**
The `turn` token is a **cooperative** ping-pong, not an access control. Nothing prevents *another*
worker thread (a malicious worker has many) from writing `ring[i].buf` while `turn == 1` (sentry
owns it). The sentry validates the `iovec[]` (B) and the msghdr graph (C) **in place in `buf[]`**,
writing the rebased pointers back into the same shared bytes, and then `service_local()` /the kernel
**re-reads** those same bytes at syscall time. A second worker thread can overwrite
`iv[k].iov_base`/`iov_len` **after** the bounds check but **before** the kernel reads it, escaping
the check entirely → arbitrary read/write at a worker-chosen address **in the sentry**. The scalar
registers are safe because they are snapshotted into `tmp` (424-430); the buf-resident structures
are **not** snapshotted — they are validated and consumed directly in shared memory.

Root cause for A–E: **the sentry trusts the worker-side length/count caps and validates structured
data in attacker-writable shared memory instead of copying request metadata into sentry-private
memory and validating it there.**

### F. FDPASS lends a worker-named fd with no ownership check — **GAP (critical)**
`sentry_service_one()` 414-421:

```c
if (R->rawnr == SENTRY_OP_FDPASS) {
    int idx = (int)(R - g_shm->ring);
    int fd = (int)(int64_t)R->a[0];
    sentry_send_fd(g_ctl[idx][1], fd);   // <-- fd is attacker-controlled
    R->ret = (fd >= 0) ? 0 : -EBADF;
    ...
}
```

The fd is taken straight from `R->a[0]` and the **only** validation is `fd >= 0`. A malicious worker
sends `SENTRY_OP_FDPASS` with `a[0]` = any integer and the sentry `SCM_RIGHTS`-duplicates **that**
fd into the worker: the sentry's own control socket (`g_ctl[*][1]`), the daemon stdio pipes, a host
file/socket fd the sentry opened for another lane, etc. The legitimate flow (file-backed `mmap`,
640-661) expects `a[0]` to be a previously-returned sentry fd for *this* mapping, but there is no
table of "fds this guest legitimately holds." With `DDJIT_SANDBOX` **off** (the env vars are
independent; only the daemon couples them) the borrowed fd is fully usable → **direct escape**.
With Seatbelt on, data ops are blocked, but getting the sentry's control-socket end lets the worker
desync/inject on the ring, and fd metadata/`dup`/`fcntl` may still be reachable.

### G. SCM_RIGHTS send leaks a worker-named sentry fd outward — **GAP (medium)**
sendmsg control flattening (877-884) copies the guest cmsg bytes verbatim, and the header comment
(94, 877) states the fd integers "cross verbatim" because a guest fd *is* a sentry fd. So a worker
can put **any** sentry fd number in an `SCM_RIGHTS` cmsg and `service_local()` will send that fd over
a real socket — exfiltrating a sentry-owned fd to an external peer (or back to itself via a
socketpair). Same root cause as F: **guest fd space == raw sentry fd space, with no virtualization or
ownership validation**, exactly the "design for virtual guest-fd numbers from day one" that
`sentry-split.md` §8 warned about and the implementation did not do.

### H. Seatbelt profile — **NEEDS-CHECK / partial GAP**
`worker_sandbox()` / `k_worker_sbpl` (372-402):
- **`(allow mach-lookup)` with no filter — GAP.** `sentry-split.md` §4 explicitly required
  `(deny mach-lookup (global-name-regex #".*"))` because reaching the bootstrap server / WindowServer
  is a classic macOS local-privilege escape. The shipped profile **allows all mach lookups**. Also
  `(allow mach-priv-task-port)` is unrestricted where the design specified `(target self)`. On macOS
  this is the most likely *sandbox-escape* primitive for a popped worker.
- **Fail-open — GAP.** On `sandbox_init` failure the code *warns and continues* (393-400) — the
  worker then runs **unconfined**. The comment admits production must fail closed (`_exit`).
- **Defense-in-depth only, and conditional.** Seatbelt is `#ifdef __APPLE__` only — on a Linux host
  there is **no** worker confinement at all (the promised seccomp-bpf allowlist is unimplemented).
  And it is applied only when `DDJIT_SANDBOX` is set; `DDJIT_UNTRUSTED` alone routes syscalls but
  leaves the worker able to hit the host fs/net directly.
- **Soundness caveat (documented):** deny-default is only correct once the *full* fs/net/proc set is
  forwarded; with an incomplete set, a still-local fs syscall is denied and breaks a real guest, so
  the profile is sound today only for the bundled demo. Net: **Seatbelt is not currently a
  load-bearing boundary for real images** — the sentry-side checks must stand on their own, and per
  §A–F they do not.
- The deny-default itself (file/network) **is** otherwise SOUND in shape: `(deny default)` covers
  exec, iokit, sysctl-write, etc., and the explicit file/network denies are belt-and-suspenders.

### I. Lane / ownership control words — **SOUND as a trust boundary (advisory only)**
`ring_for_thread()` CAS-claims `owner` (186-201); `ring_release()`/`sentry_fork_child()` manage it.
A worker **cannot** be stopped from writing any `ring[i]` regardless of `owner` (shared memory), but
all lanes in one shm belong to the **same untrusted principal** (§1), so "stealing a sibling's lane"
crosses no privilege boundary. Likewise `g_shm->quit` is worker-writable → a worker can kill its own
sentry (self-DoS, equivalent to exiting). The owner-gating in `sentry_shutdown()` (567) and the
fork-child re-identification (219-224, 612) are **correctness** mechanisms (don't tear down the
shared sentry under live siblings), not security controls, and are robust for that purpose. The only
way lane confusion escalates is by driving the OOB primitives of §A/B/E into sentry memory — i.e.
the escalation lives in A/B/E, not here.

### J. fork/exec/wait lanes — **SOUND (no privilege gradient)**
`clone` fork (608-615) runs `service_local()` then re-identifies the child via `getpid() !=
g_worker_pid`. A forked child gets `sentry_fork_child()` → fresh token, fresh lane, **not** the
sentry owner, so it cannot tear down the shared sentry. `wait4` (625-635) short-circuits wait-any
with no guest children to `-ECHILD` and never surfaces the sentry pid. A forked worker is still the
same untrusted principal, so even if it forged ownership it would only DoS itself. The mechanism is
sound; the residual concern is the **shared fd table** across forked workers (§K).

### K. Residual gaps — security ratings
- **Shared sentry fd table across forked workers** (roadmap 1133-1135): all forked workers of one
  guest share the sentry's fd table. Same principal, so no cross-tenant impact — but it is a
  **confused-deputy / correctness** hazard (one guest process can `close`/`dup` over another's fd) and
  it compounds F/G (no per-process fd ownership to validate against). Security impact: **LOW**
  (intra-principal), but it is the missing structure that would *fix* F/G. Rate **MEDIUM** as an
  enabler.
- **execve image read in-worker** (618-621): `execve` stays local and `service_local()` reloads the
  ELF using direct host syscalls — which a deny-default Seatbelt worker **cannot** perform (it has no
  fs read). So sandbox + execve are mutually exclusive today: either execve is broken under the
  sandbox, or the worker has enough fs reach to read images (a hole). Security impact: **MEDIUM**
  (functionality/security tension; currently a reason the sandbox is not generally usable).
- **Futex/`__ulock` still a spin** (1149): perf only. A worker can burn CPU, but running untrusted
  compute already permits that. Security impact: **LOW**.
- **Incomplete forwarded set**: any still-local fs/net syscall under `DDJIT_UNTRUSTED` without a
  working Seatbelt hits the host directly. Until the set is complete *and* Seatbelt is load-bearing,
  `DDJIT_UNTRUSTED` is **not** a containment boundary on its own. **HIGH** as a deployment caveat.

---

## 4. Verdict on the bounds-checking

**The bounds-checking has critical gaps.** It is *not* sound to treat the ring as fully hostile
today:

- Offsets are checked (`redir[i] < SENTRY_BUFSZ`), and the **per-iovec-segment** length is checked
  (`boff+len <= BUFSZ`) — those two are SOUND.
- But the **scalar lengths** (`a[2]`/`a[4]`, socklen/optlen windows) are **never clamped** to the
  remaining window (§A, §D), the **segment/iov counts** (`iovn`, msghdr `in`) are **never bounded**
  (§B, §C), the iovec **base** is dereferenced without confirming `redir[1]` was validated (§B), and
  all structured data is **validated in place** in attacker-writable memory and re-read by the kernel
  (§E, TOCTOU). The FDPASS/SCM_RIGHTS paths perform **no fd-ownership validation** (§F, §G).

So the sentry does **not** yet treat all ring fields as hostile: it trusts the worker-side caps for
every length/count and validates buffers in shared memory. Under the design's own stated threat model
(arbitrary native code in the worker), a malicious worker has an OOB read (info leak) and OOB write
(memory corruption) into the **trusted sentry**, plus the ability to pull arbitrary host fds into
itself — i.e. the split does not currently contain a fully-compromised worker.

---

## 5. Prioritized hardening checklist

**P0 — close the memory-safety holes in `sentry_service_one()` (without these the split is not a
boundary):**
1. **Clamp every length to its window, sentry-side.** For each redirected scalar arg, require
   `len <= SENTRY_BUFSZ - off` (read/write/recvfrom/sendto `a[2]`/`a[4]`); reject otherwise. Re-derive
   the length from the redir window, never trust `R->a[i]` as the kernel length without a bound.
2. **Bound `iovn` and msghdr `in`** to `(window - hdr)/sizeof(struct iovec)` and to `SENTRY_IOVMAX`
   **inside the sentry**, before the rebase loops (442, 470). Reject if `redir[1] < 0` while
   `iovn > 0`.
3. **Clamp `msg_namelen`, `msg_controllen`, and all socklen/optlen out-lengths** read from `buf[]`
   to their window sizes sentry-side (§C, §D).
4. **Eliminate the validate-in-place TOCTOU (§E):** copy the request metadata (the `iovec[]` array,
   the 56-byte msghdr, sockaddr/optlen windows) into **sentry-private** memory, validate the copy,
   and point `service_local()` at the private copy — never let the kernel re-read attacker-writable
   shared memory after the check. (Snapshotting scalars is already done; extend it to structures.)
5. **Guard the last ring:** place a guard page after `ring[SENTRY_NRINGS-1].buf` (or per-ring) so any
   residual OOB faults the *sentry* deterministically instead of corrupting it, and size the mapping
   accordingly.

**P1 — fd capability model:**
6. **Validate FDPASS (§F):** keep a per-lane/per-guest set of fds the sentry has legitimately handed
   this guest; reject `SENTRY_OP_FDPASS` for any fd not in that set. Never lend `g_ctl[*]`, the daemon
   pipes, or another lane's fds.
7. **Virtualize guest fds (§G):** translate between a per-guest virtual fd space and real sentry fds
   (the `sentry-split.md` §8 "virtual guest-fd numbers" plan), and validate every fd in an outbound
   `SCM_RIGHTS` cmsg against that space before `service_local()` sends it.

**P2 — confinement / fail-closed:**
8. **Fix the Seatbelt profile:** restore `(deny mach-lookup (global-name-regex #".*"))` and scope
   `mach-priv-task-port` to `(target self)` (§H).
9. **Fail closed:** make `sandbox_init` failure `_exit()` in the untrusted path; refuse to start the
   worker unconfined.
10. **Couple the gates / land Linux seccomp:** treat `DDJIT_UNTRUSTED` without a working confinement
    layer as unsupported (the worker can reach the host directly); implement the promised seccomp-bpf
    allowlist for the Linux host.

**P3 — structural / residual:**
11. **Per-process sentry fd tables** for forked workers (§K) — also the substrate for P1.
12. Resolve the **execve image-read** tension (read the new image *through the sentry* over the ring,
    not via direct host syscalls in a confined worker).
13. Replace the `turn` spin with a process-shared futex (perf; no security impact).

Until **P0** lands, the sentry should be considered an *organizational* split (defense-in-depth
against an *un-compromised* worker), **not** a security boundary that contains a fully-malicious
worker — which is its stated purpose. The split is structurally correct (right resources on the right
side, no guest pointer dereferenced, sound ping-pong ordering, sound lane/fork model); the gap is
that the sentry under-validates the hostile ring.
