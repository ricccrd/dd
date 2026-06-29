# dd-jit REFACTOR — boundaries, ownership, order

Goal (owner's words): *isolate boundaries so changes don't collide and we don't hit
unexpected bugs.* Not a rewrite. Read `ARCHITECTURE.md` first — the interface table there
is the contract this plan protects. PLAN ONLY: nothing here is executed yet.

The architecture is already good: the `abi.h` G_* seam + the `dispatch_hooks.h` seam keep
the two linux frontends sharing `os/linux/` + `jit/` without coupling to each other. The
problem is **a few oversized shared files where unrelated features land in one switch**.

## Collision points (where edits step on each other today)

| # | hot file | why it collides | isolating boundary |
|---|---|---|---|
| C1 | **os/linux/service.c** (3585) main `switch(nr)` | every syscall family NOT yet split lands in one switch → two agents adding unrelated syscalls touch the same 3k-line file; merge conflicts + accidental case fallthrough | finish the **split-module pattern** (#5): move each remaining family to `service/<family>.c` behind the `svc_<family>()` return-1 protocol. Shrink service.c to dispatch+plumbing only. |
| C2 | **frontend/x86_64/translate.c** (3184) opcode switch | one giant switch for all instruction classes; an AVX fix and an ALU fix collide; hard to reason which `R_*` exits exist | split by **instruction class** behind the existing `e_*` emit interface (emit.c). Keep ONE dispatch switch that calls per-class `translate_<class>()` in their own files. |
| C3 | **include/cpu_*.h** struct + `OFF_*` | any field insert mid-struct shifts baked offsets used by emitted code AND `run_block` asm → silent corruption across the whole engine | **append-only past the baked region** + a single `OFF_*` block as the sole source of truth; a compile-time `_Static_assert` on key offsets makes a bad edit fail to build, not fail at runtime. |
| C4 | **targets/*.c** unity includes | the include ORDER encodes the dependency graph (state→cache→emit→translate→service…); a wrong reorder = forward-ref breakage; it's the de-facto "linker script" | treat each `targets/*.c` as the **owned build manifest** for that target — comment each include with its layer, change order only deliberately. |
| C5 | **sentry.c** (1747) + service.c | the untrusted-guest path (`syscall_route`/`g_untrusted`) is interleaved with the normal service path | keep the sentry split as its own module (already is); ensure the ONLY coupling is the `service()`→`syscall_route()` fwd-decl, not shared mutable state. |
| C6 | **avx.c** (1703) | correctness-first block-exit emulation; large and self-contained but mixed VEX/EVEX/legacy-0F38 | already isolated behind `R_AVX`/`R_SSE3B`; fine as one file — split only if VEX vs EVEX start to collide. Low priority. |

## What to SHARE vs keep PER-ENGINE (the balance)

The right balance = **abstract the OS surface and the host engine; duplicate the guest ISA.**

| concern | decision | why |
|---|---|---|
| `jit/` engine (cache, dispatch, emit_arm64) | **SHARE** | host ISA is ARM64 for all targets; one engine, hooks for divergence |
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

1. **Add the offset `_Static_assert`s** to cpu_*.h. Zero risk, immediately prevents the
   nastiest silent corruption (C3). Do this first.
2. **Finish the service.c split (C1).** Move one syscall family at a time from the main
   switch into a new/existing `service/<family>.c` `svc_*()`. One family per commit; run the
   matrix between each. Highest isolation gain, lowest churn (the protocol exists).
3. **Document the targets/*.c include order (C4)** as the build manifest (comments only) so
   later moves don't reorder by accident. (Doc step, not a code move.)
4. **Split translate.c by instruction class (C2).** Extract one class (e.g. x87, then
   string ops, then ALU) into `translate/<class>.c` (the `translate/` dir already exists with
   trace/x87/repstr), leaving a thin dispatch switch. One class per commit, matrix between.
5. **(Later, larger) dedup the engines:** lift frontend/x86_64’s remaining own pieces onto
   shared `jit/` where the register model allows, and lift `os/darwin` onto `jit/` + a darwin
   personality mirroring the os/linux split. This is the existing "dedup" roadmap — do it ONLY
   after 1–4, because it touches the trampoline/register-model seam (#9), the riskiest one.

Stop after step 4 for the "isolate collisions" goal; step 5 is the long-horizon convergence.
