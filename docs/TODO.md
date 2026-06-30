# dd — todo

Shipped through **v0.9.0**. Open bugs below, grouped by nature. Most arm64 paths are solid; the
bulk of open work is **x86_64 heavy-software** (codegen/opcodes) + a few cross-arch os/loader gaps.
Surfaced by the real-software discovery sweep (#125). Update after each batch.

## Cross-cutting / highest impact
- [ ] **#104/#113 guest-JIT store-drop** — engine mistranslates one core-path instruction so V8's
  Turboshaft MachineLowering drops `Store` ops under OSR. Det. repro `node --no-maglev --predictable
  -e 'let s=0;for(let i=0;i<1000000;i++)s+=i;console.log(s)'`. Likely the SAME root as **#132** (jq/ld/git
  x86 SIGSEGV/heap-corruption in plain C — a dropped store corrupts heap metadata). Blocks java/.NET/node-opt.
- [ ] **#129 overlay-lower file mmap broken** *(arch-independent)* — file-backed mmap of a file that
  lives only in a read-only image lower fails; java jimage mmap aborts (read() is byte-correct). Likely
  also **#133** (Julia sysimage SIGBUS). High value. os/linux mem.c + overlay fd.

## x86_64 opcode gaps (translate/x86_64 — hold behind #104)
- [ ] **#128 SHA-NI (0F38 C8–CD) + PTEST (66 0F38 17)** — breaks sha256sum/openssl/python-hashlib + node:alpine.
  NOTE: #115 advertises SHA in CPUID but the opcodes aren't implemented → software uses them and crashes. Implement them.
- [ ] **#137 VEX BMI1 ANDN (0F38 F2)** — zstd decompress fails. Check the whole VEX BMI1/BMI2 group.
- [ ] **#136 base64 -d SIMD shuffle miscompile** — busybox base64 decode scrambles bytes (x86 only).
- [ ] **#130 non-PIE Go loader** — textStart not honored after high rebase → moduledataverify fatal.
- [ ] **#138 git write-tree → empty-tree hash** (x86) — likely #128 SHA; re-test after.

## Loader / host / process
- [ ] **#131 .NET host** — "Failed to resolve full path of the current executable []" + segfault. Re-test after #124.
- [ ] **#117 flaky PIE fork+exec SIGSEGV** — post-fork execve ld.so GOT relocated against wrong base (ASLR-dependent).
- [ ] **#119 mongosh 193MB node-SEA early EXC_BAD_ACCESS.**
- [ ] **#123 `docker run node node -e`** via daemon → "open: No such file" (node binary won't start).
- [ ] **#139 clang full link fails both arches** — clang_17 arm64 SIGSEGV; clang_18 x86 can't exec sub-tool.

## Signal / syscall / env
- [ ] **#126 SIGCHLD-handler crash** *(agent active — postgres blocker)* — fault delivering SIGCHLD to initdb's
  handler after fork+exec+sigmask.
- [ ] **#127 execve discards guest envp** — `export FOO; exec env` loses it (proc.c case 221 ignores a2).
- [ ] **#140 aarch64 chroot (syscall 51) unhandled** — buildkitd.
- [ ] **#120 RFLAGS ID flag (bit 21)** not modeled — 32-bit CPUID-detection handshake fails.

## Runtime-specific
- [ ] **#134 R hangs at startup** (R --version → timeout). **#135 PyPy x86 JIT backend asserts** (2nd guest-JIT).

## Deferred / non-blocking
- [ ] **#78** gcc-bundle /hello.c fixture · **#93** host-asm encoder de-dup · **#94** README benchmarks

## Test-env notes (not code bugs)
- Host `~/.docker/config.json` has bogus Hub creds → real pulls 401 unless `DOCKER_CONFIG=<empty>`.
- Daemon-using lanes (docker.sh, scenarios) share `dd-scenarios.sock` → not concurrent. Shared poc/images
  rootfs are mutated by parallel agents → confirm volume/fs tests standalone.
- Engine-direct overlay tests: upper MUST be under the shared project dir (not /tmp); direct mode doesn't
  forward the image PATH (export it in-guest).

## Process
- Each fix: verify on the real path → batch → gate (basics both arches + docker.sh) → tag → push. Keep this file current.
