# PLAN — remaining work (single source of truth)

`dd` is a Cargo workspace: `dd-jit` (the JIT runtime + Rust bindings), `dd-daemon` (the Docker Engine API),
and the desktop surface (`dd-client`/`dd-gui`/`dd-cli`). **This is the only plan — a work list of what is NOT
yet implemented.** Detailed subplans for the unimplemented items live in [`docs/design/`](design/). Validate
via the Makefile: `make test` (cross-engine matrix, **~240 green** / 3 engines), `make test-docker[-full|-net]`,
`make test-macos` (23/23), `make test-realsw`, `make coverage` (syscall/opcode gap report).

> The wave-1–6 JIT optimization sweep (20 opts), the wave-5 server bugs, and the W6A deep-bug cluster are all
> **landed** (matrix-240-green, each behind an env kill-switch) — no longer listed here. See git history.

## Large subsystems (priority order) — designs in `docs/design/`

1. **jit86 engine dedup** → [`engine-dedup.md`](design/engine-dedup.md). PR1/PR2 done (x86 on shared
   `jit/cache.c`; dispatch.c frontend-hook seam). **Remaining: PR3/PR4** — x86 hook definitions + swap the x86
   target onto `jit/dispatch.c`; hook the 5th divergence (the per-block `g_trace` dump). Gate: matrix green both
   engines.
2. **In-process netstack** → [`netstack.md`](design/netstack.md). External egress + L3 identity/reachability
   done (docker-net 7/7: per-container IP, `--network` join, br_* AF_UNIX switch, reach-by-name/ip, cross-net
   isolation). **Remaining:** only the optional in-process `smoltcp` stack behind `DD_NETSTACK`.
3. **Untrusted-guest sentry process-split** → [`sentry-split.md`](design/sentry-split.md) ·
   [`w6b-sentry.md`](design/w6b-sentry.md). First PR built (read/write/open(at)/close/lseek forwarded over an
   SPSC ring to a forked authority process; gate OFF = one-branch passthrough), **not yet integrated**.
   **Remaining:** per-context rings (a forking `sh` stalls at the first `clone`), the full fs/net/proc set
   (stat/iovec/sockets/execve/fork), SCM_RIGHTS for file-backed mmap, futex wakeup, allow/deny policy.
4. **x86 translator → native** → [`x86-perf.md`](design/x86-perf.md). Lazy NZCV, `pmovmskb`, and the full
   tier-2 substrate (W4-E arm tier-up / W6-C mangle elimination / W5-B x86 tier-2) landed. **Remaining:**
   carry-value consumers (`adc`/`sbb` deferral) + x87 `fptop` tracking.

## JIT correctness gaps (guest aborts / wrong output)

- **x86 SSE2 `strchr`/`strrchr` tail bug** — **both** miss a match in the final partial 16-byte block adjacent
  to the NUL (return NULL). Deterministic minimal repro (from `make test-diff` differential oracle): a 63-byte
  buffer with the only `Z` at index 56 → dd-jit `strrchr`/`strchr` = NULL, qemu/native oracle = 56; aarch64 is
  correct. A flag/mask edge in the `pmovmskb`-consuming tail; reproduces on stock baseline + `NOSSEOPT=1` alike
  (not the SSE opt). **Broader than previously thought — `strchr` is affected, not just `strrchr`.** Tracked as
  an `.xfail` regression case in `make test-diff` (`diff_strops`) that flips to xpass when fixed.
- **non-PIE `ET_EXEC` fork+execve — PARTIAL** · gate `NONPIE_NOFIXUP=1` · [`w6a-deepbugs.md`](design/w6a-deepbugs.md),
  [`fix-nonpie-crash.md`](design/fix-nonpie-crash.md). Landed: a no-relink SIGSEGV fault-fixup (redirect absolute
  code jumps + emulate faulting integer ld/st at `+bias`; 0 PIE regression). **Remaining:** SIMD-Q/`ldr q` +
  LSE-atomic-RMW absolute forms, and syscall pointer-args into low `.rodata`/`.data` (they clean-abort, not
  silently corrupt). The host-`__PAGEZERO`-shrink path is a confirmed dead end. Victims: gcc, postgres `gosu`.

## Scoped bug-fix designs (root-caused, fix scoped — subplans in `docs/design/`)

- **postgres:alpine startup** → [`fix-postgres.md`](design/fix-postgres.md). The post-shebang clone-stack fix
  (B3) is landed; full default-path `docker run postgres:alpine` still needs the non-PIE `gosu` residual above
  (B2) **or** the daemon `--user`→`--uid` / OCI-Entrypoint path (B4, `dd-daemon/src/images.rs`) to skip the
  `gosu` re-exec.

## JIT perf — remaining squeeze

- **redis throughput** — the jitted server event loop is 98.5% CPU-bound, ~18× off native (transport is fine).
  x86 trace + x86 tier-2 + 2-way IBTC are landed but need **exercising/tuning on the redis command-dispatch/dict
  hot path** specifically. ← highest-ceiling lever.
- **Tier-2 multi-block traces spanning call-free spans** — real busybox forms ~0 stolen-register traces (the x30
  mangle is in call prologues/epilogues, excluded by W6-C's call-free constraint); multi-block traces over the
  spans *between* calls would capture it.
- **sqlite temp-file spill = 97% `pread64`/`pwrite64`** to a host temp file → back the sorter/index temp with an
  in-memory/tmpfs fd → turn ~2,834 host I/O syscalls into memcpy.
- **Sub-millisecond startup** — the fork-server is at 1.22 ms; the residual is `fork()` of a large-VM-reservation
  process + the guest's own ld.so.

## Validation blocked on inputs

- **Run real redis / postgres / nats end-to-end** — sockets (AF_INET/INET6) + epoll + futex + the wave-5
  server-bug fixes make the server-DB class runnable; needs **x86_64 images** (only arm64 are local) and the
  non-PIE `-static` residual above.

## Coverage gaps — syscalls
*Source: `make coverage`.*
- **Edge corners** (`edge` group; xfail-tracked): `mprotect` PROT_NONE is a no-op (no fault → RELRO/guard pages
  unenforced), `pipe2(O_DIRECT)` packet mode (host-limited — macOS pipes can't frame writes).
- **Host-limited (emulate or leave ENOSYS):** POSIX mqueue `mq_*` (180-185), `timer_create`/`timer_*` (107-111)
  *(could ride kqueue)*; `pidfd`/`io_uring`/NUMA/keyring/module/`ptrace` out of scope.

## Docker API — remaining field/behaviour fidelity (Engine API v1.43, single-node; Swarm out of scope)

| Area | What's left | Pri |
|------|-------------|:---:|
| `docker exec` | `--privileged` (rare; `-e`/`-w`/`-d`/`-u` done) | P3 |
| `docker run` opts | wider `HostConfig`: restart policy, `--cap-add`, `--device`, `--mount`, `--privileged` (`--user`/`--label` done) | P2 |
| `docker build` | BuildKit/layer cache (every build re-runs from base) — args/target/nocache/labels/digest-IDs done | P2 |
| `docker cp` | non-default-driver named volumes (default `<volumes_dir>/<name>` handled) | P3 |

## Docker functional gaps (from `make test-docker-fn` — 72 pass/0 fail; real behavioral assertions)
`docker exec -it` / `run -it` **confirmed working end-to-end** (pty raw mode, Ctrl-C→SIGINT, resize→SIGWINCH,
stdin, exit codes). Remaining real gaps:

**Correctness bugs (wrong behavior on valid commands):**
- **Duplicate `--name` accepted** — a 2nd container starts with an in-use name (should error "already in use"). P1
- **`docker rm` of a *running* container succeeds without `-f`** (should error). P1
- **`exec` into a *stopped* container runs the command** (should error "is not running"). P2
- **`docker events --filter container=<name>` returns nothing** (unfiltered events work — the by-name filter is broken). P2
- **Logs don't interleave stdout/stderr chronologically** (stderr appended after stdout). P3

**Missing / partial features:**
- **`-p` published ports not reachable from host** + **container↔container by-name/IP not reachable** — the
  netstack/TCP-cork area (see [`netstack.md`](design/netstack.md)). **P0 for real usage.**
- **`-v …:ro` not enforced** (writes to a read-only bind succeed) — correctness/isolation. P1
- **`--rm` doesn't auto-remove** the container on exit. P2
- **`docker commit` unimplemented** ("no route for `/commit`" — contradicts the stale "38/38 compliant" claim). P2
- **`build` with `FROM <previous-stage>` as a base** tries to pull the stage name from the registry (`COPY --from` works). P3

**Harness/infra:** the `scenarios/*.sh` isolate only the *socket*, not state — adopt `DD_STATE`/`DD_VOLUMES`
isolation (a non-isolated daemon loaded **339 containers** from shared `~/.dd/state.json`). Compose suite
(`compose.sh`) not delivered — no compose binary on the Linux harness; add where available.

## Platform limitations (macOS host — can't provide the Linux primitive; off the work-list)
Non-PIE `ET_EXEC` fixed-vaddr (the `__PAGEZERO` low-4GB reservation — see the JIT correctness gap above for the
achievable workaround), cpu/io throttling (no cpu/io cgroup; mem+pids *are* enforced via rlimit), `pidfd`,
`io_uring`. These would come for free on a linux→linux build.

## Portability matrix (seams exist; only darwin-host / both-guests built)
`hal/<os>` (darwin built; linux = mprotect/SIGSEGV-ucontext/seccomp), `jit/emit_<isa>` (arm64 built),
`frontend/<arch>` (aarch64 + x86_64 built), `os/<os>` (linux built). Eventual targets: linux→darwin,
darwin→linux, linux→linux, darwin→darwin.
