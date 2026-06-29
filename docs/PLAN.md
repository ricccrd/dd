# dd — THE plan (open bugs first, then phased refactor)

`dd` is a VM-less container runtime: a Cargo workspace of `dd-jit` (JIT runtime + Rust bindings),
`dd-daemon` (Docker Engine API), and the desktop surface (`ddcli` / `dd-gui`). It JIT-translates
guest Linux/macOS binaries (x86-64 / aarch64) to the arm64 macOS host — no VM.

This is the **single source of truth**. Read in order:

- **PART A — what is still missing or failing** (the open inventory). Fix these FIRST.
- **PART B — the phased refactor** (structural work; begins only after the bugs in PART A are clear).
- **PART C — capabilities & product roadmap** (the *other* axis: features/productization/testing/ops
  that are neither a bug nor a refactor — what we don't build yet).

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

### PHASE 10 — Observability/debug-build consolidation

> **Goal (owner's words):** *standard mechanisms that dump what we need, a debug-vs-production build,
> and a clear way to launch in debug mode when an external report comes in.* Today there is **one**
> build flavor (`clang -O2`, no `-g`/asserts/sanitizer), every diagnostic is a runtime `getenv` in the
> hot path, the knobs use three naming schemes, and — the killer — **the binding does not forward
> them** (only `DD_*` reaches the `exec env …` launch prefix). Full spec + runbook:
> [`DEBUGGING.md`](DEBUGGING.md).

Independent of the structural moves; can proceed alongside PHASE 1–8 (mostly additive). **BEHAVIORAL.**

| step | kind | goal | exit criteria |
|---|---|---|---|
| 10.1 build profiles | build | `DD_DEBUG`/`DD_SAN` in `build.rs` → release (trace **compiled out**, `-DNDEBUG`) vs debug (`-g`+asserts+`_Static_assert`s) vs asan/tsan; `make engine-{debug,asan,tsan}` | release binary byte-identical to today (trace points compiled out, not just gated); debug build carries symbols+asserts; matrix green on release |
| 10.2 knob namespace | behavioral | fold bare/`JIT86_*` knobs under `DDJIT_*` (`DDJIT_TRACE=`, `DDJIT_PROF=`, `DDJIT_NO=`, `DDJIT_CRASHDUMP=`, `DDJIT_SAMPLE=`, `DDJIT_DEBUG=` umbrella) with back-compat readers for the old names | both old + new names work; matrix green |
| 10.3 env forwarding | behavioral | `SpawnConfig::script()` copies any ambient `DDJIT_*` into the launch prefix (allow-listed), like `DD_*` | `DDJIT_TRACE=block,syscall dd run …` reproduces with full diagnostics via the normal/daemon path |
| 10.4 standard dumps | behavioral | unify the UNIMPL spellings → `[syscall-unimpl]`/`[op-unimpl]`; one tagged-line crash dump; add the **guest-PC sampler** + promote the **indirect-branch/IBTC log** + engine banner (§3 DEBUGGING.md) | a spin bug is localizable from `DDJIT_SAMPLE` alone; bundle reproduces the 5 symptom classes |
| 10.5 runbook | doc | the copy-pasteable single-bundle collection (§6 DEBUGGING.md) is the standard external-repro path | DEBUGGING.md runbook validated against one real open bug (e.g. `toolchain-link-hang` via the sampler/ibranch log) |

**Exit:** one switch flips debug↔prod; the hot dispatch path is zero-cost in prod; a `DDJIT_*` knob
set on a normal `dd run` reaches the engine; an external report is reproduced + a full diagnostic
bundle collected by following [`DEBUGGING.md`](DEBUGGING.md) §6 without hand-instrumenting the engine.

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

---

## PART C — CAPABILITIES & PRODUCT ROADMAP

The **other axis**: what `dd` does not yet *do*, independent of the open bugs (PART A) and the structural
refactor (PART B). These are features/productization/testing-breadth/ops — capability gaps, not defects.
Status legend: **EXISTS** (shipped, works) · **PARTIAL** (works for the common case, real gaps) ·
**DESIGNED** (design doc only, no code) · **MISSING** (not designed, not built). Priority is rough
product priority, not urgency vs PART A bugs (bugs always come first per PHASE 0).

> Do not re-list PART A bugs or PART B refactor steps here. Observability/debug-build work is owned by
> **PHASE 10** + [`DEBUGGING.md`](DEBUGGING.md) — referenced, not duplicated. Extensibility to a **new
> guest OS/ISA** (a Windows row, a riscv64 column) is *illustrative only* — it validates the refactor's
> two-axis tree ([`architecture/REFACTOR.md`](architecture/REFACTOR.md), [`architecture/TREE.md`](architecture/TREE.md))
> and is **not planned work**; it appears nowhere below as a roadmap item.

### C.1 Networking (container L3 identity & DNS)

| item | status | what's missing | pointer | pri |
|---|---|---|---|---|
| Egress (guest→internet TCP/UDP) | EXISTS | guest `AF_INET` socket *is* a host socket; egress + DNS already work | [`design/netstack.md`](design/netstack.md) §0 | — |
| Inbound `-p H:C` port publish | EXISTS | host bind + `getsockname` report; single-host only | netstack.md §0 | — |
| Loopback isolation, `--network none` | EXISTS | per-cid `AF_UNIX` redirect; `DD_NET_ISOLATE` | netstack.md §0 | — |
| **Per-container L3 identity (distinct IP)** | DESIGNED | containers have **no IP** — every container is "the host" on the wire; breaks `inspect .NetworkSettings.IPAddress`, container→container by-IP, IPAM in `network inspect` | netstack.md §0–1 (no-root userspace synth) | **HIGH** |
| **Embedded DNS (container→container by name)** | DESIGNED | no resolver for service names on a user network | netstack.md | **HIGH** |
| Full userspace TCP/IP stack (`smoltcp`, real `eth0`/netlink/raw ICMP `ping`) | DESIGNED + BLOCKED | `DD_NETSTACK`; needed only for true L3 semantics; `smoltcp` crate unfetchable offline | netstack.md §1, PART A.3 | MED |

### C.2 Daemon / Docker Engine-API depth

The route surface is broad (containers/images/exec/build/volumes/networks/events/stats/logs/commit/
auth/push/df — see `dd-daemon/src/main.rs`). Remaining capability gaps:

| item | status | what's missing | pointer | pri |
|---|---|---|---|---|
| Core CLI + lifecycle, exec, attach, logs (`--follow`/`--tail`), stats, events, prune, df, auth, push, commit, save/load | EXISTS | conformance suite green (`make test-docker[-full]`) | `dd-daemon/src/*` | — |
| Compose (`up`/`ps`/`logs`/`exec`/`down`) | PARTIAL | works **via the Engine API** driven by the `docker compose` plugin (`make test-compose`); no dd-native compose; depends on C.1 for multi-service networking | `dd-tests/scenarios/compose.sh` | MED |
| **Healthchecks** | MISSING | no health model — every container reports `health=none`; `HEALTHCHECK` Dockerfile verb is parsed-then-ignored; no `--health-*` runtime, no `(healthy)` state, no health events | `containers/inspect.rs:429`, `build.rs:553` | **HIGH** |
| **BuildKit / buildx** | MISSING | classic builder only (copy base rootfs, run each `RUN` in the JIT); no BuildKit frontend, cache mounts, multi-stage parallelism, `--platform` matrix, secrets/ssh mounts | `dd-daemon/src/build.rs:33` | MED |
| Registry **pull/push auth depth** | PARTIAL | `/auth` + push exist; verify token-refresh, multi-registry creds, manifest-list/OCI index handling | `dd-daemon/src/registry.rs` | MED |
| Swarm / services | MISSING (by design) | reported `inactive`; out of scope for a single-host runtime — list only so it's a conscious non-goal | `system.rs:52` | LOW |
| Resource limits enforcement (cpu/io) | MISSING | `update`/`--cpus`/blkio accepted but no cgroup on macOS; mem+pids via rlimit only | PART A.4 | MED |

### C.3 Guest coverage (the three EXISTING targets only)

Targets: **linux/x86_64**, **linux/aarch64**, **darwin/aarch64**. (aarch64 NEON/crypto/CRC/LSE/FP16/
dotprod/i8mm/bf16 opcode coverage is **complete** per the completeness probe — STATUS.md.) Real gaps:

| item | status | what's missing | pointer | pri |
|---|---|---|---|---|
| x86 **x87 transcendentals** | PARTIAL | `FPREM` unimplemented (blocks ruby x86_64); audit `FSIN/FCOS/FYL2X/F2XM1/FPATAN` family | PART A #3, `frontend/x86_64/x87.c` | MED |
| x86 **AVX-512 residuals** | PARTIAL | VEX/EVEX AVX/AVX2/AVX-512 emulated in `avx.c`; long tail of EVEX masked/broadcast forms likely still UNIMPL — no exhaustive probe yet | `frontend/x86_64/avx.c` | LOW |
| Edge/obscure **syscalls** | PARTIAL | madvise/renameat2/fallocate/SEEK_HOLE/abstract-AF_UNIX/F_SETPIPE_SZ/clock_nanosleep-ABSTIME diverge; `pidfd`/`io_uring`/`mq_*` absent | PART A #5, A.4 | LOW |
| Big dynamic ET_EXEC loader robustness | PARTIAL | mongosh ld.so SIGSEGV (193 MB) — loader/TLS edge | PART A #4 | MED |
| darwin/aarch64 guest breadth | PARTIAL | jail + DBT proven on a small set; real macOS app/runtime breadth unproven | memory `dd-mac-containers` | LOW |
| *(illustrative, not planned)* new guest OS row / ISA column | n/a | the refactor tree *could* host a Windows `os/` row or riscv64 `translate/` column — demonstrates extensibility, **not roadmap** | REFACTOR.md, TREE.md | — |

### C.4 Performance (design levers not yet pulled)

Core engine opts are landed (chaining, IBTC, tier-2, inline syscalls, path/openat caches, pcache,
fork-server — OPTIMIZATIONS.md). Pending levers:

| item | status | what's missing | pointer | pri |
|---|---|---|---|---|
| **aarch64 SQLite parity** (A3→A1→B1→B2) | DESIGNED (gated diffs ready) | apply in order, rebuild + `make test-diff`/`test-gates` each | `design/arm-{a3,a1,b1}.*`, `arm-sqlite-parity.md` | **HIGH** |
| x86 perf levers | DESIGNED | `design/x86-perf.md` levers unshipped | `design/x86-perf.md` | MED |
| redis command-dispatch hot path | PARTIAL | ~18× off native, 98.5% CPU-bound — iterative | PART A.3 | MED |
| `rep cmps`/`scas` fast path | MISSING | called out as remaining; rep mov/sto done | memory `dd-jit-optimization-sweep` | MED |
| Sub-ms startup (fork of large VM reservation + guest `ld.so`) | DESIGNED | not built | PART A.3 | MED |
| engine-dedup (PR3/4) perf+size | DESIGNED | overlaps refactor PHASE 9 | `design/engine-dedup.md` | LOW |

### C.5 GUI & rendering

| item | status | what's missing | pointer | pri |
|---|---|---|---|---|
| `dd-gui` (GTK4/relm4 desktop app: home/containers/images/networks/volumes/system/settings/onboarding) | EXISTS | macOS-only; pure GTK4 (no libadwaita) | `dd-gui/src/ui/views/` | — |
| `ddcli` / `dd` CLI (run/daemon/context) | EXISTS | thin; proxies docker CLI; no native `ps`/`build`/compose verbs | `dd-cli/src/` | LOW |
| **GUI/GPU rendering of guest apps to macOS** (Wayland/X11→DDP→Metal, IOSurface zero-copy, Vulkan/Zink→Metal) | DESIGNED | no code; large effort; build gated on review | [`ideas/RENDERING.md`](ideas/RENDERING.md), [`ideas/RENDERING_PLAN.md`](ideas/RENDERING_PLAN.md) | MED |
| Cross-platform terminal emulator | MISSING (roadmap idea) | website pillar; no design doc | `website/roadmap.html` | LOW |
| Linux-as-host (run dd on Linux; macOS containers on Linux) | DESIGNED/roadmap | website pillar; partial harness facts, no host port | `website/roadmap.html` | MED |

### C.6 Security / isolation

fd-virtualization + per-process fd tables + `DD_*` input validation are **shipped**. Remaining:

| item | status | what's missing | pointer | pri |
|---|---|---|---|---|
| **Linux seccomp worker confinement** | MISSING | only macOS Seatbelt exists; no seccomp filter on the untrusted worker | [`design/sentry-security.md`](design/sentry-security.md) §3, PART A.3 | **HIGH** |
| Guard page after last ring (defense-in-depth) | MISSING | sentry hardening | sentry-security.md | MED |
| mach-lookup filter / execve-under-Seatbelt image read | PARTIAL | hardening gaps in the Seatbelt profile | sentry-security.md | MED |
| Rootless / capabilities / user-namespace mapping | MISSING | no `--cap-add/drop`, no userns; runs as the invoking user | — | LOW |
| Untrusted-image "can we trust the sandbox yet?" | PARTIAL | "**not yet**" until seccomp + guard page land | sentry-security.md §1 | HIGH |

### C.7 Release / ops / distribution

| item | status | what's missing | pointer | pri |
|---|---|---|---|---|
| DMG packaging + ad-hoc/Developer-ID signing | EXISTS | `make dmg`, signing wired (`dd-sign` keychain) | `dd-gui/package/`, memory `notarization-clock-skew` | — |
| CI: release DMG, Pages, real-image smoke (both arches) | EXISTS | `.github/workflows/{release,pages,smoke}.yml`; stale-engine guard via `cargo clean -p ddjit` | memory `dd-ci-stale-engine` | — |
| **Notarization** | PARTIAL/BLOCKED | signing proven; notarize blocked by orbstack clock skew + `xcrun` not on nix PATH | memory `notarization-clock-skew` | **HIGH** |
| Auto-update | PARTIAL | `dd-gui/src/update.rs` exists — verify the feed/channel + signature check end-to-end | `dd-gui/src/update.rs` | MED |
| **Homebrew / public distribution channel** | MISSING | no tap/cask; DMG is the only artifact | — | MED |
| Debug-vs-production build flavor | MISSING | one `clang -O2` flavor today; owned by **PHASE 10.1** (`make engine-{debug,asan,tsan}`) | PHASE 10, DEBUGGING.md | (see PHASE 10) |

### C.8 Testing / QA breadth

| item | status | what's missing | pointer | pri |
|---|---|---|---|---|
| Cross-engine matrix ×3, docker-full/net/compose, macOS parity, realsw, diff-vs-qemu, coverage tool | EXISTS | the correctness backbone | Makefile, `testing/` | — |
| xfail backlog / stale-marker recheck | PARTIAL | clear stale `edge/*` markers; recheck python-slim-amd, tsc/vdso/hwcap, toolchain-suite when link lands | STATUS.md | MED |
| **Fuzzing** (decoder / ELF loader / ring-marshaling) | MISSING | no fuzz harness; sentry ring + ELF parser are prime targets | sentry-security.md (untrusted surface) | MED |
| Differential-oracle breadth: real **x86_64** server images | PARTIAL/BLOCKED | only arm64 images local; redis/postgres/nats x86 e2e unrun | PART A.3 | MED |
| JIT-in-JIT (JVM/V8/CoreCLR/RyuJIT) on **arm-mac** + extra runtimes (Deno/Bun/GHC/Swift…) | PARTIAL | green on Linux oracle; MAP_JIT path not broadly exercised on mac | STATUS.md coverage gaps | MED |
| Offset `_Static_assert`s (build-time struct-offset guard) | MISSING | owned by refactor **PHASE 2** — cross-ref | PHASE 2 | (see PHASE 2) |
| Soak/endurance + sanitizer (ASan/TSan/UBSan) runs in CI | PARTIAL | soak exists; sanitizer builds gated on PHASE 10.1 | PHASE 10, memory `dd-tests-coverage` | MED |
