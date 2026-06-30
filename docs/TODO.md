# dd — todo

Shipped through **v0.8.9**. The earlier triage's "~230 failures" was a stale-engine artifact —
its biggest groups are all fixed. Remaining open work below.

> NOTE: the daemon resolves the engine via `resolve_bundled` → installed `/Applications/dd.app`
> unless `DDJIT_DIR` is set. Install the new DMG for fixes to take effect.

## Open — `[ ] ID — issue — why`
- [ ] **#104 + #113 guest-JIT codegen mistranslated** *(agent active — deepest, all JIT langs)* —
  node V8 / .NET RyuJIT / java hot loops crash/miscompute (x86 1M-array truncates flaky; arm
  `EXC_BAD_ACCESS fault=0x1`). Engine mistranslates code the guest JIT writes at runtime →
  SMC / freshly-written code-page invalidation gap. Both arches.
- [ ] **#115 x86 CPUID missing SSE2** *(agent active)* — java aborts "SSE2 not supported"; CPUID
  feature leaf under-reports the mandatory x86-64 baseline.
- [ ] **#101 PID-1 self-signal** *(agent active)* — mongosh/node hang at exit; init drops its own
  re-raised SIGTERM (PID-1 ignores unhandled fatal signals).
- [ ] **#117 flaky PIE fork+exec SIGSEGV** *(after guest-JIT frees elf.c)* — post-fork execve ld.so
  faults on a GOT pointer relocated against the wrong base; ASLR-dependent. Needs a deterministic
  collision-checked base for the execve image (`translate/x86_64/elf.c`).

## Deferred / non-blocking
- [ ] **#78** stage `/hello.c` into the gcc-bundle rootfs (test fixture)
- [ ] **#93** host-asm encoder de-dup (riskiest refactor)
- [ ] **#94** README benchmark numbers

## Process
- [ ] Re-run the scenario triage on the fresh engine → confirm zero genuine failures; sweep residue.
- [ ] Each fix: verify on the real path → batch → gate (basics both arches + docker.sh) → tag → push.
