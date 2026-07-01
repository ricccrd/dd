# dd — todo

Shipped **v0.9.0 / v0.9.1 / v0.9.2**; **v0.9.3 batch staged**. Real software working end-to-end now:
**postgres (SELECT=42)**, redis, jq, etcd/go, julia, node-x86, java/.NET host, sha256sum, zstd,
buildkitd, haproxy, git, the interpreters. ~31 of ~33 swept bugs fixed. Remaining tail below.

> Daemon resolves the engine *beside the daemon* (`target/release/ddjit-linux_*`) then `/Applications/dd.app`.
> The release engine is NOT auto-rebuilt by `cargo build --release -p dd-daemon` (build.rs #include trap) —
> `cargo clean -p ddjit` first, or copy the fresh engine in, before any daemon/scenario run.

## In progress
- [ ] **#153 x87 FNSTENV (D9 /6) + FLDENV (D9 /4)** *(agent active)* — R/OpenBLAS.

## Deep / hard (engine codegen + signal-fork machinery)
- [ ] **#104 V8 store-drop** — Turboshaft MachineLowering drops Stores under OSR (java/.NET/node-opt). ET_DYN/PIE.
- [ ] **#117 flaky x86 fork+exec** — execve re-translation can't tolerate a reused guest image base. ~0.4% under load.
- [ ] **#155 go build/run driver SIGSEGV** — heavy go toolchain; same x86-codegen class as #117/#104.
- [ ] **#161 postgres fast-shutdown SIGSYS** — forked child re-issues kevent in-place after a host signal →
  macOS SIGSYS. Robustness-only (SELECT=42 works). signal.c/sigframe/fork — held/regression-prone (node/redis/mongosh).
- [ ] **#135 PyPy x86 JIT** asserts (2nd guest-JIT).

## x86 opcode / flag gaps (small)
- [ ] **#145 x86 flag residuals** — ror %cl CF, imul CF, shift OF(count==1). qemu differential.
- [ ] **#120 RFLAGS ID flag** (32-bit CPUID detect; ready patch needs cpu_x86_64.h field + pushfq/popfq).

## Loaders / servers / misc
- [ ] **#139 clang full link** — crash gone, but the link emits no runnable binary (`./h` not found). Driver→ld exec/output.
- [ ] **#119 mongosh** 193MB node-SEA early crash. **#123 node-via-daemon** ENOENT (re-test via daemon).
- [ ] **#158 memcached** libevent listener (not loopback). **#159 IPv6 (::) loopback** redirect.

## Deferred
- [ ] **#78** gcc-bundle /hello.c fixture · **#93** host-asm encoder de-dup · **#94** README benchmarks

## Process
- Each fix: verify on the real path → batch → gate (basics both arches + docker.sh) → tag → push. Keep current.
- Scenario lane: amd failures are a test-env image gap, not engine bugs; arm is the meaningful signal.
