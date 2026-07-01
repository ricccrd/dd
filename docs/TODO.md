# dd — todo

Shipped **v0.9.0 / v0.9.1 / v0.9.2** (≈28 fixes this cycle, from the real-software discovery sweep #125).
Most apps now run: redis (serves), jq, etcd/go, julia, node-x86, sha256sum, zstd, buildkitd, haproxy,
the interpreters; postgres runs initdb + start + connect (one blocker left). Open work below.

> Daemon resolves the engine via `resolve_bundled` → installed `/Applications/dd.app` unless `DDJIT_DIR`
> set. Install the new DMG for fixes to take effect.

## In progress
- [ ] **#160 postgres last blocker** *(agent active)* — AF_UNIX getsockname/getpeername mis-classifies the
  unix connection → pg_hba "no entry for host '???'". Everything else (initdb/start/connect) works.

## Deep / hard (engine codegen)
- [ ] **#104 V8 store-drop** — Turboshaft MachineLowering drops Stores under OSR (java/.NET/node-opt). ET_DYN/PIE.
- [ ] **#117 flaky x86 fork+exec** — execve re-translation can't tolerate a reused guest image base (stale
  code-cache blocks) + x86 loader/stack untracked by gmap. ~0.4% under load. Needs engine execve-teardown fix.
- [ ] **#155 go build/run driver SIGSEGV** — heavy go toolchain; same x86-codegen class as #117/#104.
- [ ] **#135 PyPy x86 JIT** asserts (a 2nd guest-JIT).

## x86 opcode / flag gaps (ready or small)
- [ ] **#136 base64 -d** — `xchg %ch,%cl` (byte-reg with high-byte operand) corrupts host regs. READY PATCH for translate.c:1041.
- [ ] **#153 x87 FNSTENV (D9 /6) + FLDENV (D9 /4)** — R/OpenBLAS.
- [ ] **#145 x86 flag residuals** — ror %cl CF, imul CF, shift OF(count==1).
- [ ] **#120 RFLAGS ID flag** (32-bit CPUID detect, ready patch needs cpu_x86_64.h field).
- [ ] **#138 git write-tree** wrong hash (re-test after #128 SHA-NI — likely fixed).

## Loaders / hosts / misc
- [ ] **#131 .NET host** exe-path empty (re-test after #124). **#139 clang** link fails both arches (arm64 SIGSEGV).
- [ ] **#119 mongosh** 193MB node-SEA early crash. **#123 node-via-daemon** ENOENT.
- [ ] **#147 redis-arm crash** — re-test (procfs #143 + cloexec #157 likely fixed the root).
- [ ] **#158 memcached** libevent listener. **#159 IPv6 (::) loopback** redirect (redis dual-stack).

## Deferred
- [ ] **#78** gcc-bundle /hello.c fixture · **#93** host-asm encoder de-dup · **#94** README benchmarks

## Process
- Each fix: verify on the real path → batch → gate (basics both arches + docker.sh) → tag → push. Keep current.
- Note: full basics matrix runs get SIGTERM'd under heavy agent load (bridge contention) — run when agents idle.
