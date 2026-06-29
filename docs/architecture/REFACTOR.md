# dd-jit REFACTOR — boundaries, ownership, order

Goal (owner's words): *isolate boundaries so changes don't collide and we don't hit
unexpected bugs.* Not a rewrite. Read `ARCHITECTURE.md` first — the interface table there
is the contract this plan protects. PLAN ONLY: nothing here is executed yet.

The architecture is already good: the `abi.h` G_* seam + the `dispatch_hooks.h` seam keep
the two linux frontends sharing `os/linux/` + `jit/` without coupling to each other. The
problem is **a few oversized shared files** + **names that don't say what each domain does**.

## Organize by domain — naming & the target tree

### The naming problem
"jit" is overloaded: it's the brand (crate `dd-jit`), a directory (`jit/`), AND a technique.
But **JIT is just one technique the runtime uses**, not the product:
- the product is a **VM-less container runtime** for foreign-OS/foreign-ISA binaries;
- `frontend/aarch64` barely "JITs" — it transliterates near-1:1;
- **darwin has TWO mechanisms**, and the answer to "does darwin have a JIT?" is *one does,
  one doesn't*: `jitdarwin.c` IS a same-ISA DBT (it has a code cache + translator + emitter,
  used as a syscall-interception point), while `darwinjail.c` is a pure **DYLD-interpose jail
  with no JIT at all**. Lumping both under "jit" hides that they are different domains.

### The smell: `engine/emit_arm64.c` (host assembler ≠ engine)

Correct instinct. `jit/emit_arm64.c` tangles **two layers**:
- a **host ARM64 assembler** — `emit32`, `e_ldr`, `e_movz`, `e_movconst`, … (pure instruction
  encoders, no engine/guest knowledge); AND
- **engine block-ABI stubs** — `emit_prologue`, `emit_spill`, `emit_ibtc_miss`,
  `emit_chain_exit` (the JIT's trampoline/IBTC semantics).

Because the assembler is buried inside the engine and **named after the host ISA**, it can't
be cleanly shared: the aarch64 frontend reaches into it, while the x86 frontend **duplicates
it** (`frontend/x86_64/emit.c` has 81 `e_*` vs the engine's 25). That's the smell — a host
primitive masquerading as engine logic, and duplicated as a result.

Fix: extract the assembler into its OWN host layer below the engine. The engine then becomes
**host-ISA-agnostic** (cache + dispatch loop) plus its block-ABI stubs that *call* the
assembler. Both frontends use the one assembler → the x86 duplication collapses (only the
x86-specific SSE/x87/flag emitters stay in the frontend).

### Two axes: the matrix is (guest OS × guest ISA)

The architecture has **two independent axes**. Naming + dirs must keep them independent so a
new OS or a new ISA is an additive change, never a cross-cutting edit:

```
                 guest ISA  →  x86_64        aarch64       (riscv64 …)
   guest OS ↓
     linux              linux_x86_64   linux_aarch64
     darwin             —              darwin_aarch64
    (windows)           windows_x86_64 windows_aarch64   ← adding Windows = a new ROW
```

A **row** (guest OS) = one `os/<osname>/` personality. A **column** (guest ISA) = one
`translate/<isa>/` + `include/cpu_<isa>.h`. A **cell** = one `targets/<os>_<isa>.c`. The
engine + host assembler are shared by the whole grid.

### One word per domain (vocabulary — define once, use everywhere)

| domain | what it does | current dir | proposed | axis |
|---|---|---|---|---|
| **host** | host primitives: the ARM64 **assembler** + MAP_JIT/mmap/icache (the README's `hal`) | inside `jit/` + inline | `host/arm64/` (`asm.c`) `host/darwin/` | host (fixed) |
| **engine** | host-ISA-agnostic JIT machinery: code cache, dispatcher, block-ABI stubs | `jit/` | `engine/` | host (fixed) |
| **translate** | guest ISA → host code | `frontend/` | `translate/<isa>/` | per guest ISA |
| **os** | guest-OS personality: syscalls, threads, signals, loader | `os/linux/` | `os/<osname>/` | per guest OS |
| **syscalls** | the syscall switch within a personality | `os/linux/service*` | `os/<os>/syscall/` | per guest OS |
| **container** | jail, overlay, netns, /proc synth | `os/linux/container/` | keep | per guest OS (reusable) |
| **isolation** | untrusted-guest sentry split | `os/linux/sentry.c` | keep | opt-in |
| **jail** | DYLD-interpose native jail (no JIT) | `os/darwin/darwinjail.c` | `os/darwin/jail/` | darwin only |
| **runtime** | per-target entry: compose slice, load, run | `targets/` | keep | per cell |

Keep the crate brand **dd-jit**; rename `jit/` → `engine/` + lift the assembler to `host/`.

### Proposed domain-organized tree (target state)

```
src/runtime/
├─ host/            HOST (fixed = arm64 macOS) — below the engine, used by engine + translate
│  ├─ arm64/asm.c   the ARM64 assembler: emit32 + e_*  (the extracted primitive)
│  └─ darwin/       MAP_JIT arena, mmap, sys_icache_invalidate, W^X flip
├─ engine/          ENGINE (host-ISA-agnostic) — cache.c  dispatch.c (run_guest)  stubs.c (prologue/IBTC/trampoline)
├─ include/         cpu_<isa>.h                         (one per guest ISA — the column key)
├─ translate/       per guest ISA (the columns)
│  ├─ x86_64/       decode.c  translate.c  translate/<class>.c  emit.c  avx.c  abi.h  dispatch_hooks.h
│  └─ aarch64/      translate.c  abi.h  dispatch_hooks.h
├─ os/              per guest OS (the rows)
│  ├─ linux/        thread.c signal.c elf.c fscache.c sentry.c  syscall/  container/
│  ├─ darwin/       jitdarwin.c  jail/
│  └─ windows/      ← NEW ROW would live here: pe.c (PE/COFF loader)  syscall/ (NT)  …
└─ targets/         one cell per built binary:  <os>_<isa>.c  → dd_run + main
```

Each top-level dir = one domain; each subdir = one axis value. A maintainer reads the path and
knows OS-vs-ISA-vs-engine. Renames are mechanical `git mv` + include-path fixes (only the
`targets/*.c` manifests change); do them as isolated commits, matrix green between each.

### Canonical entrypoint per layer (same name everywhere)

There are currently **no enforced naming conventions** — entry symbols diverge
(`jit_run`/`jit86_run`), the assembler is mis-homed, and plurals/prefixes drift. Adopt ONE
canonical entry file + symbol per layer so every engine looks identical:

| layer | entry file (same name across the axis) | entry symbol | one per |
|---|---|---|---|
| runtime | `targets/<os>_<isa>.c` | `dd_run(rootfs, argc, argv)` + `main` | cell |
| os personality | `os/<os>/syscall/dispatch.c` | `service(cpu)` | guest OS |
| translate | `translate/<isa>/translate.c` | `translate_block(gpc)` | guest ISA |
| engine | `engine/dispatch.c` | `run_guest(cpu)` | shared |
| host asm | `host/arm64/asm.c` | `emit32` + `e_*` | shared |

`translate_block`, `run_guest`, `service` are already uniform; the only fixes are `dd_run` and
homing the assembler. After that, "where does engine X start?" has one answer per layer.

### File-naming conventions (so the path/prefix tells the story)

| thing | rule | example |
|---|---|---|
| directory | one domain (`host` `engine` `translate` `os`) or one axis value (`x86_64` `linux`) | `os/windows/` |
| target file | `<guestos>_<guestisa>.c` | `windows_x86_64.c` |
| seam macro | `G_*` (cross-layer contract) | `G_NR`, `G_DISPATCH_REASON` |
| engine/host global | `g_*` | `g_cache`, `g_ibtc` |
| container-config env | `DD_*` (see LAUNCH.md) | `DD_ROOTFS` |
| engine tuning env | `DDJIT_*` | `DDJIT_PCACHE` |
| syscall-family handler | `svc_<family>()` in `syscall/<family>.c` | `svc_io` |
| instruction-class translator | `translate_<class>()` in `translate/<class>.c` | `translate_x87` |
| ARM64 instruction emitter | `e_<mnemonic>` | `e_ldr` |
| block-exit reason | `R_<NAME>` | `R_SYSCALL` |

The prefix alone classifies any symbol: `G_`=contract, `g_`=engine state, `DD_`=user contract,
`DDJIT_`=engine knob, `svc_`/`translate_`/`e_`/`R_` = its layer.

### Adding a new guest is the test (worked example: Windows)

The structure is correct iff a new OS/ISA is purely additive. Two cases below.

> **Illustrative only — NOT planned work.** Windows and riscv64 are *thought experiments*
> used to prove the two axes are decoupled (a guest-OS change must not touch `translate/`;
> a guest-ISA change must not touch `os/`). They are **not** on any roadmap. The real and
> only intended targets are **linux/x86_64, linux/aarch64, darwin/aarch64**. Nothing in this
> section is a commitment to build Windows or riscv64.

**New guest OS (Windows on x86_64)** = add ONE row, touch nothing existing:
1. `os/windows/` — the personality: `pe.c` (PE/COFF loader, the ELF analogue), `syscall/`
   (NT syscall dispatch, mirroring `os/linux/syscall/`), threads/signals as needed.
2. `targets/windows_x86_64.c` — the cell: `#include translate/x86_64` + `engine/` + `host/` +
   `os/windows/`, define `dd_run`/`main`. **Reuses the entire x86_64 translator and engine
   unchanged** — translation doesn't care about the guest OS.
3. The frontend already provides `abi.h` `G_*`; the Windows personality consumes the same seam
   (it switches on its own syscall numbering via `G_NR`). No translate/ or engine/ edit.

**New guest ISA (e.g. riscv64)** = add ONE column: `include/cpu_riscv64.h` +
`translate/riscv64/` (decode/translate/emit/abi.h/dispatch_hooks.h) + a `<os>_riscv64.c` cell.
Reuses every `os/` personality unchanged.

This is why the two axes must stay decoupled: a guest-OS change must never force a translate/
edit, and a guest-ISA change must never force an os/ edit. The `abi.h` G_* seam is exactly the
joint that keeps them independent.

## Collision points (where edits step on each other today)

| # | hot file | why it collides | isolating boundary |
|---|---|---|---|
| C1 | **os/linux/service.c** (3585) main `switch(nr)` | every syscall family NOT yet split lands in one switch → two agents adding unrelated syscalls touch the same 3k-line file; merge conflicts + accidental case fallthrough | finish the **split-module pattern** (#5): move each remaining family to `service/<family>.c` behind the `svc_<family>()` return-1 protocol. Shrink service.c to dispatch+plumbing only. |
| C2 | **frontend/x86_64/translate.c** (3184) opcode switch | one giant switch for all instruction classes; an AVX fix and an ALU fix collide; hard to reason which `R_*` exits exist | split by **instruction class** behind the existing `e_*` emit interface (emit.c). Keep ONE dispatch switch that calls per-class `translate_<class>()` in their own files. |
| C3 | **include/cpu_*.h** struct + `OFF_*` | any field insert mid-struct shifts baked offsets used by emitted code AND `run_block` asm → silent corruption across the whole engine | **append-only past the baked region** + a single `OFF_*` block as the sole source of truth; a compile-time `_Static_assert` on key offsets makes a bad edit fail to build, not fail at runtime. |
| C4 | **targets/*.c** unity includes | the include ORDER encodes the dependency graph (state→cache→emit→translate→service…); a wrong reorder = forward-ref breakage; it's the de-facto "linker script" | treat each `targets/*.c` as the **owned build manifest** for that target — comment each include with its layer, change order only deliberately. |
| C5 | **sentry.c** (1747) + service.c | the untrusted-guest path (`syscall_route`/`g_untrusted`) is interleaved with the normal service path | keep the sentry split as its own module (already is); ensure the ONLY coupling is the `service()`→`syscall_route()` fwd-decl, not shared mutable state. |
| C6 | **avx.c** (1703) | correctness-first block-exit emulation; large and self-contained but mixed VEX/EVEX/legacy-0F38 | already isolated behind `R_AVX`/`R_SSE3B`; fine as one file — split only if VEX vs EVEX start to collide. Low priority. |
| C7 | **ARM64 assembler duplicated** (`jit/emit_arm64.c` 25 `e_*` vs `frontend/x86_64/emit.c` 81) | the host assembler is buried in the engine and named after the host ISA, so the x86 frontend re-implements it → a host-encoding fix must be made twice, and they can drift | extract `host/arm64/asm.c` (`emit32`+`e_*`); engine keeps only block-ABI stubs that call it; both frontends share the one assembler. |

## What to SHARE vs keep PER-ENGINE (the balance)

The right balance = **abstract the OS surface and the host engine; duplicate the guest ISA.**

| concern | decision | why |
|---|---|---|
| host ARM64 assembler (`emit32`/`e_*`) | **SHARE** in `host/` | a pure encoder; both frontends + the engine emit host code through it (kills the C7 duplication) |
| `engine/` (cache, dispatch, block-ABI stubs) | **SHARE** | host ISA is ARM64 for all targets; one engine, hooks for divergence |
| os/linux personality (service, container, threads, signal, elf) | **SHARE** behind G_* | guest OS is the same Linux regardless of guest ISA |
| `struct cpu` | **PER-ISA** (never merged) | register files genuinely differ (16 vs 31 GPRs, x87/AVX vs NEON) |
| frontend decode/translate/emit | **PER-ISA, duplicated** | x86 decode and aarch64 transliteration share NOTHING real; coupling them would be false reuse. They meet ONLY at cpu struct + the emit/jit core. |
| run_block/block_return | **PER-ISA** via `G_OWN_TRAMPOLINES` | register model is the one irreducible divergence |

**Rule: the two frontends must never `#include` or reference each other's internals.**
They communicate only through (a) the cpu struct, (b) `abi.h` G_*, (c) `dispatch_hooks.h`,
(d) the shared `jit/` API. If a new feature needs frontends to share code, that code belongs
in `jit/` or `os/linux/`, not in one frontend reaching into the other.

## Split-vs-big-file calls

| area | call | rationale |
|---|---|---|
| service.c | **SPLIT** (finish it) | the protocol already exists and is proven non-colliding; the remaining big switch is the main source of OS-side merge pain |
| translate.c | **SPLIT by instruction class** behind `e_*` | biggest x86 file; class boundaries are natural and the emit interface already exists. Keep one thin dispatch switch. |
| avx.c, sentry.c, netns.c, vfs.c | **KEEP as-is** | already single-responsibility modules behind a clear seam; splitting adds files without isolating a real collision |
| jit/cache.c, dispatch.c, emit_arm64.c | **KEEP** | shared engine core; small, cohesive, already hooked |
| cpu_*.h | **KEEP one file each** + assert offsets | one struct = one truth; splitting headers risks offset drift |

Bias: **few cohesive files, split only where a switch is a shared write-target for unrelated
features.** Do not split for size alone.

## Ownership zones (so two agents don't touch one file)

| zone | owns | files |
|---|---|---|
| **engine** | host ARM64 JIT core | jit/*, dispatch_hooks default block |
| **x86-frontend** | x86→ARM64 | frontend/x86_64/*, include/cpu_x86_64.h, targets/linux_x86_64.c |
| **arm-frontend** | aarch64→ARM64 | frontend/aarch64/*, include/cpu_aarch64.h, targets/linux_aarch64.c |
| **OS-core** | Linux syscalls | os/linux/service.c + service/*, thread.c, signal.c, elf.c, fscache.c |
| **OS-container** | jail/overlay/net | os/linux/container/* |
| **OS-isolation** | sentry | os/linux/sentry.c |
| **darwin** | macOS guest | os/darwin/* , targets/darwin_aarch64.c |
| **interface** (shared, change-with-care) | the contracts | abi.h, dispatch_hooks.h, cpu_*.h, fill_stat.c |

Cross-zone edit = a contract change → must touch BOTH sides of an interface in one change
and is the only case that needs the change announced. Within a zone, edits are independent.

## Same-name entrypoint per target (navigation convention)

Today the per-target entry exists but is **inconsistently named**, so you can't predict
where an engine starts:

| target | entry file | lib symbol | `main()` |
|---|---|---|---|
| linux_aarch64 | targets/linux_aarch64.c | `jit_run()` | yes |
| linux_x86_64  | targets/linux_x86_64.c  | `jit86_run()` | yes |
| darwin_aarch64| targets/darwin_aarch64.c → forwards to os/darwin/jitdarwin.c | (none; jitdarwin's own `main`) | in jitdarwin.c |

**Convention to adopt:** every target has the SAME structure — one predictable entry FILE
and one predictable entry SYMBOL — so a reader (or the Rust binding) always knows the door.

```
targets/<target>.c   ← THE entrypoint for that target, always. Same shape in all three:
    1. system #includes
    2. #include the layer slice (frontend + engine + os personality); order = the manifest
    3. int dd_run(const char *rootfs, int argc, char *const argv[])   ← unified lib entry
    4. int main(...)  → arg-parse → dd_run(...)
```

Changes:
- **Rename the lib entry to one canonical symbol** `dd_run()` in all three targets
  (replacing `jit_run`/`jit86_run`; darwin grows one wrapping jitdarwin's body). The Rust
  binding then `extern`s a single name regardless of target instead of switching per-guest.
- **darwin gets a real `targets/darwin_aarch64.c` entry** that owns `dd_run`/`main` and
  `#include`s `os/darwin/jitdarwin.c` for the body — matching the linux targets' shape, so
  the door is `targets/<target>.c` for ALL three (no "forwards elsewhere" exception).
- Pure **mechanical rename**, no behavior change — slot it as step 0 below, before the
  bigger moves, so the entrypoint stays stable while you refactor underneath.

## Make boundaries enforceable

- **Header discipline:** a frontend includes `os/linux/*` only via the G_* seam; never the
  reverse. os/linux references the guest only through `G_*`/`fill_stat`, never a frontend `.c`.
- **Naming convention:** shared seam macros are `G_*`; engine globals `g_*`; per-family
  syscall entries `svc_<family>()`; per-class translators `translate_<class>()`. A name's
  prefix tells you its zone.
- **Offset safety:** `_Static_assert(offsetof(struct cpu, field)==OFF_X)` next to each
  `OFF_*` so an accidental struct edit fails the build, not a guest at runtime.
- **One ownership table** (above) — the canonical map of who-may-edit-what.

## Incremental refactor order (mechanical, test-preserving)

Each step is a pure code-move behind an EXISTING seam → the cross-engine matrix must stay
green after every step. No behavior change in any step.

0. **Unify the entrypoint** (above): rename `jit_run`/`jit86_run` → `dd_run`, give darwin a
   real `targets/darwin_aarch64.c` entry. Mechanical rename; makes the door predictable
   before anything moves underneath.
1. **Add the offset `_Static_assert`s** to cpu_*.h. Zero risk, immediately prevents the
   nastiest silent corruption (C3). Do this first.
2. **Finish the service.c split (C1).** Move one syscall family at a time from the main
   switch into a new/existing `service/<family>.c` `svc_*()`. One family per commit; run the
   matrix between each. Highest isolation gain, lowest churn (the protocol exists).
3. **Unify launch/env (LAUNCH.md):** add back-compat readers for the `DD_*` names in the
   shared container parser, collapse the binding's os-branch, unify x86 onto `DD_GUEST_ENV`.
   Behavior-preserving; makes the public contract uniform.
4. **Domain renames** (`git mv` only, then fix include paths): `jit/`→`engine/`,
   `frontend/`→`translate/`, `os/linux/service*`→`os/linux/syscall/`,
   `os/darwin/darwinjail.c`→`os/darwin/jail/`. Each rename is one isolated commit; the unity
   TUs (`targets/*.c`) are the only files whose `#include` paths change. Pure naming → matrix
   must be byte-identical after.
5. **Extract the host assembler (C7):** `jit/emit_arm64.c` → `host/arm64/asm.c` (the `e_*`
   encoders) + leave the engine block-ABI stubs in `engine/`. Then point `frontend/x86_64`
   at the shared assembler and delete its duplicated base encoders. Mechanical move +
   de-dup; matrix byte-identical.
6. **Document the targets/*.c include order (C4)** as the build manifest (comments only) so
   later moves don't reorder by accident. (Doc step, not a code move.)
7. **Split translate.c by instruction class (C2).** Extract one class (e.g. x87, then
   string ops, then ALU) into `translate/<class>.c` (the dir already holds trace/x87/repstr),
   leaving a thin dispatch switch. One class per commit, matrix between.
8. **(Later, larger) dedup the engines:** lift translate/x86_64’s remaining own pieces onto
   shared `engine/` where the register model allows, and lift `os/darwin` onto `engine/` + a
   darwin personality mirroring the os/linux split. This is the existing "dedup" roadmap — do
   it ONLY after 1–7, because it touches the trampoline/register-model seam (#9), the riskiest.

Stop after step 7 for the "isolate collisions" goal; step 8 is the long-horizon convergence.
Steps 0 + 3 + 4 (entrypoint + launch + domain renames) are the "dead-clean naming" win and
are the safest to do first — they change no behavior, only make the tree say what it does.

The full final layout (every file) + the per-step move-map is in `TREE.md`.

## How NOT to enter bug hell (this is a live, baked-offset JIT)

The danger isn't the logic — it's that emitted machine code bakes `struct cpu` offsets and the
unity build resolves everything by `#include` ORDER. A move can compile clean and miscompile
the *guest*. Rules that keep every step boring:

| risk | guard |
|---|---|
| **a "pure move" silently changes codegen** | classify each step **STRUCTURAL** (no behavior) vs **BEHAVIORAL**. STRUCTURAL steps (0,3,4,5,6) must produce a **byte-identical engine binary** — verify with the diff below, not just a green matrix. |
| **baked offsets drift** | the `_Static_assert(offsetof==OFF_*)` (step 1) goes in BEFORE any struct touches; a wrong offset then fails the *build*. |
| **include-order / forward-ref breakage** (unity TU) | only `targets/*.c` change includes; never reorder within a step. `.clang-format` has `SortIncludes:false` — keep it; never let a tool reorder includes. |
| **one commit changes two things** | one mechanical change per commit, each independently revertable → `git bisect` lands on the exact culprit. Never bundle a rename with an edit. |
| **a regression hides until a specific guest** | run the FULL matrix (`make test`) between steps, not a subset; add the differential oracle (below) for codegen-identity. |
| **concurrency regressions** (cache/IBTC) | run the threaded scenarios (`make test-realsw`, redis/postgres) — single-thread green hides STW/IBTC races. |

**Byte-identity check for STRUCTURAL steps** (the strongest safety net):
```
clang -O2 -E targets/<t>.c | clang -O2 -x c - -S -o before.s   # preprocessed → asm, before
# … do the git mv + include-path fix …
clang -O2 -E targets/<t>.c | clang -O2 -x c - -S -o after.s    # after
diff before.s after.s      # MUST be empty for a pure rename/move
```
If `diff` is non-empty, the "pure move" wasn't pure — stop and find what leaked. Behavioral
steps (2 split-by-family, 7 split-by-class) can't be byte-identical, so they rely on the
matrix + the per-syscall/per-opcode coverage tool (`make coverage`).

**Phase order rationale:** do the zero-behavior steps first (0,1,3,4,5,6 — names/moves/asserts)
so the tree is clean and stable BEFORE the behavioral splits (2,7), and leave the
register-model dedup (8) for last. Never refactor a file in the same week someone is fixing a
bug in it (the owner pushes x86 `translate.c` upstream — coordinate step 7 around that).

## Static analysis & bug-finding tooling

The build is plain `clang -O2`, no warnings/sanitizers/analysis today. High-leverage adds,
roughly in order of value for THIS codebase:

| tool | finds | notes for a unity-build JIT |
|---|---|---|
| **`-Wall -Wextra -Wshadow -Wconversion`** | shadowed vars, implicit narrowing (the `atoi`/offset bugs), unused | cheapest win; turn on in `build.rs`, fix or `-Wno-` per case |
| **UBSan** (`-fsanitize=undefined`) | signed overflow, OOB shifts, misaligned loads, bad enum — rife in bit-twiddling emitters | instruments the ENGINE C only (not JITed guest code); run the matrix under it |
| **ASan** (`-fsanitize=address`) | heap/stack OOB, UAF in cache/loader/parsers | works with MAP_JIT; **cannot** instrument the guest's own memory — still catches engine bugs |
| **TSan** (`-fsanitize=thread`) | data races in the cache / IBTC / STW flush (the threaded hazards the code comments worry about) | run `make test-realsw` (redis/postgres = real threads) under it |
| **clang static analyzer** (`scan-build`) | null deref, leaks, dead stores, uninit reads — across the whole TU | works great on unity TUs (one big TU = whole-program view) |
| **`cppcheck --enable=all`** | buffer caps, `strtok` misuse, format strings | fast, no build; good for the parsers (the validation gaps) |
| **`semgrep`** (custom rules) | **enforce the boundaries**: ban `translate/x86_64` ↦ `translate/aarch64` includes; ban `atoi` in config parsers; require `DD_*`/`DDJIT_*` prefixes | turns the ownership rules into CI gates |
| **the existing oracle** | guest correctness vs real CPU/OS | `make test` (cross-engine matrix), `make coverage` (syscall/opcode gaps), the QEMU/Docker differential oracle — already the primary net; keep it the gate |

What does NOT help here: **valgrind** (can't cope with MAP_JIT self-modifying code on macOS),
and **IWYU / include-sorting** (would break the load-bearing unity include order — keep
`SortIncludes:false`). Sanitizers can't see *guest* memory (it's the guest's, not malloc'd by
us); the differential oracle remains the only check for translation correctness.

Recommended baseline: a `make analyze` that runs `scan-build` + `cppcheck` + a UBSan/ASan
matrix run, plus a `semgrep` boundary-rule gate in CI. Add the boundary rules WITH the renames
(steps 3–4) so the new structure can't silently rot.
