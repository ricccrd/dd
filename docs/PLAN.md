# PLAN — remaining work (single source of truth)

`dd` is a Cargo workspace: `dd-jit` (JIT runtime + bindings), `dd-daemon` (Docker Engine API), and the desktop
surface. **This is the only plan — a work list of what is NOT yet implemented.** Subplans in
[`docs/design/`](design/). Validate: `make test` (**249-green** / 3 engines), `make test-docker[-full|-net|-fn]`,
`make test-macos`, `make test-realsw`, `make coverage`.

> **Landed (see git history):** the wave-1–6 opt sweep, wave-5 server bugs, the W6A deep-bug cluster,
> **engine-dedup PR3/4**, strchr/strrchr, **non-PIE** SIMD/LSE fixup **+ syscall pointer-arg g2h** (only nested
> iovec/msghdr left), adc/sbb, POSIX timers, **x87 fptop**, sigpipe, the **`-p` TCP + UDP host-port bridges**,
> the **sentry** (first PR + fs set + **socket family** + **N-ring pool** + daemon `--security-opt` wiring),
> postgres B3/B4, the munmap guard-tail leak, the **vfs.c / translate.c / daemon containers.rs splits**, and the
> full **Docker-API** HostConfig / build-cache / `commit` / `--name`·rm·exec·`--rm`·events·`:ro` fidelity.

## In flight (agents running)

- **Sentry next PR** — execve/clone/wait4 (forked-guest lanes), sendmsg/recvmsg + SCM_RIGHTS fd-passing,
  poll/epoll over sentry-owned fds, futex wakeup. Closes the gap to a sound sandbox for real images.
- **sqlite in-memory temp-file backing** — back O_TMPFILE/unlinked-scratch with a host memfd (the 97%-pread/pwrite
  spill → memcpy perf win).
- **docker logs chronological interleave** — the last `make test-docker-fn` behavioral bug.

## Remaining (queued / harder)

- **Perf squeeze** (lower priority): redis command-dispatch hot-path tuning (~18× off native), tier-2 multi-block
  traces over call-free spans, sub-ms startup (fork of the large-VM reservation + ld.so). [`x86-perf.md`](design/x86-perf.md)
- **Refactoring:** `service.c` (3.5k) io/proc/misc dispatcher split — needs a careful retry (a prior attempt
  broke threading; root cause never found, so reverted). The sysv/mem/signal/time splits already landed.
- **`-v :ro` JIT-layer write enforcement** — the daemon parses + reports it; the JIT `Volume{}` has no RO flag,
  so writes to a read-only bind aren't blocked at the mount.

## Blocked on inputs / environment

- **netstack smoltcp** (`DD_NETSTACK`) — the crate dep can't be fetched in the offline build.
- **real redis / postgres / nats end-to-end** — needs **x86_64 images** (only arm64 local).

## Platform limitations (macOS host — can't provide the Linux primitive)

Non-PIE `ET_EXEC` `__PAGEZERO` (the JIT pointer-fixup is the achievable workaround), cpu/io throttling (no
cgroup; mem+pids via rlimit), `pidfd`, `io_uring`, mqueue `mq_*`, `edge` corners (`mprotect` PROT_NONE,
`pipe2(O_DIRECT)`). Free on a linux→linux build.

## Portability matrix (only darwin-host / both-guests built)

`hal/<os>` (darwin), `jit/emit_<isa>` (arm64), `frontend/<arch>` (aarch64 + x86_64), `os/<os>` (linux).
Eventual: linux→darwin, darwin→linux, linux→linux, darwin→darwin.
