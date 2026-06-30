# dd — open bugs (consolidated)

Shipped through **v0.8.8**; **v0.8.9 staged** (6 fixes). The earlier triage's "~230 failures"
was a **stale-engine** measurement — its 4 biggest groups are now fixed (see below), so the
real remaining set is small. Re-run the triage on the fresh engine for the true count.

> NOTE: the daemon picks the engine via `resolve_bundled` → the installed `/Applications/dd.app`
> unless `DDJIT_DIR` is set. Users must **install the new DMG** for any fix to take effect.

## Fixed (shipped v0.7–v0.8.8 or staged for v0.8.9)
- G1 detached-PID-1 fork+exec (~130) — v0.8.8 `getpid()==1`.
- G2 bare-exec overlay resolve (~77) — node/python/ruby/php load. *(staged)*
- G3/G4 x86 byte adc/sbb CF/AF flags + clc/stc/cmc decode; getpgrp. *(staged)*
- G7 membarrier(283) — haproxy. *(staged)*
- mount-parent `ls /x` + docker coverage tests; coverage tool repaired. *(staged)*

## Open — `[ ] ID — issue — why`
- [ ] **#106 interactive job control** *(agent active)* — `ls` in `-it` bash → `Stopped`;
  v0.8.8 fixed getpid==1, remaining is the real-TTY `tcsetpgrp`/SIGTTOU pgrp handoff.
- [ ] **#104 + #113 guest-JIT codegen mistranslated** *(NEXT — deepest, all JIT langs)* — node
  V8/.NET RyuJIT/java hot loops crash/miscompute (x86 1M-array truncates flaky; arm
  `EXC_BAD_ACCESS fault=0x1`). Engine mistranslates code the **guest** JIT writes at runtime →
  SMC / freshly-written code-page invalidation gap. Both arches.
- [ ] **#115 x86 CPUID missing SSE2** *(NEXT)* — java aborts "SSE2 not supported"; CPUID feature
  leaf under-reports the mandatory x86-64 baseline.
- [ ] **#114 daemon `docker exec` drops stdout** *(NEXT)* — `exec …|head` empty (works direct);
  daemon exec-path stdio/pipe plumbing.
- [ ] **#101 PID-1 self-signal** *(after #106 frees signal.c)* — mongosh/node hang at exit; init
  drops its own re-raised SIGTERM (PID-1 ignores unhandled fatal signals).

## Deferred / non-blocking
- [ ] **#78** stage `/hello.c` into the gcc-bundle rootfs (test fixture)
- [ ] **#93** host-asm encoder de-dup (riskiest refactor)
- [ ] **#94** README benchmark numbers

## Next-work plan
1. Land **v0.8.9** (gate + tag) once #106 finishes.
2. Fan out: **#104+#113** (guest-JIT/SMC — one agent, both arches), **#115** (CPUID), **#114**
   (daemon exec) — disjoint, in parallel now; **#101** after #106.
3. **Re-run the triage on the fresh engine** → confirm zero genuine failures; sweep any residue.
4. Tag → push → re-run full coverage; repeat until the open list is empty.
