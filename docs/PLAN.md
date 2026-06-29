# dd — THE plan (open bugs first, then phased refactor)

`dd` is a VM-less container runtime: a Cargo workspace of `dd-jit` (JIT runtime + Rust bindings),
`dd-daemon` (Docker Engine API), and the desktop surface (`ddcli` / `dd-gui`). It JIT-translates
guest Linux/macOS binaries (x86-64 / aarch64) to the arm64 macOS host — no VM.

This is the **single source of truth**. Read in order:

- **PART A — what is still missing or failing** (the open inventory). Fix these FIRST.
- **PART B — the phased refactor** (structural work; begins only after the bugs in PART A are clear).

Detail lives in the referenced docs — this plan does not duplicate them:
[`STATUS.md`](STATUS.md) (full live gap inventory + recheck/coverage gaps),
[`architecture/`](architecture/) (ARCHITECTURE, REFACTOR, TREE, LAUNCH, OPTIMIZATIONS),
[`design/`](design/) (per-lever subplans + gated diffs),
[`testing/`](testing/) (CHARTER, TESTING, IMAGE-MANIFEST), [`PERFORMANCE.md`](PERFORMANCE.md).

Validate everything with: `make test` (cross-engine matrix ×3), `make test-docker[-full|-net|-fn]`,
`make test-macos`, `make test-realsw`, `make coverage` (syscall/opcode gap tool), `make test-diff`
(byte-exact vs qemu/native).

> _Recently shipped (pruned from the list below — do **not** re-add; see git history): non-PIE guest_base
> bias-fold, ARM futex lost-wakeup, stop-the-world threaded code-cache flush, rustup multiplexer +
> rustc/rustc-frontend, ruby SSE movss/movsd, x86 ud2/int3/div0 trap→signal + sync-fault guard, jit86
> SSE2 packed-int + SSE/AVX(VEX) opcode lane, node V8 munmap-range, aarch64 LDAR-vs-LDXR exclusive,
> go version/env/gofmt, sentry guest-fd virtualization + per-process fd tables, daemon
> image-config/env/rmi/overlay, DD_* input validation. This plan is forward-looking only._

---

## PART A — KNOWN BUGS / WHAT'S MISSING OR FAILING

### A.1 Open engine / runtime bugs (the live list)

The current verified open set (discovery-3, confirmed on today's engine), priority order:

| # | id | pri | area | symptom | status | suspected file |
|---|----|-----|------|---------|--------|----------------|
| 1 | `toolchain-link-hang` | HIGH | os/linux fork+exec | gcc/rust **LINK** hangs (arm64): `gcc -c` and standalone `ld --version` both work, but `gcc file.c -o exe` (driver→collect2→ld nested fork+exec+pipe) hangs indefinitely on glibc **and** musl. Blocks rustc full compile + all C/C++/Rust builds. | IN PROGRESS | `os/linux` fork+exec / wait4 / pipe |
| 2 | `go-tool-compile` | HIGH | translate/aarch64 | `go tool compile` SIGSEGV (fault=0x0 NULL deref) in the multithreaded Go compiler's copystack/morestack path; `go run`/`build` then hang on the dead child. `go version`/`env`/`gofmt` PASS. | IN PROGRESS | `frontend/aarch64/translate.c` |
| 3 | `ruby-x87-fprem` | MED | translate/x86_64 | ruby x86_64 hits unimplemented x87 `FPREM` (`UNIMPL d9 f8`, exit 70). The SSE movss bug is fixed; it now faults on x87. | IN PROGRESS | `frontend/x86_64/x87.c` |
| 4 | `mongosh-ldso` | MED | loader (arm64) | `mongosh --version` SIGSEGVs **early in ld.so startup** (relocation/TLS), exit 255, before reaching the app (193 MB dynamic ET_EXEC) — does NOT run past cache-full. | open | `os/linux/elf.c` / aarch64 loader |
| 5 | `edge-syscalls` | LOW | os/linux syscalls | a handful of obscure syscalls still diverge (madvise DONTNEED no-op, renameat2 flags, fallocate PUNCH_HOLE, SEEK_HOLE/DATA, abstract AF_UNIX, F_SETPIPE_SZ, clock_nanosleep ABSTIME) | open | `service.c` per-syscall handlers |

### A.2 Minor / cosmetic divergences (the LOW cluster — correct results, wrong edge value)

| id | area | symptom | suspected file |
|----|------|---------|----------------|
| `cfg-truncation` | os/linux config | `g_netns` 40-char + `DD_HOSTNAME` 64-char silently truncate | container parser |
| `mem-max-drift` | config | `DD_MEM_MAX` format drift (linux suffixes vs darwin raw bytes) | `container_parse.h` |
| `de-si_addr` | translate/x86_64 | 64-bit div/idiv `si_addr` points one insn past on `#DE` | `frontend/x86_64` (div) |
| `div0-exit-code` | signal | no-handler div0 exits 136 (treated as normal exit) vs killed-by-signal | signal delivery |
| `dead-files` | translate/x86_64 | `frontend/x86_64/{cache.c,dispatch.c}` unused/dead — delete during refactor PHASE 4 | `frontend/x86_64/{cache,dispatch}.c` |

### A.3 Missing features / not-yet-implemented (not bugs — gaps)

- **Sentry hardening (remaining):** guard page after the last ring (defense-in-depth), **Linux seccomp**
  worker confinement (only macOS Seatbelt exists), edge/perf — futex/`__ulock` wakeup (replace the
  servicer spin), eventfd/timerfd/signalfd forwarding, sendmmsg/recvmmsg, execve-under-Seatbelt image read.
  (guest-fd virtualization + per-process fd tables are done.) See [`design/sentry-security.md`](design/sentry-security.md).
- **Perf (iterative, not one-shot):** redis command-dispatch hot path (~18× off native, 98.5% CPU-bound);
  **aarch64 SQLite parity** — gated, bit-exact diffs ready, apply in order **A3 → A1 → B1 → B2**
  ([`design/arm-sqlite-parity.md`](design/arm-sqlite-parity.md), `design/arm-a{3,1}.diff`, `design/arm-b1.diff`,
  rebuild + `make test`/`make test-diff`/`make test-gates` after each); sub-ms startup (fork of the large VM
  reservation + guest `ld.so`).
- **Blocked on environment:** netstack `smoltcp` (`DD_NETSTACK`) — crate unfetchable offline
  ([`design/netstack.md`](design/netstack.md)); real x86_64 redis/postgres/nats end-to-end — only arm64 images local.

### A.4 Platform limitations (the macOS primitive does not exist — workaround, not a fix)

non-PIE `ET_EXEC` `__PAGEZERO` (the ~4GB low-vaddr is mandatory; the **shipped** workaround is the JIT
`guest_base` bias-fold + per-fault fixups + dispatch-redirect — `nonpie-native-vaddr` native low-vaddr load
is **wontfix**); cpu/io throttling (no cgroup; mem+pids via rlimit — the supported mechanism); `pidfd`, `io_uring`, mqueue `mq_*`; `edge` corners
(`mprotect` PROT_NONE, `pipe2(O_DIRECT)`); macOS plain `mmap(PROT_EXEC)` without MAP_JIT (the correct
MAP_JIT + `pthread_jit_write_protect_np` path works); AT_PAGESZ must equal the host 16K granularity.

---

## PART B — REFACTORING RESEARCH IN PHASES

Goal (owner's words): *isolate boundaries so changes don't collide and we don't hit unexpected bugs.*
**Not a rewrite.** The architecture is already sound (the `abi.h` `G_*` seam + `dispatch_hooks.h` seam);
the problems are a few oversized shared files and names that don't say what each domain does. Full
rationale: [`architecture/REFACTOR.md`](architecture/REFACTOR.md); final tree + per-step move-map:
[`architecture/TREE.md`](architecture/TREE.md); contracts: [`architecture/ARCHITECTURE.md`](architecture/ARCHITECTURE.md)
+ [`architecture/LAUNCH.md`](architecture/LAUNCH.md).

### PHASE 0 — Bugs before structure (the gate)

> **The rule:** PART A is fixed (or each open row consciously deferred and xfail-tracked) **before** any
> structural code-move in PHASE 4+ begins. A baked-offset JIT punishes refactoring on top of unknown bugs —
> a "pure move" can compile clean and miscompile the *guest*. Land the bug fixes, keep the matrix green,
> *then* move code. Never refactor a file in the same week someone is fixing a bug in it.

Each step below is classified **STRUCTURAL** (must produce a byte-identical engine binary) or **BEHAVIORAL**
(relies on the matrix + `make coverage`). One mechanical change per commit; run the FULL matrix between steps.

| phase | kind | goal | steps | exit criteria |
|------|------|------|-------|---------------|
| **1. Unify entrypoint** | STRUCTURAL | one predictable door per target | rename `jit_run`/`jit86_run` → `dd_run`; give darwin a real `targets/darwin_aarch64.c` entry | byte-identical binary; matrix green; Rust binding externs one symbol |
| **2. Offset asserts** | safety | stop silent struct-offset drift | add `_Static_assert(offsetof(struct cpu,…)==OFF_*)` next to each `OFF_*` in `cpu_*.h` | wrong offset now fails the **build**, not a guest |
| **3. Split service.c** | BEHAVIORAL | isolate syscalls | move one syscall family at a time from the main switch into `service/<family>.c` `svc_*()`, one family/commit | matrix + `make coverage` green after each family |
| **4. Domain renames** | STRUCTURAL | tree says what it does | `git mv` only: `jit/`→`engine/`, `frontend/`→`translate/`, `service*`→`syscall/`, `darwinjail.c`→`jail/`; fix include paths in `targets/*.c` only; **delete dead `frontend/x86_64/{cache,dispatch}.c`** | byte-identical binary per the diff check below |
| **5. Unify launch/env** | BEHAVIORAL | one public launch contract | back-compat `DD_*` readers in the shared container parser; collapse the binding's os-branch; unify x86 onto `DD_GUEST_ENV` | behavior-preserving; matrix green |
| **6. Extract host assembler** | STRUCTURAL | de-dup the assembler | `jit/emit_arm64.c` → `host/arm64/asm.c` (the `e_*` encoders) + leave block-ABI stubs in `engine/`; point `frontend/x86_64` at the shared assembler, delete its duplicated base encoders | byte-identical binary |
| **7. Document include order** | doc only | freeze the unity build manifest | comment the `targets/*.c` `#include` order so later moves don't reorder | no code change |
| **8. Split translate.c** | BEHAVIORAL | shrink the giant | extract one instruction class at a time (x87, then string, then ALU) into `translate/<class>.c`, leaving a thin dispatch | matrix between each class |
| **9. (Later) Dedup engines** | BEHAVIORAL | long-horizon convergence | lift translate/x86_64's remaining own pieces onto shared `engine/`; lift `os/darwin` onto `engine/` + a darwin personality | do **only** after 1–8 (touches the trampoline/register-model seam — riskiest); see [`design/engine-dedup.md`](design/engine-dedup.md) |

**Stop after PHASE 8 for the "isolate collisions" goal; PHASE 9 is the long-horizon convergence.**
Safest-first ordering: the zero-behavior phases (1,2,4,6,7) before the behavioral splits (3,5,8), dedup (9) last.

### Byte-identity check (STRUCTURAL phases only)

```
clang -O2 -E targets/<t>.c | clang -O2 -x c - -S -o before.s     # before
#   … git mv + include-path fix …
clang -O2 -E targets/<t>.c | clang -O2 -x c - -S -o after.s      # after
diff before.s after.s     # MUST be empty for a pure move; non-empty ⇒ the move wasn't pure, stop
```

### Bug-hell guards (apply to every phase)

| risk | guard |
|------|-------|
| a "pure move" silently changes codegen | classify STRUCTURAL vs BEHAVIORAL; STRUCTURAL ⇒ byte-identical binary, verify with the diff (not just a green matrix) |
| baked offsets drift | the offset `_Static_assert`s (phase 2) go in BEFORE any struct touch |
| include-order breakage (unity TU) | only `targets/*.c` change includes; keep `.clang-format` `SortIncludes:false`; never let a tool reorder |
| one commit changes two things | one mechanical change per commit → `git bisect` lands the exact culprit |
| a regression hides until a specific guest | run the FULL `make test` between steps, plus `make test-realsw` (redis/postgres real threads) for cache/IBTC/STW races |

### Static-analysis & boundary tooling (add WITH the renames so the new structure can't rot)

`-Wall -Wextra -Wshadow -Wconversion` (cheapest; catches the narrowing/offset bugs) · **UBSan**/**ASan**
(engine C only — cannot see guest memory) · **TSan** under `make test-realsw` (cache/IBTC/STW races) ·
**scan-build** + **cppcheck** (great on unity TUs) · **semgrep** custom rules to *enforce* boundaries (ban
cross-frontend includes, ban `atoi` in config parsers, require `DD_*`/`G_*` prefixes). Ship a `make analyze`.
**Not useful here:** valgrind (MAP_JIT SMC) and IWYU/include-sorting (would break the load-bearing unity order).
The differential oracle (`make test` / `make test-diff` / `make coverage`) stays the primary correctness gate.
