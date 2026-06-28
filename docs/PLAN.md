# PLAN — remaining work (single source of truth)

`dd` is a Cargo workspace: `dd-jit` (JIT runtime + bindings), `dd-daemon` (Docker Engine API), and the desktop
surface. **This is the only plan — a work list of what is NOT yet implemented.** Subplans in
[`docs/design/`](design/). Validate: `make test` (261-green / 3 engines), `make test-docker[-full|-net|-fn]`,
`make test-macos`, `make test-realsw`, `make coverage`. (The clearable JIT + daemon work, the sentry validation,
and its P0/P1/P2 security hardening are landed — see git history; only the items below remain.)

## Sentry — structural hardening (security P0/P1/P2 + validation are done)

- **guest-fd virtualization** — the guest fd space is still the raw sentry fd space; give each guest virtual fd
  numbers so a raw number can never name a sentry-internal fd.
- **per-process fd tables** — forked workers share the sentry fd table (fine for the fork+exec inherit pattern;
  two long-lived post-fork processes that independently mutate fds would alias).
- **guard page** after the last ring (defense-in-depth — the length clamps already make OOB unreachable).
- **Linux seccomp** — the macOS Seatbelt worker confinement has no Linux equivalent wired.
- **edge/perf**: futex/`__ulock` wakeup (replace the servicer spin), eventfd/timerfd/signalfd forwarding,
  sendmmsg/recvmmsg, execve-under-Seatbelt image read.

## Perf — iterative profiling (not a one-shot change)

- redis command-dispatch hot path (~18× off native, 98.5% CPU-bound).
- sub-ms startup — the fork-server residual is `fork()` of the large-VM reservation + the guest `ld.so`.
- (tier-2 multi-block: investigated — already realized by the opt4 superblock former + the self-loop fold.)

## Refactoring

- `service.c` proc/misc dispatcher split — a prior verbatim attempt regressed threading and the root cause was
  never found; needs that debugging first, not a mechanical retry. (sysv/mem/signal/time/io are split.)

## Blocked on inputs / environment

- **netstack smoltcp** (`DD_NETSTACK`) — crate dep unfetchable in the offline build.
- **real x86_64 redis/postgres/nats end-to-end** — only arm64 images are local.

## Platform limitations (macOS host — the Linux primitive doesn't exist)

- non-PIE `ET_EXEC` `__PAGEZERO` (the JIT pointer-fixup is the workaround), cpu/io throttling (no cgroup; mem+pids
  via rlimit), `pidfd`, `io_uring`, mqueue `mq_*`, `edge` corners (`mprotect` PROT_NONE, `pipe2(O_DIRECT)`).

## Portability matrix (only darwin-host / both-guests built)

- `hal/<os>` (darwin), `jit/emit_<isa>` (arm64), `frontend/<arch>` (aarch64 + x86_64), `os/<os>` (linux).
  Eventual: linux→darwin, darwin→linux, linux→linux, darwin→darwin.
