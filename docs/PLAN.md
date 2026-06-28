# PLAN — remaining work (single source of truth)

`dd` is a Cargo workspace: `dd-jit` (JIT runtime + bindings), `dd-daemon` (Docker Engine API), and the desktop
surface. **This is the only plan — a work list of what is NOT yet implemented.** Subplans in
[`docs/design/`](design/). Validate: `make test` (**261-green** / 3 engines), `make test-docker[-full|-net|-fn]`,
`make test-macos`, `make test-realsw`, `make coverage`.

> **The clearable JIT + daemon work is DONE, VALIDATED, and REVIEWED (see git history):** the wave-1–6 opt
> sweep, wave-5 server bugs, W6A deep bugs, engine-dedup PR3/4, strchr/strrchr, non-PIE (SIMD/LSE + syscall
> pointer-arg g2h, now incl. sockaddr/buffer args), adc/sbb, x87 fptop, POSIX timers, sigpipe, the `-p` TCP+UDP
> host bridges, the **sentry** (fs + sockets + N-ring pool + fork/exec/wait + sendmsg/SCM_RIGHTS + poll/epoll),
> sqlite RAM temp-file backing, `-v :ro` enforcement, postgres B3/B4, the munmap guard-tail, the vfs.c/
> translate.c/containers.rs/service-io.c refactor splits, the full Docker-API fidelity, per-scenario test
> isolation — **plus this session's review-and-harden pass** (JIT + daemon correctness reviews with every real
> finding fixed, and the sentry P0/P1 security hardening).

## Sentry: VALIDATED + HARDENED

- **Validated** under `DDJIT_UNTRUSTED`: golden guests for fs (`sentry_fs`), sockets (`sentry_net`), and the
  fork lane (`sentry_fork`) run sandbox-forwarded and match the trusted baseline on both linux engines (261).
- **P0/P1 security hardened**: clamp every kernel length to `min(len, BUFSZ-offset)` (OOB closed), copy + validate
  iovec/msghdr/socklen in per-thread private memory (TOCTOU closed), `FDPASS` checks a `g_guest_fds` ownership
  bitset (control sockets / daemon fds → `-EBADF`), Seatbelt denies `mach-lookup` + fails closed.
- **In flight**: outbound-cmsg fd validation (P2 — reject a worker-supplied cmsg fd it doesn't own on the send
  path). **Residual (structural)**: guest-fd virtualization, per-process fd tables, guard page (defense-in-depth
  now the clamps make OOB unreachable), Linux seccomp.

## Residual — NOT clearable by a mechanical agent pass

- **Perf** (iterative profiling): redis command-dispatch hot path (~18× off native), sub-ms startup (fork of the
  large-VM reservation + ld.so). (tier-2 multi-block: investigated — already realized by opt4 + the self-loop fold.)
- **Sentry edge/perf**: futex/`__ulock` wakeup, eventfd/timerfd/signalfd forwarding, sendmmsg/recvmmsg.
- **Refactoring**: `service.c` proc/misc dispatcher split — the io split landed cleanly, but the proc+misc batch
  regressed threading in a prior attempt with the root cause never found; needs that debugging first, not a retry.

## Blocked on inputs / environment

- **netstack smoltcp** (`DD_NETSTACK`) — crate dep unfetchable offline. **real x86_64 redis/postgres/nats** —
  only arm64 images local.

## Platform limitations (macOS host — the Linux primitive doesn't exist)

Non-PIE `ET_EXEC` `__PAGEZERO` (JIT pointer-fixup is the workaround), cpu/io throttling (no cgroup; mem+pids via
rlimit), `pidfd`, `io_uring`, mqueue `mq_*`, `edge` corners (`mprotect` PROT_NONE, `pipe2(O_DIRECT)`).

## Portability matrix (only darwin-host / both-guests built)

`hal/<os>` (darwin), `jit/emit_<isa>` (arm64), `frontend/<arch>` (aarch64 + x86_64), `os/<os>` (linux).
Eventual: linux→darwin, darwin→linux, linux→linux, darwin→darwin.
