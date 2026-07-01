# dd — todo

Shipped **v0.9.0 → v0.9.5** (node/V8 **72× faster** SMC fix, postgres SELECT=42, x87/R, apt id-drop,
compose, IPv6, x86 flags, macOS cd.., faster tests). **v0.9.6 batch staged** (8 fixes, gate-pending).
Real software working: postgres, redis, R, jq, node, java/.NET host, git, sqlite, nginx, openssl,
tar(arm), gcc/clang(arm), sed/awk/grep, all event/fd syscalls.

> **FIXED (9d20e08):** the #181 chown merge had reverted fs.c's #179 (kept fixed `char nm[1024][256]`
> + a 4-arg caller) while overlay.c had the new allocating 3-arg `overlay_readdir` → linux engine
> wouldn't compile; half-built siblings served corrupted (empty) readdir names. fs.c now holds heap
> pointers + frees the snapshot. Both engines build clean; glob/sentry-fs prove names return.
> **Build race:** concurrent agents `cargo clean -p ddjit` in the shared target wipe each other's
> engines. Each agent MUST use an isolated `CARGO_TARGET_DIR=target-<slug>`. Gate when builders idle.
> **Env doesn't cross the OrbStack bridge** to the mac engine — use file-gates `poc/runtime/jit86/*`
> (NOIBTC, FAULT_ON, …) for diagnostics, not env.

## v0.9.6 staged (verify when builders idle → tag)
IPv6 loopback · perf-x86 PF/AF (1.71×) · perf-IBTC 8Ki→64Ki (40×) · perf-syscall · overlay 1024-cap ·
x86 LOOP(0xE0-3) · chown-via-xattr · overlay `.`/`..`.

## THE big one (in progress)
- [ ] **#176/#117/#155 x86 IBTC stale across execve** *(agent active)* — the IBTC serves a freed host
  body when a fork+exec'd child reuses guest PCs → branch-to-0x1 SIGSEGV. Empirically isolated (NOIBTC
  → 0/10 vs 10/10). Blocks **apt, tar czf, clang -O2, git, rustc, cargo, go build**. Fix = flush g_xibtc
  on execve + reset FS/TLS base (clang `fs:[0x28]` clue). Closes a whole class.

## Open bugs (dev-day + tail)
- [ ] **#185 procfs incomplete** — /proc/self/cmdline,/comm,/fd/N readlink,/mountinfo,/proc/<pid> missing
  → ps shows nothing, df / fails. (vfs proc synth)
- [ ] **#186 cmake configure fails** — gmake can't `include` a flags.make that exists on disk; fs
  path-cache/fscache coherence under cmake churn (FS-mutating-syscall epoch misses a case). Blocks cmake.
- [ ] **#187 JVM hangs at startup on arm** — `java -version` no output even -Xint; futex/thread or a
  blocking /proc/cgroup read. No Java on arm.
- [ ] **#188 x86 CPUID feature flags missing** — /proc/cpuinfo + CPUID advertise no sse2/sse/mmx (insns
  work) → JVM "SSE2 not supported", numpy/ffmpeg abort. (translate-x86 CPUID + cpuinfo; behind #176)
- [ ] **#183 x86 0x8c MOV r/m,Sreg (+0x67)** unimplemented — secondary tar/gzip.
- [ ] **#161 postgres fast-shutdown SIGSYS** (forked child kevent retry; held/signal-fork).
- [ ] **#175 npm/ruby hang via daemon** (forked child exit not observed → rc=124).
- [ ] **#167 amd terminal/jobctl** container exits early.
- [ ] **#104 V8 store-drop** (Turboshaft, ET_DYN) · **#135 PyPy x86 JIT** · **#119 mongosh SEA**.
- [ ] **#158 memcached** libevent listener.

## Infra / deferred
- [ ] **#171 docker.sh gate** — pin daemon to current mac engine (placement). **#178 PCACHE execve** gap.
- [ ] **#170 mkdir-EPERM** (likely moot — apt is #176; re-confirm). **#93** encoder de-dup · **#78** fixture.

## Process
- dev-day = fast/shallow reporter (symptom+repro, no diagnosis) → fix agents root-cause+fix, isolated
  build dirs. Each fix: agent-verify → merge → (gate when idle) → batch → tag.
