# dd — todo

Shipped through **v0.9.0**. Open work below (fixed items are removed as they ship; update this
file after each batch).

> NOTE: the daemon resolves the engine via `resolve_bundled` → installed `/Applications/dd.app`
> unless `DDJIT_DIR` is set. Install the new DMG for fixes to take effect.

## Open — `[ ] ID — issue — why`
- [ ] **#104 + #113 guest-JIT codegen mistranslated** *(deepest — blocks java/.NET/node-opt/mongosh)* —
  the engine mistranslates ONE core-path instruction that corrupts V8's Turboshaft `MachineLowering`
  pass so it DROPS `Store` ops from OSR-optimized code. Deterministic repro:
  `node --no-maglev --predictable -e 'let last=-1;for(let i=0;i<1000000;i++)last=i+1;console.log(last)'`
  → `12236` (should be `1000000`). Not SMC/W^X. Fix via differential tracing (qemu oracle) to pin the opcode.
- [ ] **#123 `docker run node node -e …` → "open: No such file"** — bash/uname run from the pulled
  node:latest rootfs but the `node` binary itself won't start (curated node_alpine runs `node -e` fine).
  Loader/interp path or V8 startup specific to the real image.
- [ ] **#124 postgres bring-up: find_my_exec self-path resolution** — `initdb` can't resolve its own
  absolute path when launched via PATH (`could not resolve path … to absolute form`) so it can't find
  the sibling `postgres` binary; blocks full postgres cluster bring-up (and pg_ctl/pg_dump). Binary loads
  fine (`postgres --version` → 16.14). Engine realpath/readlink or /proc/self/exe for a PATH-launched binary.
- [ ] **#117 flaky PIE fork+exec SIGSEGV** — post-fork execve ld.so faults on a GOT pointer relocated
  against the wrong base; ASLR-dependent. Needs a deterministic collision-checked base for the execve
  image (`translate/x86_64/elf.c`).
- [ ] **#119 mongosh 193MB node-SEA early crash** — `EXC_BAD_ACCESS fault=0x0` while loading the SEA
  blob, before any eval (no longer hangs). Loader/mmap or V8-snapshot path.
- [ ] **#120 RFLAGS ID flag (bit 21) not modeled** — pushfq emits ID=0 / popfq discards it, so the
  toggle-ID CPUID-detection handshake fails for 32-bit guests/libs. Ready patch (needs cpu_x86_64.h field).

## Deferred / non-blocking
- [ ] **#78** stage `/hello.c` into the gcc-bundle rootfs (test fixture)
- [ ] **#93** host-asm encoder de-dup (riskiest refactor)
- [ ] **#94** README benchmark numbers

## Test-env notes (not code bugs)
- Host `~/.docker/config.json` has bogus Hub creds (`nouser:bad`) → real Docker Hub pulls 401 unless
  bypassed (`DOCKER_CONFIG=<empty dir>`).
- Shared `poc/images/*` rootfs are mutated by concurrent agents → volume docker.sh tests flake under
  parallel runs; confirm volume tests with a standalone docker.sh.
- Daemon-using lanes (docker.sh, scenarios) share `dd-scenarios.sock` → cannot run concurrently.

## Process
- Each fix: verify on the real path → batch → gate (basics both arches + docker.sh) → tag → push.
- Re-run the scenario triage on the fresh engine periodically to surface new real-software failures.
