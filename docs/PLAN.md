# PLAN — remaining work (single source of truth)

`dd` is a Cargo workspace: `dd-jit` (JIT runtime + Rust bindings), `dd-daemon` (Docker Engine API), and the
desktop surface (`dd-client`/`dd-gui`/`dd-cli`). **This is the only plan — a work list of what is NOT yet
implemented.** Subplans for the deeper items live in [`docs/design/`](design/). Validate: `make test`
(cross-engine matrix, **247-green** / 3 engines), `make test-docker[-full|-net|-fn]`, `make test-macos`,
`make test-realsw`, `make coverage`.

> **Landed (no longer listed; see git history):** the wave-1–6 opt sweep (20 opts), wave-5 server bugs, the W6A
> deep-bug cluster, **engine-dedup PR3/4** (x86 on shared `jit/dispatch.c`), **strchr/strrchr** SSE min/max fix,
> **non-PIE SIMD-Q/LSE-RMW** fault-fixup, **adc/sbb** lazy-carry deferral, **POSIX timers** (kqueue), the
> **sentry first PR + daemon wiring** (`--security-opt sandbox`), **postgres B3/B4**, the munmap guard-tail leak,
> and the full **Docker-API HostConfig / build-layer-cache / exec / cp** fidelity.

## Docker functional gaps (real behavioral bugs — `make test-docker-fn`) ← highest user-facing priority

- **[P0] `-p` published ports not reachable from host** + container↔container by-name/IP not reachable — the
  netstack/port-forward gap. See [`netstack.md`](design/netstack.md).
- **[P1]** duplicate `--name` accepted (should error "already in use"); `docker rm` of a *running* container
  succeeds without `-f` (should error); `-v …:ro` not enforced (writes to a read-only bind succeed).
- **[P2]** `exec` into a *stopped* container runs the command (should error); `docker events --filter
  container=<name>` returns nothing; `--rm` doesn't auto-remove on exit; **`docker commit` unimplemented**
  (no route for `/commit`).
- **[P3]** logs don't interleave stdout/stderr chronologically; `build` with `FROM <previous-stage>` as a base
  tries to pull the stage name (`COPY --from` works).
- **Harness:** `scenarios/*.sh` isolate only the socket, not state (adopt `DD_STATE`/`DD_VOLUMES`); no compose
  binary on the Linux harness.

## Subsystems (remaining)

- **Sentry completion** *(in progress)* → [`sentry-split.md`](design/sentry-split.md). First PR + daemon wiring
  landed (forwards read/write/open(at)/close/lseek, gate `--security-opt sandbox`). **Remaining:** the full
  fs/net/proc set (stat/iovec/getdents/pread-pwrite *in progress*; then sockets/execve/clone), per-context rings
  (a forking `sh` stalls at the first `clone`), SCM_RIGHTS for file-backed mmap, futex wakeup, allow/deny policy.
- **x86 translator → native** → [`x86-perf.md`](design/x86-perf.md). Lazy NZCV, pmovmskb, full tier-2, adc/sbb
  landed. **Remaining:** x87 `fptop` tracking *(in progress)*.
- **In-process netstack** → [`netstack.md`](design/netstack.md). docker-net 7/7 done; the optional `smoltcp`
  stack behind `DD_NETSTACK` is **blocked** (the crate dep can't be fetched in the offline build). Tied to the
  `-p` ports P0 above.

## JIT correctness (remaining)

- **non-PIE syscall pointer-args into low image** — the last non-PIE residual: a `g2h()` redirect on non-PIE
  syscall pointer args that point into low `.rodata`/`.data` (today they clean-abort). [`w6a-deepbugs.md`](design/w6a-deepbugs.md).
- **`sigpipe` write()-to-socket** *(in progress)* — set `SO_NOSIGPIPE` at socket creation so write/send to a
  closed peer returns EPIPE.

## JIT perf — remaining squeeze (lower priority)

- **redis throughput** — jitted event loop 98.5% CPU-bound, ~18× off native; x86 trace/tier-2/IBTC need
  exercising+tuning on the redis command-dispatch/dict hot path.
- **Tier-2 multi-block traces** over call-free spans (capture the x30 call-prologue mangle W6-C can't).
- **sqlite temp spill** = 97% pread/pwrite → tmpfs-back the sorter temp fd.
- **Sub-ms startup** — fork-server at 1.22 ms; residual = `fork()` of the large-VM-reservation process + ld.so.

## Refactoring — split oversized files *(in progress)*

`service.c` (3.5k → dispatcher split: sysv/mem/signal/time extracted; io/proc/misc reverted, needs a careful
retry), `translate.c` (2.5k), `vfs.c` *(in progress)*, daemon `containers.rs` (1k). Pattern: verbatim
`#include` of category sub-files, matrix-gated byte-identical.

## Validation blocked on inputs

- **Run real redis / postgres / nats end-to-end** — needs **x86_64 images** (only arm64 local) + the non-PIE
  pointer-arg residual above.

## Platform limitations (macOS host — can't provide the Linux primitive; off the work-list)

Non-PIE `ET_EXEC` fixed-vaddr `__PAGEZERO` (see the JIT correctness gap for the achievable workaround), cpu/io
throttling (no cgroup; mem+pids enforced via rlimit), `pidfd`, `io_uring`, mqueue `mq_*`, `edge` corners
(`mprotect` PROT_NONE, `pipe2(O_DIRECT)`). Free on a linux→linux build.

## Portability matrix (seams exist; only darwin-host / both-guests built)

`hal/<os>` (darwin), `jit/emit_<isa>` (arm64), `frontend/<arch>` (aarch64 + x86_64), `os/<os>` (linux).
Eventual: linux→darwin, darwin→linux, linux→linux, darwin→darwin.
