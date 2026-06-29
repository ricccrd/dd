# W6B — Untrusted-guest isolation via a sentry process-split (PoC)

**Engine:** jit86 (x86-64 guest → ARM64 host, `targets/linux_x86_64.c`).
**Gate:** `g_untrusted` (env `DDJIT_UNTRUSTED` or trigger file `runtime/jit86/UNTRUSTED`). OFF by default.
**Seatbelt gate:** `g_sentry_sandbox` (env `DDJIT_SANDBOX`).
**Tree:** `/Users/x/w6b-sentry` (built+run via `mac`). Mirror: `scratchpad/opt-work/w6b-sentry`. Diff: `scratchpad/opt-reports/w6b-sentry.diff`.
**Status:** read/write/open family works end-to-end through the sentry ring, **byte-identical** to the local path; ring overhead **~375 ns / forwarded syscall**; deny-default Seatbelt implemented + proven to deny. Trusted path (`g_untrusted` off) is byte-identical to baseline by construction.

---

## 1. Trust boundary + authority split

The entire guest→host authority crossing is one call: `run_guest()` → `service(c)` in `os/linux/service.c`.
A malicious translated guest can forge arbitrary register state at a syscall, so `service()` is the single
point that turns guest intent into real host effects. The PoC routes that call through a new
`syscall_route(c)` (the only edit to the hot loop: `dispatch.c` line 147) and splits `service()` by
**authority across a process boundary**:

| Process | Trust | Syscalls it executes | Host objects it holds |
|---|---|---|---|
| **WORKER** (this process) | untrusted (runs the JIT + translated guest) | compute/memory only: `brk`, anon `mmap`, `futex`, `clock_*` (inline fast-paths), `exit`, TLS `arch_prctl`, signal/sched bookkeeping | none — **no real fs/net fds** |
| **SENTRY** (forked child) | trusted (the policy authority) | **fs/net/proc**: forwarded over the ring, executed via the real `service()` (path-jail, `/proc` synth, overlay all preserved) | the real fd table, sockets, host fs |

**Forwarded set in this PoC** (canonical aarch64 numbers, after `x86_normalize`):
`openat(56)`, `close(57)`, `lseek(62)`, `read(63)`, `write(64)` — the read/write/open family.
Everything else falls through to the local in-worker `service(c)` unchanged.

Key isolation property: **guest memory is never shared.** Only marshaled bytes cross the ring, so the
sentry never dereferences a worker-controlled pointer into guest memory, and the worker never receives a
real host fd (an `openat` result is an integer that is only meaningful inside the sentry; subsequent
`read`/`write`/`close` on it are *also* forwarded, so the fd lives and dies entirely in the sentry).

Routing is faithful to the existing normalization contract: `syscall_route` runs `G_NORMALIZE(c)` once
(so legacy x86 `open`→`openat` etc. are classified by canonical number; a `return 1` like `arch_prctl`
stays local exactly as `service()` does), then forwards or delegates. The sentry's `service()` re-runs
`G_NORMALIZE` as a no-op on the already-`*at` registers, so the executed semantics are identical.

## 2. SPSC ring protocol (`os/linux/sentry.c`)

Transport is a single `MAP_SHARED|MAP_ANON` region created before `fork()`:

```c
struct sentry_ring {
    _Atomic uint32_t turn;   // 0 = worker owns (builds request); 1 = sentry owns (executes)
    _Atomic uint32_t quit;   // worker sets at teardown -> sentry _exit()s
    uint64_t rax,rdi,rsi,rdx,r10,r8,r9; // normalized x86 syscall registers (request)
    int32_t  bufreg;         // guest-ptr arg redirected into buf[] (6 = rsi/a1) or -1
    uint32_t inlen, outcap;  // input bytes present / max output bytes to copy back
    int64_t  ret;            // response: syscall return or -errno
    uint32_t outlen;         // response: valid output bytes in buf[]
    uint64_t nserved;        // sentry counter (measurement / leak diagnostic)
    uint8_t  buf[1<<20];     // 1 MiB inline payload
};
```

- **Layout / ownership.** `turn` is the single ownership token; strict ping-pong (no third state) ⇒
  deadlock-free. Worker fills the request fields, then `store(turn,1,release)`; sentry spins on
  `load(turn,acquire)==1`, executes, fills the response, then `store(turn,0,release)`. The release/acquire
  pair makes every field write *happen-before* the peer's read of the token ⇒ **no torn messages**.
- **Marshaling.** The worker ships the 7 normalized registers. For the one guest pointer in this family
  (always `a1`=`rsi`): `openat` copies the NUL-terminated path in; `write` copies `min(len,1MiB)` payload
  bytes in (and caps `rdx` so the sentry writes exactly what was shipped — a short write is legal and the
  guest libc loops); `read` reserves `outcap` and the sentry's `read()` lands directly in `buf[]`, which
  the worker copies back into guest memory on return. `lseek`/`close` carry no buffer. The sentry rebuilds
  a `struct cpu`, redirects `r[6]` to `&buf[]` (the **only** crossing point), and calls the real
  `service()`.
- **fd passing (SCM_RIGHTS).** Not required for this family because real fds stay in the sentry. It is the
  documented mechanism for the next stage (a guest fd that must be used by a *local* worker syscall, e.g.
  file-backed `mmap`): the sentry would `sendmsg` the fd over a `socketpair` with `SCM_RIGHTS` so the
  worker holds a kernel-validated dup. A control `socketpair` is the natural home for this and for a
  futex-style wakeup; the PoC keeps the data path purely in shared memory.
- **Backpressure.** Guest syscalls are synchronous, so the channel is effectively **1-deep**: the worker
  blocks (spins) until the response lands. Backpressure is therefore implicit — there is never more than
  one in-flight request per ring. A multi-slot head/tail ring (depth>1) is only useful once async/batched
  syscalls or multiple producers exist (see §7).

## 3. PoC diff

Full unified diff: `scratchpad/opt-reports/w6b-sentry.diff` (+259 new `sentry.c`, +12 in `linux_x86_64.c`,
1 line in `dispatch.c`). Shape:

```diff
--- a/src/runtime/frontend/x86_64/dispatch.c
+++ b/src/runtime/frontend/x86_64/dispatch.c
@@ run_guest loop @@
-            service(c);
+            syscall_route(c); // untrusted-guest isolation seam (g_untrusted): pass-through when off

--- a/src/runtime/targets/linux_x86_64.c
+++ b/src/runtime/targets/linux_x86_64.c
@@ includes @@
+#include "../os/linux/sentry.c"   // SPSC ring + sentry split (g_untrusted)
@@ engine_global_init @@
+    g_untrusted      = getenv("DDJIT_UNTRUSTED") != NULL || <trigger file>;
+    g_sentry_sandbox = getenv("DDJIT_SANDBOX")   != NULL || <trigger file>;
@@ run_loaded @@
+    if (g_untrusted) sentry_init();     // fork host-authority sentry (+ optional worker Seatbelt)
     run_guest(&c);
+    if (g_untrusted) sentry_shutdown(); // signal quit + waitpid (reap, no orphan)
```
`sentry.c` adds: the ring struct, `sentry_forwarded()` (authority classifier), `worker_sandbox()`
(Seatbelt), `sentry_loop()` (child body), `sentry_init`/`sentry_shutdown`, and `syscall_route()`.

## 4. Proof: end-to-end byte-identical + measured overhead

Demo guest `sandbox/guest_io.c` (raw-syscall static-pie x86-64, no libc): `openat` a file → `read` it in
64-byte chunks → `write` to stdout → `openat`+`write` a copy in 32-byte chunks → `close`×2. Run OFF vs ON:

```
TRUSTED  (g_untrusted OFF):  stdout + out.txt  ─┐
UNTRUSTED(g_untrusted ON ):  stdout + out.txt  ─┴─ diff: STDOUT byte-identical, OUTFILE byte-identical
                                                    out.txt == in.txt  (full open/read/write roundtrip via sentry)
[sentry] forwarded 14 syscalls; sentry reaped
```

**Stress** (`sandbox/guest_stress.c`, 52 800 B copied at 17-byte reads / 13-byte writes ≈ 9 323 forwarded
syscalls/run, 5 runs): every run `cmp`-identical to input, **deterministic 9 323 forwarded each run**
(no lost/torn/duplicated messages), zero orphan processes afterward.

**Ring overhead** (`sandbox/guest_bench.c`, 1 000 000 `lseek` = pure hop, no buffer copy):

| mode | wall (1M syscalls) | per-syscall |
|---|---|---|
| trusted (local `lseek`) | 0.20 s | ~200 ns |
| untrusted (via sentry ring) | 0.575 s (avg of 3) | ~575 ns |
| **ring round-trip overhead** | **+0.375 s** | **≈ 375 ns / forwarded syscall** |

375 ns is the cost of the IPC hop (two cache-line ownership transfers + occasional `sched_yield`). For a
security/isolation feature this is the expected, acceptable order of magnitude; `write`/`read` add a
`memcpy` of the payload on top (negligible for small buffers, memory-bandwidth-bound for large).

## 5. Seatbelt deny-default — IMPLEMENTED + proven

`worker_sandbox()` calls `sandbox_init()` with a deny-default SBPL profile after the sentry forks, so the
worker can reach the host **only** through the sentry. Verified two ways:

1. **Profile denies** (standalone `sandbox/sbtest.c`, exact profile string):
   `open("/etc/hosts")` before = fd 3; `sandbox_init` = 0; after = -1 **EPERM**. File reads/writes and
   network are denied.
2. **Worker confined, guest still works**: `DDJIT_UNTRUSTED=1 DDJIT_SANDBOX=1 ./ddjit-x86 guest_io`
   prints `[sentry] worker confined under deny-default Seatbelt profile`, output stays byte-identical, and
   `out.txt == in.txt`. The worker did **zero** direct host fs/net — all I/O flowed through the sentry.

**Soundness caveat (why it stays gated):** deny-default is only safe once the *full* fs/net/proc set is
forwarded. With only the read/write/open family forwarded, any still-local fs syscall (`getdents64`,
`newfstatat`, `readlinkat`, …) would be denied and break a general guest. So `DDJIT_SANDBOX` is OFF by
default and is currently sound exactly for guests whose entire syscall surface is the forwarded family
(the bundled demo). Completing the forwarded set (§7) makes it sound in general.

## 6. Correctness / safety / non-regression

- **No deadlock / no torn messages:** strict `turn` ping-pong with release/acquire; 5×9 323-RPC stress is
  bit-exact and count-deterministic.
- **No fd / process leak:** `exit`/`exit_group` is intercepted in `syscall_route` to `sentry_shutdown()`
  (signal `quit` + `waitpid`) *before* `service()` calls `_exit()` — otherwise the worker's direct
  `_exit` would orphan the spinning sentry. Defense-in-depth: the sentry's spin loop checks
  `getppid()==1` and exits if reparented (covers a *crashed* worker). After deliberately killing a hung
  worker, `ps` showed **NONE** leaked. `sentry_shutdown` clears the pid so the exit-path and `run_loaded`
  teardown can't double-reap.
- **Trusted-path non-regression:** with `g_untrusted` off, `syscall_route` is a one-branch pass-through to
  `service(c)`; `sentry_init/shutdown` are not called; the only other added code is two `getenv/access`
  probes in `engine_global_init`. Byte-identical by construction, and verified: busybox `sh` under
  `--rootfs alpine` (echo / redirect / cat / `ls /bin`) produces identical output with the patched binary
  (off) as baseline.

## 7. Status + next steps to full untrusted isolation

**Done:** SPSC ring transport; read/write/open family forwarded end-to-end byte-identically; ~375 ns/hop
measured; deny-default Seatbelt implemented + proven; leak/deadlock-safe teardown; trusted path
non-regressing.

**Next (in priority order):**
1. **Per-context rings** — the ring is single-producer. A forked guest (`busybox sh`) spawns a second
   worker process that contends on the one mailbox and stalls. Give each worker thread/process its own
   ring + a sentry willing to service N rings (or one sentry thread per ring). This is the single biggest
   gap to running real multi-process guests under isolation. (Observed: `busybox sh` under untrusted
   prints its first forwarded `write` then stalls at the first `clone` — exactly this boundary.)
2. **Complete the fs/net/proc forwarded set** — two-buffer marshaling for `newfstatat`/`fstat`/`statx`,
   iovec for `readv`/`writev`, `getdents64`, `pread`/`pwrite`, then `socket`/`bind`/`connect`/`send`/`recv`
   and `execve`/`clone(fork)`/`wait4`. Once complete, flip `DDJIT_SANDBOX` on by default under
   `g_untrusted` (it becomes sound for any guest).
3. **SCM_RIGHTS fd passing** — for guest fds that a *local* worker syscall must touch (file-backed `mmap`):
   sentry `sendmsg`-es the fd to the worker over a control `socketpair`.
4. **Futex/`__ulock` wakeup** — replace the spin (busy-waits a core while the sentry blocks in a real
   `read`) with a process-shared futex on `turn`; lowers idle cost and CPU during blocking I/O.
5. **Sentry-side policy** — today the sentry runs the full `service()`; add an allow/deny policy layer
   (path allowlists, net egress rules) so the sentry *enforces* rather than merely *executes*.
6. **Multi-slot ring** — only once async/batched syscalls exist (depth>1 is unused while syscalls are
   synchronous).
```
