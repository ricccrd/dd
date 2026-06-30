# Untrusted-guest isolation — the sentry trust boundary

Forward design for sandboxing **untrusted** images. Today `dd` runs the guest, services its syscalls,
and holds the host fd table / rootfs / network all **in one process**: a guest that pops a JIT or
syscall-handler bug gets the full ambient authority of the host process. That is fine and fast for
**trusted** images (our base images, the test matrix) — keep it. Untrusted images (`docker run
some/random-image`) need a real boundary. The split is **opt-in per container** (flag/`DD_SANDBOX` /
`DDJIT_UNTRUSTED`); when off, the in-process path is byte-for-byte unchanged (this is what keeps the
matrix green and the engine un-regressed).

## The invariant (the whole point)

**The trust boundary IS the process boundary.** Two OS processes with different privilege:

- **SANDBOX** (untrusted): the JIT + the guest's translated code + the guest's memory + the MAP_JIT
  code cache. Locked down (macOS Seatbelt **deny-default**; Linux seccomp-bpf allowlist). Holds **no**
  host fds, **no** filesystem, **no** sockets, **no** `execve`. A full compromise lands in a sealed,
  empty process with one pipe out.
- **SENTRY** (trusted): owns the host fd table, rootfs overlay/`confine`, netns + port-map, real
  `execve`, container state. Services the sandbox's syscall requests over a shared-memory RING.

**The sentry must never dereference a guest pointer.** Pointer args are marshalled (the pointed-to
bytes copied into a shared bounce arena, the pointer replaced by an `(offset,len)`); the sentry touches
only the arena. Sandbox fds are **virtual** integers resolved by the sentry — so `SCM_RIGHTS` becomes
"register a virtual fd," never "install a host fd in the sandbox." Design for virtual guest-fds from
day one.

## Shape (gVisor-like) and the seam

`service(c)` splits by *what authority a case needs*: `service_local` (touches only guest memory /
CPU state — `brk`, anon `mmap`, futex, clocks, signals, thread spawn) stays in the sandbox, zero IPC;
`service_remote` (host fd / fs / net / process lifecycle) is marshalled to the sentry. The **one seam**
is `dispatch.c`'s `service(c)` → `syscall_route(c)`: `if (!g_untrusted) service(c)` (unchanged) else
local-fast-path or `ring_call`. The same `service.c` handler bodies run in both roles — relocated, not
rewritten.

**Perf:** every remote syscall is an IPC round-trip (µs vs ns). Compute-bound guests pay ~0 (hot path
never leaves the sandbox); chatty-I/O guests pay most. Keep the high-frequency syscalls local; later, a
shared guest-memory mapping eliminates the bounce copy for bulk read/write.

## Honest limitation

macOS has **no seccomp** — Seatbelt gates operation *classes*, not syscall numbers, so the macOS
sandbox is coarser than the Linux seccomp allowlist. Mitigation is defense-in-depth: the sandbox holds
no fds/fs/net *by construction*, so the class denies suffice; the true syscall-number filter arrives on
the Linux host. Put the lockdown behind a HAL seam (`sandbox_init` on darwin, `seccomp` on linux).

Status: shipped so far — the ring transport, guest-fd virtualization + per-process fd tables, the
local/remote carve, macOS Seatbelt profile. Remaining: Linux seccomp worker confinement, guard page
after the last ring, futex/`__ulock` wakeup (replace the servicer spin), eventfd/timerfd/signalfd
forwarding, `execve`-under-Seatbelt image read, and fork-under-split (the thorniest — prefer "fork =
spawn a fresh paired sandbox" over literal `fork()` of the JIT).
