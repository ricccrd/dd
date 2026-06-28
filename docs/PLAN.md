# PLAN — remaining work (single source of truth)

`dd` is a Cargo workspace: `dd-jit` (JIT runtime + bindings), `dd-daemon` (Docker Engine API), and the desktop
surface. **This is the only plan — a work list of what is NOT yet implemented.** Subplans in
[`docs/design/`](design/). Validate: `make test` (**249-green** / 3 engines), `make test-docker[-full|-net|-fn]`,
`make test-macos`, `make test-realsw`, `make coverage`.

> **Landed (see git history):** the wave-1–6 opt sweep (20 opts), wave-5 server bugs, the W6A deep-bug cluster,
> **engine-dedup PR3/4**, **strchr/strrchr** fix, **non-PIE SIMD-Q/LSE-RMW** fixup, **adc/sbb** deferral, **POSIX
> timers**, **sigpipe** (SO_NOSIGPIPE), the **`-p` TCP host-port bridge**, the **sentry** first PR + fs syscall
> set + daemon `--security-opt` wiring, **postgres B3/B4**, the munmap guard-tail leak, the **vfs.c split**, and
> the full **Docker-API HostConfig / build-layer-cache / exec / cp** fidelity.

## Docker functional gaps (real behavioral bugs — `make test-docker-fn`)

- **[P1/P2/P3]** *(agent in flight)*: duplicate `--name`, `rm` of a running container w/o `-f`, `exec` into a
  stopped container, `events --filter container=`, `--rm` auto-remove, `-v :ro` enforcement, logs interleave,
  **`docker commit`**, `build FROM <prev-stage>`.
- **[P0] `-p` ports:** TCP host-bridge **landed**; **published UDP** *(in flight)* + a same-host-port-conflict
  pre-check at create.
- **Harness:** `scenarios/*.sh` isolate only the socket (adopt `DD_STATE`/`DD_VOLUMES`); no compose binary.

## Subsystems (remaining)

- **Sentry** → [`sentry-split.md`](design/sentry-split.md). First PR + fs syscall set + daemon wiring landed;
  **sockets + per-context rings** *(in flight)*. **Remaining:** execve/clone, SCM_RIGHTS for file-backed mmap,
  futex/`__ulock` wakeup (replace the spin), allow/deny policy.
- **x86 translator → native** → [`x86-perf.md`](design/x86-perf.md). All but x87 landed; **x87 `fptop`** *(in
  flight)*. Then the perf squeeze below.
- **In-process netstack smoltcp** → [`netstack.md`](design/netstack.md). Optional, **blocked** (offline crate dep).

## JIT correctness (remaining)

- **non-PIE syscall pointer-args (g2h redirect)** *(in flight)* — the last non-PIE residual (low-image `.rodata`/
  `.data` pointers passed to syscalls). [`w6a-deepbugs.md`](design/w6a-deepbugs.md).

## JIT perf — remaining squeeze (lower priority)

- redis hot-path tuning; tier-2 multi-block traces (x30 call-prologue mangle); sqlite tmpfs temp fd; sub-ms
  startup (fork-server residual = large-VM fork + ld.so).

## Refactoring — split oversized files *(in progress)*

`vfs.c` done (→ vfs/{gmap,overlay,resolve}). Remaining: `translate.c` (2.5k), `service.c` (3.5k — sysv/mem/
signal/time extracted; io/proc/misc needs a careful retry), daemon `containers.rs` (1k). Pattern: verbatim
`#include` of category sub-files, matrix-gated byte-identical.

## Validation blocked on inputs

- **real redis / postgres / nats end-to-end** — needs **x86_64 images** (only arm64 local) + the non-PIE
  pointer-arg residual above.

## Platform limitations (macOS host — can't provide the Linux primitive)

Non-PIE `ET_EXEC` `__PAGEZERO` (see the JIT correctness gap for the workaround), cpu/io throttling (no cgroup;
mem+pids via rlimit), `pidfd`, `io_uring`, mqueue `mq_*`, `edge` corners (`mprotect` PROT_NONE,
`pipe2(O_DIRECT)`). Free on a linux→linux build.

## Portability matrix (only darwin-host / both-guests built)

`hal/<os>` (darwin), `jit/emit_<isa>` (arm64), `frontend/<arch>` (aarch64 + x86_64), `os/<os>` (linux).
Eventual: linux→darwin, darwin→linux, linux→linux, darwin→darwin.
