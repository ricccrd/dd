# Research — redis:alpine & postgres:alpine startup crashes

Status: **research only (no fix applied).** Live capture against the already-built
`target/release/dd-daemon` + the prebuilt `ddjit-linux_aarch64`
(`target/release/build/ddjit-f5c3afad57f04f24/out/ddjit-linux_aarch64`), `CRASHDBG=1` in the
daemon env. Both images are PIE, so neither is the non-PIE `__PAGEZERO` ET_EXEC crash.

**TL;DR — these are two *distinct* new bugs, and neither is the busybox-tail fork+exec instability:**

| server | confirmed crash | root cause | file:line |
|---|---|---|---|
| redis:alpine (8.8.0) | `SIGBUS`/`EXC_BAD_ACCESS`, page-aligned fault, in `checkLinuxMadvFreeForkBug` | **`mprotect` is a NO-OP** so a `PROT_READ → PROT_READ\|PROT_WRITE` *upgrade* is silently dropped; the guest's next write hits a still-read-only page | `os/linux/service.c:1456` (case 226) + `:1448` (case 222) |
| postgres:alpine (18.4) | `EXC_BAD_ACCESS` **inside the JIT's `load_elf`**, 0 guest syscalls executed | **the initial program loader has no `#!` shebang handling** (only `execve` does); a script entrypoint (`docker-entrypoint.sh`) is parsed as a bogus ELF | `os/linux/elf.c:44` + `targets/linux_aarch64.c:310` |

How the JIT was driven for traces: on this Linux dev box the daemon launches the guest via the `mac`
bridge, which **drops ambient env** (`lib.rs:166-168` only re-injects `CRASHDBG`, never `JT`). So `JT`
syscall tracing was obtained by invoking the JIT binary *directly* through the bridge with the env in the
command line: `mac bash -lc "exec env CRASHDBG=1 JT=1 <jit> --rootfs <upper> --lower <rootfs> <prog> …"`.
Both binaries are **unstripped** (`with debug_info`), so guest PCs symbolize with `aarch64-linux-gnu-addr2line`.

---

## redis:alpine — `mprotect`-upgrade dropped → write to a read-only page

### Evidence

* `docker run … redis-server --save '' --appendonly no --daemonize no` → **exit 139**, deterministic
  (6/6). Last and only redis log line is the overcommit `# WARNING …` — never "Server initialized",
  never binds; `redis-cli ping` → connection refused.
* Bisection narrows it past the allocator and past generic memory:
  * `redis-server --version` → **exit 0** (`malloc=jemalloc-5.3.0`): jemalloc arena init is fine.
  * `redis-server --test-memory 1` → **exit 0**, 1607 lines: heavy mmap/read/write is fine.
  * full server start → crash. So the fault is in `initServer`-era **startup memory checks**, not the
    allocator and not the `madvise(MADV_DONTNEED)` no-op (jemalloc detects that via its own probe —
    "MADV_DONTNEED does not work (memset will be used instead)" — and degrades gracefully; that path is
    a red herring).
* Two fault reports, reconciled:
  * PID-1 redis (Mach exception port live): `[MACH] exc=0x1(EXC_BAD_ACCESS) fault=0x1 … off=0x0 ?` —
    `hpc` lands in the JIT code cache (`off=0`, sym `?`) i.e. **in translated guest code**; `x28` (the
    JIT pins real `x28` = `struct cpu*`, see `jit/emit_arm64.c:113`) is a fixed-offset main-thread
    stack address under ASLR → **main thread**. The `fault=0x1` is the Mach `code[]` slot the handler
    prints; the authoritative address is the POSIX one below.
  * busybox-forked redis (fork clears the child's Mach port, `service.c:1669`, so the POSIX handler
    fires): `[CRASH] sig=10(SIGBUS) fault=0x0000000101e6c000 pc=… tid=0x0` — **page-aligned**
    (`0x101e6c000` is 16 KiB-aligned), **main thread** (`tid=0`).
* `JT` trace of the exact moment (guest addrs; redis base `0x10d17c000`, ld-musl base `0x10bce4000`):
  ```
  [sys] 222 (0,c000,1)            mmap(NULL, 0xc000, PROT_READ, …)      ; 0xc000 = 3 × 16 KiB pages
  [sys] 226 (10cc44000,4000,3)    mprotect(mid page, PROT_READ|WRITE)  ; ← NO-OP, dropped
  [blk] pc=10d366b24 …            redis code, then →  [MACH] fault
  ```
  No `clone`/`fork` (220) precedes the fault.
* Symbol: guest pc `0x10d366b24` → redis `+0x1eab24` → **`checkLinuxMadvFreeForkBug`**
  (`nm`: `1eaa90 T checkLinuxMadvFreeForkBug`; addr2line: `/usr/src/redis/src/syscheck.c:209`).
  That function maps three pages, flips the middle page's protection with `mprotect`, then writes `*q`
  before forking — exactly the `mmap(PROT_READ)` → `mprotect(…RW)` → store sequence above.

### Root cause

`service.c` case **226 `mprotect` (line 1456) is an unconditional NO-OP** ("the JIT never executes guest
pages, so it doesn't enforce guest page protection; calling real `mprotect` is harmful — it could make
RELRO read-only and fault the guest's own writes"). That reasoning only covers *narrowing*
(RW→RO). It misses the *widening* case: case **222 `mmap` (line 1448) passes the guest's requested prot
`(int)a2` straight to the host** `mmap`, so a `PROT_READ` (or `PROT_NONE`) region is genuinely
read-only on the host. When the guest later `mprotect`s it **up** to `PROT_READ|PROT_WRITE`, the no-op
drops the upgrade, the page stays read-only, and the guest's next store SIGBUSes at the page address.
`checkLinuxMadvFreeForkBug` is simply the first code in redis startup to rely on an mprotect *upgrade*.
(python:alpine passes because it never does a restrict-then-widen mprotect dance.)

### Candidate fixes (ranked)

1. **Map guest anon memory permissively; keep mprotect a no-op.** In case 222, for anonymous maps force
   `prot |= PROT_READ | PROT_WRITE` (the JIT already declines to enforce guest page perms). The region is
   then always writable, the case-226 no-op is consistent, and the RELRO-narrowing fear the comment
   raises stays handled. Smallest, most in-keeping with the existing design; one line.
2. **Make mprotect widen-only.** Change case 226 to `mprotect((void*)a0, a1, (int)a2 | PROT_READ | PROT_WRITE)`
   — honor upgrades, never narrow (so RELRO can't fault legit writes). Fixes the symptom precisely; still
   one line, but issues a real syscall on a hot path.
3. Honor mprotect exactly as requested. Rejected: re-introduces the RELRO self-write fault the comment
   documents.

Fix #1 is preferred (matches the "don't enforce guest protection" stance and the case-222 guard-tail
philosophy already there).

### Cheapest confirming experiment

Add an `edge`-group guest microtest: `p=mmap(NULL, 3*PAGE, PROT_READ, MAP_PRIVATE|MAP_ANON); mprotect(p+PAGE, PAGE, PROT_READ|PROT_WRITE); p[PAGE]=1;`
— faults today, XPASSes after the fix. (Confirmation in this report is already conclusive: the `JT`
trace shows the store immediately after `mprotect(…,RW)` on a `PROT_READ` map, and the POSIX fault is the
page address.) Re-running `redis-server --save '' --appendonly no` should then reach
"Ready to accept connections" instead of exit 139.

---

## postgres:alpine — entry shell-script fed to `load_elf` as a bogus ELF

### Evidence

* `docker run … postgres:alpine /usr/local/bin/docker-entrypoint.sh postgres` → **exit 139**,
  deterministic. Crash report:
  ```
  [MACH] exc=0x1 fault=0x0000000020230a6d hpc=… x28=0x706970206f65452d off=0x00000000000019b8 load_elf
  ```
  * `off=0x19b8` + symbol **`load_elf`**: the fault is in the JIT's **own ELF loader** (host C code),
    not in translated guest code.
  * `x28 = 0x706970206f65452d` = ASCII `…oeE pip` — bytes of the script line `set -Eeo pipefail`.
    `fault = 0x20230a6d` = ` #\nm` — more script bytes. The "ELF header" being parsed is shell text.
  * Direct `JT` run of the same entry → **0 `[sys]` lines**: it dies in `load_elf` *before the guest
    runs a single syscall*.
* Minimal isolation:
  * `docker-entrypoint.sh echo HELLO` (script = argv0) → **exit 139**, identical `load_elf` fault.
  * `/bin/sh /usr/local/bin/docker-entrypoint.sh echo HELLO` (argv0 = a real ELF) → **exit 2**, runs,
    no crash.
  * `/bin/bash /usr/local/bin/docker-entrypoint.sh postgres` → runs the script (later errors are env
    artifacts, e.g. `gosu: not found` from my hand-built PATH — `gosu` *is* present at
    `/usr/local/bin/gosu`).
  * `postgres --version` → **exit 0** (PostgreSQL 18.4); the postgres binary itself loads fine.
* `docker-entrypoint.sh` starts with `#!/usr/bin/env bash`.

### Root cause

`load_elf` (`elf.c:44`) does **no ELF-magic check and no `#!` handling**. It blindly reads
`phoff = rd64(f+32)`, `phnum = rd16(f+56)` (`elf.c:57-58`) from the file head, then iterates
`ph = f + phoff + i*phentsize` and `memcpy((void*)(v+bias), f+off, fsz)` (`elf.c:60-66, 84-85`). For a
shebang script those fields are ASCII, so `f + phoff` is a wild pointer and the loop dereferences it →
`EXC_BAD_ACCESS`. The initial loader `jit_run` calls `load_elf(prog_host, …)` directly
(`targets/linux_aarch64.c:310`) with **no shebang rewrite**, whereas the `execve` path
(`service.c:1697-1747`, case 221) *does* parse `#!`, resolve the interpreter, and load that instead.
So a container whose entry program is a script crashes immediately; one whose entry is a real ELF that
*then* `execve`s scripts is fine (the daemon emits `exec env … <jit> … docker-entrypoint.sh postgres`,
`lib.rs:190`, so argv0 is the script — postgres hits this on the normal entrypoint).

This is a **third, separate** loader gap — distinct from the redis `mprotect` bug and from the non-PIE
fork+exec `ET_EXEC` crash. It blocks postgres *before* any of the setuid/`setsid`(157)/daemonize/initdb
behavior the prior PLAN notes discuss is ever reached; those remain **unverified / downstream**. (Note
for that downstream work: the docker `postgres` entrypoint runs `exec postgres` in the **foreground** —
it does **not** daemonize; the real fork+exec stress is `initdb` spawning `postgres --boot`, i.e. the
same fork+execve path as the busybox-tail crash, which would be the *next* blocker after the shebang fix.)

### Candidate fixes (ranked)

1. **Add shebang handling to the entry loader.** Factor the `#!` block out of `execve` case 221
   (`service.c:1704-1746`) into a helper and call it in `jit_run` before `load_elf`
   (`targets/linux_aarch64.c:308-310`): if `prog` begins with `#!`, rewrite argv to
   `[interp, (optarg), scriptpath, args…]` and load the interpreter. Directly fixes the observed crash
   and reuses proven logic.
2. **Validate the ELF magic in `load_elf`** (`elf.c:57`): if `f[0..4] != "\x7fELF"`, fail cleanly
   (and, if `#!`, hand back the interpreter). Good defensive hardening; pair it with #1 (without #1 it
   only converts the crash into a clean error, postgres still won't run).
3. Make the daemon wrap every container command in `sh -c 'exec "$@"'` so argv0 is always a real ELF and
   the existing `execve` shebang path handles the script. Rejected: papers over the loader gap, adds a
   shell process to every container, and changes pid-1 semantics.

### Cheapest confirming experiment

Drop a two-line `#!/bin/sh`+`echo ok` script into a rootfs and run it as the JIT's entry program — it
faults in `load_elf` today, prints `ok` after fix #1. End-to-end: `docker-entrypoint.sh postgres` should
then get past the loader and into `initdb` (where the fork+exec `ET_EXEC`/address-space-reset work is the
expected next gap).

---

## Relationship to the known fork+exec crash

* **redis** — unrelated; a pure `mprotect`/`mmap` protection bug on the **main thread**, no fork/exec
  involved (no syscall 220/221 before the fault).
* **postgres** — unrelated to the *non-PIE* crash but in the loader family; it is a **missing-feature**
  (entry shebang) that triggers before any fork/exec. The non-PIE fork+exec instability is only expected
  to surface *after* this is fixed, during `initdb`.

## Incidental daemon-side gap (not a JIT crash)

The local `postgres:alpine` image metadata is incomplete:
`docker.io_library_postgres_alpine/dd-image.json` carries only `{"cmd":["/bin/bash"],"name":…}` — no
`entrypoint`, no `env` (PGDATA etc.). So a bare `docker run postgres:alpine` runs `/bin/bash`, not the
postgres entrypoint at all. Separate from the JIT bugs above; flagged for the image import/registry path.
