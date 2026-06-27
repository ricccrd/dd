# Execution backlog — all remaining work, ordered to burn down

Derived from `docs/PLAN.md` + the five `docs/design/*.md` roadmaps. **Struck-through (`~~done~~`)
items in PLAN.md are excluded.** This is the manager's execution queue: every item is a concrete
task with scope / files / effort (S/M/L) / risk / parallelism / dependencies.

**Risk legend.**
- **JIT-core** = compiles into the JIT TU(s); regression gate is the **full cross-engine matrix
  (`make test`, ~236 green over aarch64 + x86_64 + darwin)**. Never land two of these in one PR.
- **daemon-only** = Rust `dd-daemon`; gate is `make test-docker` / `-full` / `-net`. Safe to
  parallelize except where they touch the same file (see §Waves).

**Serializing files** (any two tasks touching one must be sequenced, not run in parallel):
`dd-daemon/src/model.rs`, `dd-daemon/src/main.rs`, `dd-daemon/src/containers.rs`,
`dd-daemon/src/runtime.rs`, `dd-jit/src/lib.rs`; and on the JIT side
`os/linux/service.c`, `jit/dispatch.c`, `jit/emit_arm64.c`, `frontend/*/translate.c`,
`targets/linux_{aarch64,x86_64}.c`.

Already landed since PLAN.md was last revised (do **not** re-queue): engine-dedup **PR-1**
(`jit/cache.c` lift, `G_GPC_HASH_SHIFT` in both abi.h, x86 TU includes the shared cache),
netstack **PR-1** (IPAM `172.18/12`, per-container IP, `network inspect` members → 5/7).

---

## (A) Bounded daemon-only tasks — safe to parallelize

Pure `dd-daemon` Rust, no JIT rebuild, matrix-irrelevant. Ordered by value/effort.

| # | Task | Scope (one line) | Files | Eff | Parallel-safe? | Deps |
|---|------|------------------|-------|:---:|---|---|
| A1 | **Lifecycle event bus** | A process-wide broadcast channel; container create/start/die/stop/destroy + network/volume/image events pushed into the open `GET /events` stream (compose + GUI watch it). | `main.rs` (channel + `/events` handler), new `events.rs`, emit sites in `containers.rs`/`images.rs`/`networks.rs`/`volumes.rs` | M | **Conflicts on `main.rs` + `containers.rs`** (serialize vs A3/A4/A6/A8) | — |
| A2 | **nats image arch detection** | `detect_arch` fails on distroless/scratch (no ELF to sniff); add a manifest-`platform` fallback so `nats:latest` pulls. Unblocks a realsw target, daemon-only. | `containers.rs` (`containers_create`/`detect_arch`), `images.rs`/`registry.rs` | S | Conflicts on `containers.rs` | — |
| A3 | **`exec -d` (detached)** | Detached exec returns 200 immediately instead of blocking on output. | `containers.rs` exec handler | S | Conflicts on `containers.rs` | — |
| A4 | **inspect `State.{Pid,StartedAt,FinishedAt}`** | Track + report process pid and RFC3339 start/finish timestamps (not tracked today). | `model.rs` (state fields), `containers.rs` (inspect fill), `runtime.rs` (capture on spawn/exit) | M | Conflicts on `model.rs`+`containers.rs`+`runtime.rs` | — |
| A5 | **`logs -f` / `--since`** | Follow-stream (tail-f) and `--since` time filter on the logs endpoint. | `containers.rs` logs handler | M | Conflicts on `containers.rs` | — |
| A6 | **`ps --filter` / `--size` / Labels / ImageID** | Server-side filter eval, `SizeRw/SizeRootFs`, and `Labels`/`ImageID`/network fields on `ps`. | `containers.rs` (ps), `model.rs` (size fields) | M | Conflicts on `containers.rs`+`model.rs` | — |
| A7 | **build labels + content-digest image IDs** | `LABEL` → `Config.Labels`; replace synthetic IDs with real content digests. | `build.rs`, `images.rs` | M | Independent (own files) | — |
| A8 | **image `Created` field + `Labels`** | Add `Image.created`; report real `Created`/`Labels` in image inspect/history. | `model.rs` (Image struct), `images.rs` | S | Conflicts on `model.rs` | — |
| A9 | **`cp` into named-volume paths** | `docker cp` resolves named-volume mount targets (handlers currently lack the volume list). | `archive.rs`, `containers.rs` (pass volume list) | S | Conflicts on `containers.rs` | — |
| A10 | **stop/kill signal honoring** | Honor `signal`/`t`; deliver the real signal (containers run synchronously today → needs async run + signal path). | `containers.rs`, `runtime.rs` | M | Conflicts on `containers.rs`+`runtime.rs`; pairs with A4 (async run) | soft: A4 |
| A11 | **volumes persist `--driver`/`--opt`/`--label`** | Store + report the three volume creation fields. | `volumes.rs`, `model.rs` (Vol fields) | S | Conflicts on `model.rs` | — |
| A12 | **pull/push per-layer progress; push `aux{Digest}`** | Per-layer progress bars; final digest on push. | `images.rs`/`registry.rs` | M | Independent | — |
| A13 | **`docker build` BuildKit cache** | Layer/step caching (every build re-runs from base today). | `build.rs` | L | Independent (own file) | — |

> `run --user` and **container-to-container reachability** are *not* here — they cross into the JIT
> (uid apply / virtual switch). See B4 and C2.

---

## (B) Bounded JIT tasks — matrix-gated

Each is a localized `service.c` (or small) addition. **All share `os/linux/service.c`** (and some
share `targets/linux_*.c`), so they **serialize against each other** even though each is small.
Batch them into a few sequenced PRs rather than 1-per-agent. Ordered by value/effort.

| # | Task | Scope | Files | Eff | Notes / Deps |
|---|------|-------|-------|:---:|---|
| B1 | **`setsid`(157)** | Session leader create — the daemonize primitive. | `service.c` | S | **Unblocks postgres (D4)**; macOS has the primitive |
| B2 | **`madvise(MADV_DONTNEED)`** | Actually drop anon pages (re-`mmap` `MAP_FIXED\|ANON`); reread returns zeros. | `service.c` | M | **Unblocks redis/jemalloc (D3)**; flips `edge/madvise` XPASS |
| B3 | **`rt_sigtimedwait`(137) + `rt_sigsuspend`(133)** | Guest-mask-aware signal waiting. | `service.c`, `os/linux/signal.c` | M | The PLAN-named Group-B core item |
| B4 | **uid apply for `--user`** | Apply a uid: `SpawnConfig` gains a uid field (Rust) → JIT setuid/setgid at guest start; wires `run --user` + `exec -u`. | `lib.rs` + `runtime.rs` + `containers.rs` (Rust), `service.c`/`targets/linux_*.c` (apply) | M | **Cross-boundary**: needs daemon SpawnConfig field *and* JIT apply → matrix-gated |
| B5 | **`clock_nanosleep(TIMER_ABSTIME)`** | Treat ABSTIME absolutely (today sleeps for the absolute value → hangs). | `service.c` | S | Real hang bug; flips `edge/clocknanosleep` |
| B6 | **`MSG_NOSIGNAL` + SIGPIPE→EPIPE** | Write to closed socket returns EPIPE instead of fatal SIGPIPE. | `service.c` (send family) | S | flips `edge/msgflags` corner |
| B7 | **`SCM_RIGHTS` fd-passing over AF_UNIX** | Translate the cmsg control block so `recvmsg` yields the passed fd (systemd/Docker/D-Bus). | `service.c` (`recvmsg`/`sendmsg` 211/212) | M | High-value (socket-activation patterns) |
| B8 | **abstract-namespace AF_UNIX** (`sun_path[0]==0`) | Map abstract sockets to a host backing (X11/D-Bus/systemd). | `service.c` (bind 200 + connect 203) | M | — |
| B9 | **`renameat2` flags** | Honor `RENAME_NOREPLACE`/`RENAME_EXCHANGE`. | `service.c` | S | flips `edge/renameat2` |
| B10 | **`fallocate(PUNCH_HOLE)` + `lseek(SEEK_HOLE/DATA)`** | Sparse-file hole punch + hole/data offsets. | `service.c` | S | two `edge` corners |
| B11 | **`open(O_TMPFILE)`** | Unnamed temp file support. | `service.c` (openat) | S | flips `edge/otmpfile` |
| B12 | **`/proc/self/fd` synthesis** | Readlink/enumerate the live fd table. | `service.c` / `fscache.c` | M | flips `edge/procselffd` |
| B13 | **`mprotect PROT_NONE` faulting** | Make `PROT_NONE` actually fault (RELRO/guard pages). | `service.c` + `hal` (mprotect/SIGSEGV path) | M | darwin-native already passes; risk-touchy (fault path) |
| B14 | **small corner fixes** | `pipe2(O_DIRECT)` packet mode; `F_SETPIPE_SZ/F_GETPIPE_SZ`; `dup3(fd,fd,0)`→EINVAL. | `service.c` | S | bundle into one PR; low value |
| B15 | **IPC `*ctl(IPC_STAT/IPC_SET)`** | Translate macOS `*_ds` layouts to guest ABI (returns ENOSYS today). | `os/linux/container/*` IPC | M | rare introspection; low value |

> **Sequencing within B:** because all touch `service.c`, run as a *serial chain* of small PRs, not
> parallel agents. Suggested chain order: B1, B2, B3, B5, B6 (quick high-value), then B7, B8, B4,
> then the remaining edge fixes B9–B15. B4 additionally serializes against A-group `containers.rs`.

---

## (C) Large-subsystem next PRs — one at a time, reference the design doc

Each is the *smallest matrix-green slice* from its roadmap. JIT-touching ones are matrix-gated and
mutually exclusive on shared C files. Ordered by value/effort + unblock-leverage.

| # | Task | First/next PR | Files | Eff | Risk | Deps |
|---|------|---------------|-------|:---:|---|---|
| C1 | **netstack PR2 — `/etc/hosts` injection** (`netstack.md` §6) | Daemon writes per-network `/etc/hosts`; self-name + name→IP entries. **Daemon-only.** | `runtime.rs` (hosts write), `containers.rs`, `model.rs` (Endpoint aliases) | S | daemon-only | netstack PR1 (done) |
| C2 | **netstack PR3 — 2A virtual switch** (`netstack.md` §6, §9) | Per-network AF_UNIX rendezvous switch: `bind/listen/accept/connect/getsockname/getpeername` for `DD_SUBNET`; TCP first. **Takes `docker-net.sh` → 7/7.** | new `os/linux/container/netbridge.c`, `service.c` (socket cases range-check), `container/state.c` (`parse_subnet`), `targets/linux_*.c` (`DD_NETBR/DD_IP/DD_SUBNET/DD_GW`), `lib.rs` SpawnConfig | L | **JIT-core** | C1 (hosts) for reach-by-name |
| C3 | **engine-dedup PR-2 — dispatch hooks (aarch64, inert)** (`engine-dedup.md` §D) | Refactor `jit/dispatch.c` to call `G_IBTC_FILL`/`G_BLOCK_EXIT`/`G_SHADOW_CLEAR`/`G_DISPATCH_DEBUG`; aarch64 abi.h defines them as today's inline code → byte-identical aarch64, x86 untouched. | `jit/dispatch.c`, `frontend/aarch64/abi.h` | M | **JIT-core** (aarch64 gate) | PR-1 (done). Precedes PR-3/PR-4 |
| C4 | **x86-perf Phase 1 — `sub/cmp → Jcc` fast path** (`x86-perf.md` §7) | Pending-flags state machine for `PF_SUB/PF_CMP` w∈{4,8}; drop `e_nzcv_load` at the ~3 Jcc sites, `materialize_to_membank` before chain exit. ~3 sites, revertible, −67% on the common shape. | `frontend/x86_64/translate.c` | M | **JIT-core** (x86 rows) | independent; inherits C3/C6 later |
| C5 | **tier2 Phase 1 — degenerate 2-block trace** (`tier2-optimizer.md` §10) | New `jit/trace.c`; heat counter in `emit_chain_exit` behind `TIER2`; form a 2-block identity-mapped trace, one dispatcher side-exit; `R_TRACE_HEAD` hook in `run_guest`. Matrix byte-identical with `TIER2` unset. | new `jit/trace.c`, `jit/emit_arm64.c`, `jit/dispatch.c`, `targets/linux_aarch64.c` | L | **JIT-core** (aarch64) | conflicts with C3 on `dispatch.c`/emit — sequence after C3 |
| C6 | **sentry-split Phase 1 — ring + file-I/O family behind flag** (`sentry-split.md` §7) | `DD_UNTRUSTED`/`DD_ROLE` plumbing; new `os/linux/ring.c`; `syscall_route(c)` replaces `service(c)` at `dispatch.c:122`, routes only read/write/open/close/lseek/fstat to a sentry process; lockdown stubbed. Default off ⇒ matrix unchanged. | new `os/linux/ring.{c,h}`, `jit/dispatch.c`, `os/linux/service.c` (callable handler bodies), `main()` sentry branch, `lib.rs`/`runtime.rs`/`targets/linux_aarch64.c` | L | **JIT-core** | conflicts with C3/C5 on `dispatch.c`, with B-chain on `service.c` |

> **Engine-dedup tail (after C3):** PR-3 (implement x86 hooks, code-motion) then PR-4 (x86 TU swaps to
> `#include jit/dispatch.c`, delete `frontend/x86_64/dispatch.c`). Queue these once C3 is green; both
> JIT-core, matrix-gated, sequential.

---

## (D) Deep bugs — research before implementation

Each needs a root-cause/fault-capture spike before code. JIT-core; matrix-gated. Highest leverage first.

| # | Bug | What's known / the spike | Files (suspected) | Eff | Deps |
|---|-----|--------------------------|-------------------|:---:|---|
| D1 | **non-PIE `ET_EXEC` full fix** (the GCC-toolchain crash) | Mechanism confirmed: dense post-`fork` layout + un-relocated absolute refs into a biased image → SIGSEGV. Partial fix landed (execve teardown + PC-redirect). Achievable full fix: **shrink `__PAGEZERO` so non-PIE pins low, force every *other* mapping (PIE image/interp, heap, stack, anon mmap) to a high hint** so the PIE world is unchanged. (Prior global shrink broke 43/195 PIE → must be selective.) | `os/linux/elf.c` (`load_elf`, `g_nonpie_*`), `targets/linux_*.c` (`-pagezero_size`, high-hint mmap), `service.c` (mmap hinting), `hal` | L | unblocks `compile` group (gcc/g++/cc1 XPASS) |
| D2 | **busybox flaky fork+exec SIGSEGV** | DISTINCT from D1 (busybox is PIE, immune to the bias). A fork+exec race under matrix load; `diag_crash` never fires even with `SA_ONSTACK` → fault bypasses POSIX handler → **suspect the Mach exception path**. Spike: capture via the Mach exception port, not the signal handler. | `os/linux/signal.c`, `thread.c`, fork/exec in `service.c` (220/221), `hal` Mach exception path | L | independent of D1 |
| D3 | **redis startup crash** | DISTINCT (redis-server is PIE). Crashes post-config, pre-listener; jemalloc `MADV_DONTNEED` unsupported warnings. Spike: faulting-address capture; likely resolved or narrowed by **B2 (MADV_DONTNEED)** then re-test eventloop/thread-init. | depends on capture; `service.c` madvise + eventloop/thread paths | M | **do B2 first**, then re-diagnose |
| D4 | **postgres never ready** | DISTINCT (PIE). setuid/gid drop works; gap is daemonize (**B1 `setsid`**) + possibly fork+exec during initdb (overlaps D1/D2). Spike: land B1, re-test; if still failing, capture the initdb fork+exec fault. | `service.c` (setsid), then fork+exec path | M | **do B1 first**; may need D1/D2 |
| D5 | **RWX / guest-JIT pages (`soak/smc`)** | `mmap(RWX)` → EPERM under macOS W^X. Needs the runtime to back RWX/`PROT_EXEC` maps with `MAP_JIT` (or RW+RX dual-map) and **re-translate on writes to executable pages** (the `clear_cache`/coherency path soak patches 200k×). Unblocks JVM/V8/Node/LuaJIT/.NET/PyPy. | `service.c` mmap/mprotect, `hal` W^X/MAP_JIT, `jit/cache.c` invalidation | L | independent; large |

> **Off the work-list (platform limits, not bugs to fix on macOS host):** non-PIE *as a class*
> beyond D1's mitigation, cpu/io cgroup throttling, `pidfd`, `io_uring`. Host-limited syscalls
> (`mq_*`, `timer_create`/`timer_*`) stay ENOSYS or ride kqueue if ever needed — not queued.

---

## Recommended WAVE plan

Goal: maximize parallel agents while never letting two touch a shared serializing file in the same
wave. The hard serializers are **`service.c`** (all of B + C2/C6/D), **`dispatch.c`** (C3/C5/C6),
and the Rust **`main.rs`/`containers.rs`/`model.rs`/`runtime.rs`**.

### Wave 1 — maximum parallel fan-out (no JIT-core, minimal Rust collisions)
Run as independent agents, grouped so each `containers.rs`/`model.rs`/`main.rs` touch is owned by
exactly one agent in the wave:
- **Agent W1a (events):** A1 — owns `main.rs` + event emit sites.
- **Agent W1b (container fidelity):** A4 + A5 + A6 + A10 — owns `containers.rs` + the container
  fields in `model.rs` + `runtime.rs` (bundle the container-touching daemon tasks into one agent to
  avoid `containers.rs` conflicts).
- **Agent W1c (images/build):** A7 + A8 + A12 — owns `build.rs`/`images.rs` + Image struct in
  `model.rs`. *(A8 and W1b both touch `model.rs`; if run truly concurrently, give `model.rs` to one
  and have the other rebase — or fold A8 into W1b.)*
- **Agent W1d (volumes/cp/arch):** A2 + A9 + A11 — `volumes.rs`/`archive.rs`; A2/A9 touch
  `containers.rs` → **must sequence after or merge into W1b** (flag this; `containers.rs` is the bottleneck).
- **Agent W1e (netstack PR2):** C1 — daemon-only `/etc/hosts`; touches `runtime.rs`/`containers.rs`
  → also fold into W1b's ownership or sequence.
- **Agent W1f (JIT service batch — serial):** B1 → B2 → B3 → B5 → B6, one PR each on `service.c`,
  matrix-gated. This is the *single* JIT-core lane in Wave 1 (only one allowed).

> `containers.rs` is the daemon bottleneck (A2/A4/A5/A6/A9/A10/C1 all touch it). Realistically assign
> **one** "containers" agent (W1b) and queue A2/A9/C1 behind it, rather than forcing parallel merges.

### Wave 2 — the JIT-core subsystem PRs (mutually exclusive on `dispatch.c`/`service.c`)
These cannot run concurrently with each other (shared `dispatch.c`/`emit`/`service.c`) — run **one at
a time**, each fully matrix-gated, but they *can* overlap with leftover Wave-1 daemon work:
1. **C4 (x86-perf Phase 1)** — only touches `frontend/x86_64/translate.c`; **can run parallel to a
   daemon agent and even to C3** (different files). Lowest-risk JIT win → do first.
2. **C3 (engine-dedup PR-2)** — `jit/dispatch.c` + aarch64 abi.h. Must precede C5/C6/engine-dedup PR-3.
3. **B-chain remainder** (B7, B8, B4, B9–B15) on `service.c` — serial, can interleave between C-PRs
   since C3 doesn't touch `service.c` (but B4 touches Rust `containers.rs` → coordinate with Wave 1).
4. **C2 (netstack PR3, virtual switch)** — `service.c` + new `netbridge.c`; serialize against the
   B-chain (shared `service.c`). High value (`docker-net.sh` 7/7).

### Wave 3 — the heavy JIT-core + deep bugs (sequential, each its own matrix gate)
Strictly one at a time; all share `dispatch.c`/`service.c`/elf/hal:
1. **C5 (tier2 Phase 1)** — after C3 (shares `dispatch.c`/emit).
2. **C6 (sentry Phase 1)** — after C3/C5 (shares `dispatch.c`), coordinate `service.c` with B-chain.
3. **D3 (redis)** — after B2; **D4 (postgres)** — after B1; both can spike in parallel (research
   only) but land sequentially.
4. **D1 (non-PIE full fix)** and **D2 (busybox race)** — the two hardest; sequence last, one at a
   time, full matrix each. **D5 (RWX/MAP_JIT)** independent, schedule whenever a JIT-core slot is free.
5. **engine-dedup PR-3 then PR-4** — after C3, fold `dispatch.c` into the shared engine.

### One-line wave summary
- **Wave 1:** ~5 daemon agents (collapsed around the `containers.rs` bottleneck) **+ 1 serial
  JIT-`service.c` batch**. Highest parallelism, lowest risk.
- **Wave 2:** the cheap JIT wins — C4 (isolated x86 file, parallelizable) then C3 (dispatch hooks),
  netstack PR3 + B-chain remainder serialized on `service.c`.
- **Wave 3:** tier2, sentry, and the deep bugs — strictly serial, one matrix gate each, D3/D4 gated
  on their B-prereqs.
