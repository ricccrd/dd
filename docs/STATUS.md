# dd — live gap inventory (coverage, rechecks, harness)

Companion to [`PLAN.md`](PLAN.md). **The canonical open-bug table lives in [`PLAN.md` PART A](PLAN.md#part-a--known-bugs--whats-missing-or-failing)** — do not duplicate engine bugs here.
This file tracks the *test-lane* state: coverage gaps not yet authored, xfail markers to recheck/clear,
the harness facts that drive the framework, and the bug-filing protocol. Resolved rows are **deleted**
(see git history), not archived.

Every divergence a test surfaces gets an `xfail` (so the gate stays green) + a row in PLAN PART A + a
diagnostic agent. When the engine lane fixes it, the case flips XPASS and the row is **removed** from PLAN.

## Stale xfail markers to CLEAR (recon saw XPASS — engine already fixed)

`edge/madvise` [aarch64,x86_64] · `edge/renameat2` [aarch64,x86_64] · `edge/fallocate` [aarch64,x86_64] ·
`edge/lseekhole` [aarch64,x86_64] · `edge/times` [aarch64]. Remove the `.xfail()` so a future regression re-fails.

## Recheck (gap was likely closed upstream — re-run, then clear or re-file in PLAN)

- `weird-python-slim-amd` — python:3.12-slim was xfail for `jit86-opcode-1c` (byte ADC/SBB), now fixed → recheck amd64.
- `weird-tsc/vdso/hwcap` — tsc-counter / vdso-clock / auxval-hwcap / simd-probe were gated by the toolchain
  compile; cc1+as now work (only the link, `collect2-link`, remains) → recheck.
- `weird-toolchain-suite` — gcc driver+cc1+as work; once `collect2-link` lands, the ~21 deeper `weird/*`
  probes unlock and reveal their next layer.

## Coverage gaps — categories / round-trips not yet authored

| category | not yet covered | why |
|----------|-----------------|-----|
| databases | etcd put/get + nats pub/sub round-trips | `quay.io/coreos/etcd` and `nats:latest` are FROM-scratch (no `/bin/sh`) → the `exec` bring-up path is impossible; only run-form `--version` banners fit one container. A live round-trip needs a busybox-based image or multi-container scenario support. |
| databases | mysql/mariadb/mongo markers verified live | postgres/redis/valkey/memcached/nats/etcd/couchdb/influxdb verified vs the Real docker oracle; the huge slow-boot images rely on `testing/IMAGE-MANIFEST.md` §3 pinned markers — verify live next nightly. |
| web | first `--backend dd` run of the NEW stress paths | 35 scenarios / 70 cases green on the Real oracle (arm); caddy/traefik (Go schedulers) + varnish (VCL→C dlopen) are new dd paths — watch the first dd run. (httpd is xfail via `exec-loader-noent`.) |
| languages | first `--backend dd` run of JIT-in-JIT | 51 scenarios / 102 cases green on the Real oracle (arm). JVM/V8/CoreCLR (javac/dotnet SDK, V8 hot loops) are PRIME engine stress — esp. .NET SDK (RyuJIT, RWX heaps), **not** yet xfail'd. amd64 markers unverified live (Docker Hub rate-limit) but arch-invariant. |
| languages | JIT-in-JIT on **arm-mac**; extra runtimes (Kotlin/Scala, Deno/Bun, Crystal, GHC, OCaml, Zig, Swift, R, Julia, LuaJIT) | Linux-only until MAP_JIT path is exercised broadly; openjdk:*-slim tags removed from Docker Hub → substituted eclipse-temurin. |

## Harness fact (not a bug — drives the framework design)

On the linux dev host the daemon is a **Mach-O binary run mac-side via the `mac` bridge**: env is dropped
crossing the bridge and the daemon socket lives mac-side (a linux `docker` can't reach it). The Rust scenario
harness therefore drives the daemon **and** `docker` through `mac bash <script-file>` (env inline,
socket/state under a `/Users` shared path), mirroring `lib.rs`. On a real macOS host it runs direct.

## Completeness probe baseline

`cases/ext/completeness.rs` (96 compiled guests: 48 syscall, 26 x86-64 opcode, 22 aarch64 opcode — no docker
images) `.oracle()`-diffs JIT vs native. The 22 aarch64 opcode guests (NEON + crypto/CRC/LSE/FP16/dotprod/
i8mm/bf16) all PASS — the aarch64 backend's instruction coverage is complete for everything probed. Remaining
x86_64-translator UNIMPL / syscall gaps are folded into PLAN PART A. (NB `comp-sys-proc/clone3` xfails on
x86_64 only because the qemu-user *oracle* lacks clone3 — the dd engine handles it; not an engine gap.)

---

## Bug protocol (for builder agents)

When a case fails on a target and it's an engine issue (not a flaky test):
1. Add `.xfail(&[Target::ArmLinux])` (or the affected target) to the `scen(...)` builder, with a comment
   referencing the PLAN PART A id, so the gate stays green and XPASS fires when it's fixed.
2. Add/refresh the row in [`PLAN.md` PART A](PLAN.md) (id / target / symptom / suspected file).
3. Report the bug in your final message so the orchestrator spawns a diagnostic agent.

Never edit dd-jit engine C in the test lane. Tests document; diagnostic agents root-cause; the engine lane fixes.
