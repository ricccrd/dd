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
- **aarch64 SQLite parity / beat-native** → [`arm-sqlite-parity.md`](design/arm-sqlite-parity.md) (measured).
  Codegen is native-quality (sha256 1.01×, int-sieve 0.72×); the gap is **VDBE dispatch + indirect-branch
  misprediction**, NOT probe length (the OoO core hides the IBTC mem-ops — a monomorphic loop is already
  1.01× native). Three measured levers, **ready as gated diffs** — see the *Landing guide* below.
- sub-ms startup — the fork-server residual is `fork()` of the large-VM reservation + the guest `ld.so`.
- (tier-2 multi-block: investigated — already realized by the opt4 superblock former + the self-loop fold.)

### Landing guide — ARM SQLite levers (apply in order A3 → A1 → B1; → B2)
Gated, bit-exact diffs + per-lever reports live in `docs/design/`: [`arm-a3.diff`](design/arm-a3.diff)
([report](design/arm-a3.md)), [`arm-a1.diff`](design/arm-a1.diff) ([report](design/arm-a1.md)),
[`arm-b1.diff`](design/arm-b1.diff) ([report](design/arm-b1.md)). Apply on a branch; **rebuild + verify
after each** (`make test` matrix green ×3 engines, `make test-diff` byte-exact vs qemu/native,
`make test-gates` each new gate on==off incl. stacked, + the bench harness vs native).
**Apply-path note:** `arm-a3.diff` paths are `dd-jit/src/…` → `git apply`/`patch -p1` from repo root `dd/`;
`arm-a1.diff` & `arm-b1.diff` paths are `src/…` → apply from `dd/dd-jit/` (or `git apply --directory=dd-jit`).

**1. A3 — §B-off + 16-byte block alignment · SHIP FIRST (the 1.93×→1.47× win, bit-exact, strictly safer).**
Default-disables §B shadow-return (net pessimization: ~40 host-insn push+validate vs the cheap IBTC return
returns already hit); gate `NOSHADOWTUNE=1` restores old §B for A/B. Touches `frontend/aarch64/
{dispatch_hooks.h,translate.c}`, `frontend/x86_64/dispatch_hooks.h`, `jit/{cache.c,dispatch.c}`,
`os/linux/service.c`. Clean vs HEAD (only overlaps A1 on `aarch64/dispatch_hooks.h` — trivial). Verify sqlite
≈1.47× native, sha256/int-sieve flat.

**2. A1 — steal host x16/x17 · SHIP (cleanup *and* the B1 enabler).** Adds x16/x17 to `is_stolen()` so the
IBTC hit path drops the red-zone stash/restore (small alone, but it uncaps B1's threaded path). Gate
`NOSTEAL1617=1`. Touches `include/cpu_aarch64.h`, `frontend/aarch64/dispatch_hooks.h`, `jit/emit_arm64.c`,
`targets/linux_aarch64.c`. **Conflict with A3:** only `aarch64/dispatch_hooks.h` — merge both hunks (keep A3's
§B-off hook + A1's stolen-set). Accept the minor LSE-suppression on 2 cold x16/x17-addressed atomics.

**3. B1 — VDBE meta-trace · BUILD OUT (stack ON TOP of A3+A1).** `arm-b1.diff` is the gated **prototype**
(`VDBETRACE=1`): speculatively inlines the stable VDBE dispatch target (exact 64-bit guard + correct
fallback), bit-exact, +0.77% at ~15% coverage. Touches `frontend/aarch64/translate.c`, `jit/{cache.c,
dispatch.c,emit_arm64.c}`, `os/linux/service.c`, `targets/linux_aarch64.c` — **overlaps A3** (translate.c,
cache.c, dispatch.c, service.c) **and A1** (emit_arm64.c, targets). Because of that overlap, **re-generate B1
against the A3+A1 tree** (rebase the prototype) rather than a blind 3-way patch. Its ~5%/beat-native ceiling
needs the **superblock recorder** (path-context order-3 specialization for coverage) — the build-out, not in
the diff.

**4. B2 — inter-opcode fold · FUTURE (no diff yet).** On the B1 trace, fold redundant VDBE Mem-cell
load/stores → the final push to **<1.0× (beat native)**. Stack A1+B1+B2 on the A3 baseline.

**Order rationale:** A3 is the standalone win; A1 enables B1; B1 removes the dispatch branch native itself
pays; B2 finishes it. Every lever is gated → land dark, flip after green.

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
