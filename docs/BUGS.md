# dd — open bugs (consolidated)

Shipped through **v0.8.8**; v0.8.9 in progress. The earlier scenario triage's "~230 failures"
was measured on a **stale engine** — G1/G3/G4/G7 were already fixed, so the real count is far
lower. A fresh-engine re-run is needed for the true number.

> NOTE: the daemon picks the engine via `resolve_bundled` → the **installed `/Applications/dd.app`**
> unless `DDJIT_DIR` is set. Users must **install the new DMG** for any fix to take effect.

## Open bugs — `[ ] ID — issue — why`

- [ ] **#101 PID-1 self-signal** — mongosh / node hang at exit. node's `SignalExit` re-raises
  `SIGTERM` to itself, but as PID 1 with no handler the engine drops it (Linux PID-1 ignores
  unhandled fatal signals), so node never terminates. Need correct PID-1 self-fatal-signal semantics.
- [ ] **#104 + #113 guest-JIT codegen mistranslated** — node V8 (TurboFan) and .NET (RyuJIT) hot
  loops crash/miscompute (x86: 1M-array fill truncates, flaky; arm: `EXC_BAD_ACCESS fault=0x1`).
  The engine mistranslates code the **guest** JIT writes at runtime → SMC / freshly-written
  code-page invalidation gap. Both arches; affects every JIT language (node/.NET/java).
- [ ] **#115 x86 CPUID missing SSE2** — java (temurin) aborts `"SSE2 not supported"`. The CPUID
  feature leaf doesn't advertise SSE2 (and likely sibling bits) the guest probes.
- [ ] **#114 daemon `docker exec` drops stdout** — `docker exec … 'gcc --version | head'` returns
  empty though it works run directly. The daemon's exec-path stdio/pipe plumbing (or PATH) loses output.
- [ ] **#106 interactive job control** *(agent active)* — `ls` in `docker run -it` bash → `[N]+
  Stopped`. v0.8.8 fixed `getpid()==1` (the keystone); the remaining piece is the real-TTY
  `tcsetpgrp`/SIGTTOU foreground-group handoff (guest↔host pgrp translation). Verifying under a real pty.
- [ ] **#109 dynamic-ELF interpreter load** *(agent active)* — node/python/ruby *slim* fail
  `open: No such file` loading ld.so under the daemon overlay. Interp-path resolution in the
  overlay layers (possibly a stale-engine artifact; verifying on the fresh build).

## Deferred / non-blocking
- [ ] **#78** stage `/hello.c` into the gcc-bundle rootfs (test fixture)
- [ ] **#93** host-asm encoder de-dup (riskiest refactor)
- [ ] **#94** README benchmark numbers

## Process
- [ ] **Re-run the scenario triage on the FRESH engine** (`DDJIT_DIR` pinned) for the true
  remaining-failure count — the ~230 was inflated by a stale-engine measurement.
