# Fix: signal / timer / pipe edge bugs

Scope: three `edge`-group correctness bugs in the Linux→macOS service layer
(`dd-jit/src/runtime/os/linux/service.c`). The first two are reliability bugs
(one hangs the guest, one kills the process); the third is a feature-fidelity
gap that is partly host-limited.

This is a read-only analysis written against an in-flight `service.c` change —
line numbers are as of that snapshot. Where the in-flight code already lands the
fix, this doc records *why* it is correct and flags the residual gap.

Priority order: (1) `TIMER_ABSTIME` hang and (2) `MSG_NOSIGNAL` SIGPIPE are real
correctness/reliability bugs and must ship. (3) `pipe2(O_DIRECT)` is mostly
host-limited; `F_SETPIPE_SZ`/`F_GETPIPE_SZ` are cheaply emulatable and done.

---

## Bug 1 — `clock_nanosleep(TIMER_ABSTIME)` treated as relative → hang

### (a) Failing case
- Test: `dd-tests/guests/edge_clockabstime.c:14`
  ```c
  int rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
  ```
  `deadline = now(CLOCK_MONOTONIC) + 40ms`. The guest expects to wake ~40 ms
  later and asserts `rc == 0 && 30ms <= elapsed < 2000ms`
  (`edge_clockabstime.c:17`).
- Bug: with `TIMER_ABSTIME` ignored, the host sleeps for the *absolute* timespec
  — i.e. `uptime + 40ms` worth of seconds (thousands of seconds) — and the guest
  hangs until the 25 s harness timeout fires (comment at `edge_clockabstime.c:4`).
- Handler: syscall 115 (`clock_nanosleep`) at `service.c:2152`.

### (b) macOS primitive
macOS has **no `clock_nanosleep(2)`** at all (no absolute-deadline sleep, no
per-clock sleep). The only host primitives are `nanosleep(2)` (always relative,
`CLOCK_MONOTONIC`-ish) and `clock_gettime(2)` (which macOS *does* have, used at
`service.c:2134`). So `TIMER_ABSTIME` must be emulated by converting the absolute
deadline to a relative duration.

### (c) Fix
At `service.c:2152-2178` (already in the in-flight change):

1. Read `flags = a1`; if `flags & 1` (`TIMER_ABSTIME`):
   - Map the Linux `clockid` (a0) to a macOS clock — same translation table as
     syscall 113 (`service.c:2120`): `0/5 → REALTIME`, `1/4/6/7 → MONOTONIC`,
     `2 → PROCESS_CPUTIME`, `3 → THREAD_CPUTIME` (`service.c:2160-2165`).
   - `clock_gettime(mc, &now)`; compute `d = req - now` with nsec borrow
     (`service.c:2168-2170`).
   - If `d > 0`, `nanosleep(&d, NULL)` (`service.c:2171`); return 0 (an absolute
     sleep reports no remainder — `service.c:2172`). If `d <= 0` the deadline is
     already past → return 0 immediately without sleeping (correct: matches Linux,
     which returns 0).
2. Relative path (`flags & 1 == 0`): `nanosleep(req, remain=a3)` — the host
   remainder layout matches Linux `struct timespec`, so it drops straight into
   `a3` (`service.c:2175`).

This is the right shape and fixes the hang.

**Residual gap to close (recommended):** the ABSTIME path does **not loop on
`EINTR`**. If a signal interrupts the host `nanosleep`, it returns early with a
remainder, the code ignores it and returns 0 — the guest under-sleeps silently.
Linux `clock_nanosleep(TIMER_ABSTIME)` instead keeps sleeping (or returns
`EINTR` for the guest to re-arm against the *same* absolute deadline). Make the
ABSTIME branch a loop that re-reads `now` and recomputes `d` each iteration:

```c
for (;;) {
    clock_gettime(mc, &now);
    d.tv_sec  = req->tv_sec  - now.tv_sec;
    d.tv_nsec = req->tv_nsec - now.tv_nsec;
    if (d.tv_nsec < 0) { d.tv_sec--; d.tv_nsec += 1000000000L; }
    if (d.tv_sec < 0 || (d.tv_sec == 0 && d.tv_nsec <= 0)) break; // deadline passed
    if (nanosleep(&d, NULL) == 0) break;
    if (errno != EINTR) break;   // recompute against the absolute deadline and retry
}
G_RET(c) = 0;
```

Recomputing `now` each pass (rather than feeding `nanosleep`'s own remainder
back) is what makes it *absolute*: it is immune to scheduling slop accumulated
across wakeups. The relative path already gets EINTR semantics for free because
it writes the remainder to `a3` and returns, exactly like Linux.

The 40 ms test passes with or without the loop (no signal is delivered), but the
loop is needed for guests that nanosleep under signal load.

### (d) Test gate
- `dd-tests/guests/edge_clockabstime.c` → prints `clockabstime abstime_ok=1`.
- Golden/cross-engine (deterministic; the 25 s harness timeout is itself the
  regression catch for a re-introduced relative-interpretation hang —
  `edge_clockabstime.c:4`).

---

## Bug 2 — `MSG_NOSIGNAL` ignored → fatal SIGPIPE instead of `EPIPE`

### (a) Failing case
- Test: `dd-tests/guests/edge_sigpipe.c:17`
  ```c
  signal(SIGPIPE, SIG_DFL);                  // line 15: keep SIGPIPE fatal
  ssize_t n = send(sv[1], "data", 4, MSG_NOSIGNAL);   // peer (sv[0]) already closed
  int epipe = (n < 0) && (errno == EPIPE);
  ```
  Guest expects `send` to return `-1/EPIPE` and deliver **no** signal, then
  print `sigpipe survived=1 epipe=1` (`edge_sigpipe.c:20`).
- Bug: if `MSG_NOSIGNAL` is dropped, the write to a peer-closed socket raises
  SIGPIPE; since the guest deliberately left SIGPIPE at `SIG_DFL`, the process
  dies and *prints nothing* — the missing output line is the failure signal.
- `send(2)` is glibc sugar over `sendto(2)` = syscall 206 (`service.c:2425`).
  Same flag must also be honored on `sendmsg` (211, `service.c:2479`) and the
  `sendmmsg` fan-out (269/243, `service.c:2506`).

### (b) macOS primitive
Linux carries SIGPIPE suppression **per call** via the `MSG_NOSIGNAL`
(`0x4000`) send flag. macOS `send`/`sendto`/`sendmsg` have **no `MSG_NOSIGNAL`**
flag. The macOS mechanism is the **`SO_NOSIGPIPE` socket option** (`SOL_SOCKET`),
which is a *sticky per-socket* property: once set, every send on that fd returns
`EPIPE` instead of raising SIGPIPE. (macOS also has a `F_SETNOSIGPIPE` fcntl for
non-socket fds, not needed here.)

Per-call vs sticky is a minor semantic mismatch — setting `SO_NOSIGPIPE` makes
suppression persist for later sends on the same fd that did *not* pass
`MSG_NOSIGNAL`. In practice harmless: programs that ask for it once almost always
want it for the socket's lifetime, and nothing in the guest can observe the
option flipping. The alternative (block SIGPIPE around the single send via
`pthread_sigmask` + drain) is heavier and races with async signal delivery; the
sockopt is the correct, idiomatic macOS approach.

### (c) Fix
Translate `MSG_NOSIGNAL` → `SO_NOSIGPIPE` on the target fd just before the host
send, then strip it from the flags passed to the host (it isn't a valid macOS
send flag). Already in the in-flight change:

- `sendto` (206): `service.c:2428`
  ```c
  if ((int)a3 & 0x4000) { int on = 1; setsockopt((int)a0, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on); }
  ```
  then `sendto(..., msgflags_l2m((int)a3), ...)` (`service.c:2429`). Verified:
  `msgflags_l2m` (`dd-jit/src/runtime/os/linux/container/netns.c:57`) is a
  whitelist that maps only OOB/PEEK/DONTROUTE/TRUNC/DONTWAIT/EOR/WAITALL —
  `0x4000` is **not** in it, so MSG_NOSIGNAL is silently dropped and never passed
  to the host as an (invalid) send flag. Good.
- `sendmsg` (211): `service.c:2494` (same setsockopt, guarded on `nr == 211`).
- `sendmmsg` (269) / `recvmmsg` (243): `service.c:2506-2533` loop calls
  `sendmsg(..., msgflags_l2m(rf))`. **Gap:** this path does *not* set
  `SO_NOSIGPIPE` from `a3 & 0x4000`. Add the same one-shot setsockopt before the
  loop when `nr == 269 && (a3 & 0x4000)`:
  ```c
  if (nr == 269 && ((int)a3 & 0x4000)) { int on = 1; setsockopt((int)a0, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on); }
  ```
  (the test only exercises `sendto`, but `sendmmsg(MSG_NOSIGNAL)` is real and
  used by some runtimes).

Why this is sufficient for the test: the guest's `signal(SIGPIPE, SIG_DFL)` maps
to a host `signal(sig_l2m(SIGPIPE), SIG_DFL)` (`service.c:2058-2059`), i.e. host
SIGPIPE stays fatal — there is **no** global `SIG_IGN` masking the bug
(confirmed: no global SIGPIPE ignore anywhere in `dd-jit/src`, only the
guest-driven `SIG_IGN` path at `service.c:2061`). With `SO_NOSIGPIPE` set on the
socket, the host `sendto` never raises SIGPIPE and returns `-1/EPIPE`, which
`service.c:2430` forwards as `-errno`. Guest sees `EPIPE`, survives, prints the
verdict.

### (d) Test gate
- `dd-tests/guests/edge_sigpipe.c` → prints `sigpipe survived=1 epipe=1`
  (absence of the line == SIGPIPE killed the guest == regression).
- Golden/cross-engine (deterministic — `edge_sigpipe.c:4`).

---

## Bug 3 — `pipe2(O_DIRECT)` packet mode + `F_SETPIPE_SZ`/`F_GETPIPE_SZ`

Two independent corners. One is cheaply emulatable; one is host-limited.

### 3a. `F_SETPIPE_SZ` / `F_GETPIPE_SZ` — FIXABLE (done)

**(a) Failing case** — `dd-tests/guests/edge_pipesz.c:18-20`:
```c
int set = fcntl(fds[1], F_SETPIPE_SZ, 256*1024);
int got = fcntl(fds[1], F_GETPIPE_SZ);
int size_ok = (set >= 0) && (got >= 256*1024);   // SET must succeed, GET must report >= requested
```
(Same test also checks `dup3(fd, fd, 0) == EINVAL` — covered by Bug-list
neighbor, handled at `service.c:404-408`.)

**(b) macOS primitive** — none. macOS pipes have a fixed kernel buffer and **no
`F_SETPIPE_SZ`/`F_GETPIPE_SZ` fcntl** (those Linux cmd numbers 1031/1032 don't
exist). But the guest only needs the value to *stick and round up* — it never
checks that the kernel actually buffers 256 KiB.

**(c) Fix** — emulate with a per-fd shadow table. Done at `service.c:476-492`
(table declared `service.c:25`, `static int g_pipesz[1024]`):
- `F_SETPIPE_SZ` (1031): round the requested size up to a page, clamp to `>= 1
  page`, store in `g_pipesz[fd]`, return the rounded size (`service.c:478-486`).
- `F_GETPIPE_SZ` (1032): return `g_pipesz[fd]` if recorded, else a sane default
  `65536` (Linux's default pipe size — `service.c:488-491`).

Honest scope: this is a **cosmetic/contract emulation** — actual host pipe
capacity is unchanged, so a guest that relies on a 256 KiB pipe to avoid blocking
across two threads still blocks at the real macOS pipe limit. Acceptable: such
fine capacity dependence is rare, and the alternative (swapping the pipe for a
larger-buffered socketpair) breaks `S_ISFIFO`/pipe semantics (see 3b). Leave as
the value-emulation.

**(d) Test gate** — `edge_pipesz.c` → `pipesz size_ok=1 dup3_einval=1`
(diffed vs native → oracle, `edge_pipesz.c:3`).

### 3b. `pipe2(O_DIRECT)` packet mode — HOST-LIMITED

**(a) Failing case** — `dd-tests/guests/edge_pipepacket.c:11-17`:
```c
pipe2(fds, O_DIRECT);
write(fds[1], "AAA", 3);  write(fds[1], "BBBBB", 5);
ssize_t n1 = read(fds[0], b1, 64);   // packet mode: exactly 3, NOT 8 coalesced
ssize_t n2 = read(fds[0], b2, 64);   // exactly 5
```
Expects `n1=3 n2=5` with message boundaries preserved. Handler: `pipe2` = syscall
59 at `service.c:1186`; it parses `O_CLOEXEC`/`O_NONBLOCK` but **ignores
`O_DIRECT` (`0x4000`)** (`service.c:1193-1201`), so the two writes coalesce in
the host pipe's byte stream and a single `read` returns 8.

**(b) macOS primitive** — none. macOS pipes are pure byte streams with **no
`O_DIRECT`/packet mode**; there is no flag or fcntl that imposes write-boundary
framing on a pipe. So O_DIRECT packet semantics are **not emulatable on a real
macOS pipe**.

**(c) Decision — host-limited; O_DIRECT silently ignored (stream mode).**
Justification:
- There is no macOS pipe facility that frames writes. The guest gets a working
  byte-stream pipe; only the rarely-used packet framing is lost.
- A **partial emulation exists but is not worth its fidelity cost**: when
  `O_DIRECT` is requested, back the pipe with `socketpair(AF_UNIX,
  SOCK_SEQPACKET, 0, sv)` instead of `pipe()`. `SOCK_SEQPACKET` *does* preserve
  message boundaries on macOS, so `n1=3 n2=5` would pass. But this changes the
  fd's identity in observable ways:
  - `fstat` reports `S_ISSOCK`, not `S_ISFIFO` — guests that special-case FIFOs
    (and Linux `O_DIRECT` pipes still stat as FIFOs) would misbehave.
  - The endpoints become bidirectional and lose pipe-specific `PIPE_BUF`
    atomicity / `EPIPE`-on-read-end-closed nuances.
  - `F_SETPIPE_SZ` shadowing (3a) and the eventfd/pipe bookkeeping elsewhere
    assume real pipes.
  Swapping in a socketpair to satisfy one narrow corner risks regressing the
  common, correct stream-pipe path used everywhere. **Not worth it.**
- Real-world blast radius is small: `O_DIRECT` pipes are niche (a handful of
  message-framing libraries). Failure mode is *graceful degradation* to a stream
  pipe — data is intact, only boundaries merge — not a crash or hang.

Recommendation: leave `service.c:1186` as-is (ignore `O_DIRECT`), and **document
it on the platform-limitations list in `docs/PLAN.md`** next to the existing
"Host-limited" entries. If a real guest is later found to depend on packet
framing, revisit the `SOCK_SEQPACKET` swap behind an `O_DIRECT`-only branch with
the fstat caveat understood.

**(d) Test gate** — `edge_pipepacket.c` is **xfail** (expected-fail / oracle-diff,
not golden — `edge_pipepacket.c:3`). It documents the divergence rather than
gating CI green; if the `SEQPACKET` swap is ever adopted it flips to XPASS.

---

## Summary

| # | Bug | macOS primitive | Disposition | Gate |
|---|-----|-----------------|-------------|------|
| 1 | `clock_nanosleep(TIMER_ABSTIME)` hang | none (`clock_gettime` + relative `nanosleep`) | **Fixed** `service.c:2152`; add EINTR loop | `edge_clockabstime.c` golden |
| 2 | `MSG_NOSIGNAL` → SIGPIPE | `SO_NOSIGPIPE` sockopt | **Fixed** `service.c:2428/2494`; add to `sendmmsg` 269 | `edge_sigpipe.c` golden |
| 3a | `F_SETPIPE_SZ`/`GETPIPE_SZ` | none (no pipe-size fcntl) | **Fixed** (value-emulation) `service.c:476` | `edge_pipesz.c` oracle |
| 3b | `pipe2(O_DIRECT)` packet mode | none (no pipe framing) | **Host-limited** — ignore O_DIRECT, document | `edge_pipepacket.c` xfail |

Action items beyond the in-flight change:
1. ABSTIME: wrap the `nanosleep` in an EINTR-recompute loop (`service.c:2158-2173`).
2. MSG_NOSIGNAL: extend the `SO_NOSIGPIPE` shim to `sendmmsg` (269) at
   `service.c:2506`.
3. (verified — no action) `msgflags_l2m` (`netns.c:57`) already strips `0x4000`.
4. Add `pipe2(O_DIRECT)` to the host-limited list in `docs/PLAN.md`.
