# PLAN — remaining work (single source of truth)

`dd` is a Cargo workspace: `dd-jit` (JIT runtime + bindings), `dd-daemon` (Docker Engine API), and the desktop
surface. **This is the only plan — a work list of what is NOT yet implemented.** Subplans in
[`docs/design/`](design/). Validate: `make test` (**249-green** / 3 engines), `make test-docker[-full|-net|-fn]`,
`make test-macos`, `make test-realsw`, `make coverage`.

> **The clearable JIT + daemon work is DONE (see git history):** the wave-1–6 opt sweep, wave-5 server bugs, the
> W6A deep-bug cluster, engine-dedup PR3/4, strchr/strrchr, non-PIE (SIMD/LSE fixup **+ syscall pointer-arg
> g2h**), adc/sbb, **x87 fptop**, POSIX timers, sigpipe, the **`-p` TCP + UDP host-port bridges**, the **sentry**
> (fs + socket family + N-ring pool + fork/exec/wait + sendmsg/SCM_RIGHTS + poll/epoll + daemon `--security-opt`
> wiring), the **sqlite RAM temp-file backing**, **`-v :ro` enforcement**, postgres B3/B4, the munmap guard-tail
> leak, the **vfs.c / translate.c / daemon containers.rs refactor splits**, and the full **Docker-API** fidelity
> (HostConfig / build-cache / `commit` / `--name`·rm·exec·`--rm`·events·logs-interleave·`:ro`).

## Residual — NOT clearable by a mechanical agent pass

**Perf (needs iterative profiling, not a one-shot change):**
- redis command-dispatch hot path (~18× off native, 98.5% CPU-bound) — a measure-and-tune loop.
- sub-ms startup — the fork-server residual is `fork()` of the large-VM reservation + the guest `ld.so`.
- tier-2 multi-block: investigated — **already realized** by the `opt4` superblock former + the self-loop fold;
  no gap on the same-ISA aarch64 engine.

**Refactoring (blocked on a root-cause, not a mechanical split):**
- `service.c` (3.5k) io/proc/misc dispatcher split — a prior verbatim attempt regressed threading and the cause
  was never found, so it was reverted. sysv/mem/signal/time are split. Retrying mechanically risks the same
  subtle regression; this needs the root cause found first (a debugging task — per "don't break anything").

**Sentry completeness (the sandbox is functional for real images; these are edge/perf):**
- futex/`__ulock` wakeup (replace the servicer spin), per-process fd tables, eventfd/timerfd/signalfd
  forwarding, sendmmsg/recvmmsg, execve-under-Seatbelt image read.

## Blocked on inputs / environment (cannot do in this sandbox)

- **netstack smoltcp** (`DD_NETSTACK`) — the crate dep can't be fetched in the offline build.
- **real x86_64 redis/postgres/nats end-to-end** — only arm64 images are local.

## Platform limitations (macOS host — the Linux primitive doesn't exist)

Non-PIE `ET_EXEC` `__PAGEZERO` (the JIT pointer-fixup is the workaround), cpu/io throttling (no cgroup; mem+pids
via rlimit), `pidfd`, `io_uring`, mqueue `mq_*`, `edge` corners (`mprotect` PROT_NONE, `pipe2(O_DIRECT)`). Free
on a linux→linux build.

## Portability matrix (only darwin-host / both-guests built)

`hal/<os>` (darwin), `jit/emit_<isa>` (arm64), `frontend/<arch>` (aarch64 + x86_64), `os/<os>` (linux).
Eventual: linux→darwin, darwin→linux, linux→linux, darwin→darwin.
