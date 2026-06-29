# dd-jit ARCHITECTURE — the mental model

dd-jit JIT-translates guest machine code (linux/x86_64, linux/aarch64, darwin/aarch64)
to host ARM64 macOS and emulates the guest OS in userspace — no VM. One C codebase,
three Mach-O executables (one per `(guest OS, guest ISA)` target).

See also `../README.md` (Layout / Decomposition state). This doc is the **interface map**;
`REFACTOR.md` is the **structure/change plan**; `LAUNCH.md` is the **launch/env contract**;
`TREE.md` is the **full target folder layout** (every file + the move-map).

## Layer map

```
            guest ISA                 host ISA            guest OS
  ┌────────────────────────┐   ┌──────────────────┐  ┌────────────────────────┐
  │ frontend/x86_64  (jit86)│   │ jit/  (the engine│  │ os/linux/   (personality│
  │   decode→translate→emit │   │  shared by all   │  │  service + threads +    │
  │ frontend/aarch64 (jit)  │   │  linux targets): │  │  signal + elf)          │
  │   transliterate+mangle  │   │  cache, dispatch,│  │ os/linux/container/     │
  └────────────┬───────────┘   │  emit_arm64      │  │   vfs, overlay, netns   │
               │               └────────┬─────────┘  │ os/linux/service/       │
               │  cpu struct (include/) │            │   io,mem,signal,time,…  │
               │  + abi.h G_* seam      │            │ os/darwin/ (whole POC)  │
               └────────────────────────┴────────────┴────────────────────────┘
                                        │
                          targets/<target>.c  ← unity TU: #includes the slice
                                        │           it needs, compiled by build.rs
                                   ddjit-<target>  (Mach-O exe)
```

Everything is `#include`d into ONE translation unit per target (no separate compilation /
no link step between modules). A "module" = a `.c` file pulled in by `targets/*.c`.

## Folder tree (as-is — what's where today)

```
src/runtime/
├─ include/                  GUEST CPU layout (the #1 interface — baked offsets)
│  ├─ cpu_x86_64.h
│  └─ cpu_aarch64.h
├─ frontend/                 TRANSLATION domain — guest ISA → host ARM64 (one subdir per guest ISA)
│  ├─ x86_64/               x86-64 JIT (jit86)
│  │  ├─ abi.h               ← G_* cpu/syscall seam (interface #2)
│  │  ├─ dispatch_hooks.h    ← dispatcher seam (interface #3)
│  │  ├─ decode.c            instruction decode
│  │  ├─ translate.c         the big opcode→ARM64 switch  (3184 LOC)
│  │  ├─ translate/{x87,repstr,trace}.c   class-split helpers (the split has begun)
│  │  ├─ emit.c              ARM64 emitters + SSE + x87
│  │  ├─ avx.c               VEX/EVEX AVX emulation  (1703 LOC)
│  │  ├─ elf.c               x86 ELF load + auxv + fault handlers
│  │  ├─ sigframe.c fill_stat.c x86_ops.c legacy.c sysmap.h   per-ISA helpers
│  │  ├─ cache.c dispatch.c engine_glue.c pcache.c            x86 engine leftovers (pre-dedup)
│  │  └─ forkserver.c fclient.c                               resident ddjitd fork-server
│  └─ aarch64/              aarch64 transliterator (jit) — near-1:1
│     ├─ abi.h dispatch_hooks.h   ← seams (interfaces #2/#3)
│     ├─ translate.c        transliterate + mangle + §B shadow + LSE  (1514 LOC)
│     └─ sigframe.c fill_stat.c
├─ jit/                      ENGINE domain — host ARM64 execution core (shared by linux targets)
│  ├─ cache.c               code cache + gpc→host map + chaining + STW flush
│  ├─ dispatch.c            run_guest() loop + default seams + trampolines
│  └─ emit_arm64.c          low-level ARM64 encoder + IBTC/IC
├─ os/                       GUEST-OS domain — syscall personality + container
│  ├─ linux/
│  │  ├─ service.c           SYSCALLS: main switch + split-module dispatch  (3585 LOC)
│  │  ├─ service/{io,mem,signal,time,sysv,helpers}.c   syscall families
│  │  ├─ thread.c signal.c elf.c fscache.c             threads/signals/loader/fd-cache
│  │  ├─ sentry.c            ISOLATION: untrusted-guest sentry split  (1747 LOC)
│  │  └─ container/          CONTAINER: jail / overlay / net
│  │     ├─ vfs.c state.c netns.c
│  │     └─ vfs/{resolve,overlay,gmap}.c
│  └─ darwin/                macOS guest (see naming note in REFACTOR.md)
│     ├─ jitdarwin.c         same-ISA DBT (code cache + translator + Mach/BSD trap intercept)
│     └─ darwinjail.c        DYLD-interpose jail (NOT a JIT — separate mechanism)
└─ targets/                  RUNTIME domain — one unity-TU entrypoint per target
   ├─ linux_aarch64.c        #includes the slice + jit_run() + main()
   ├─ linux_x86_64.c         #includes the slice + jit86_run() + main()
   └─ darwin_aarch64.c       #includes os/darwin/jitdarwin.c
```

The domains are already roughly separated by directory; the naming is inconsistent (`jit/`
is the engine, not "the JIT"; `service.c` is "syscalls"; darwin mixes a DBT and a jail).
`REFACTOR.md` proposes domain-clean names + the target tree.

## The three targets (what each unity TU pulls in)

| target | frontend | engine | OS personality |
|---|---|---|---|
| `linux_aarch64` | frontend/aarch64 | jit/ (cache+dispatch+emit_arm64) | os/linux + container |
| `linux_x86_64`  | frontend/x86_64 (own emit + own trampolines) | jit/cache + jit/dispatch | os/linux + container |
| `darwin_aarch64`| os/darwin/jitdarwin.c (whole, self-contained) | — | (inline) |

`linux_x86_64` already SHARES the whole `os/linux/` personality and the shared `jit/cache.c` +
`jit/dispatch.c`; it keeps its own `emit.c`/`translate.c` (different register model →
`G_OWN_TRAMPOLINES`). `darwin_aarch64` is still imported whole.

## Module inventory (sizes, owner zone)

| file | LOC | zone | role |
|---|---:|---|---|
| os/linux/service.c | 3585 | OS-core | main syscall switch + `service_local` + split-module dispatch |
| frontend/x86_64/translate.c | 3184 | x86-frontend | the big opcode→ARM64 switch + own trampolines |
| os/linux/sentry.c | 1747 | OS-isolation | untrusted-guest SPSC ring + sentry split |
| frontend/x86_64/avx.c | 1703 | x86-frontend | VEX/EVEX AVX/AVX2/AVX-512 emulation |
| frontend/aarch64/translate.c | 1514 | arm-frontend | transliterate + mangle + §B shadow + LSE |
| os/linux/container/netns.c | 816 | OS-container | sockets, loopback netns, termios |
| frontend/x86_64/emit.c | 736 | x86-frontend | ARM64 emitters + SSE + x87 |
| frontend/x86_64/elf.c | 689 | x86-frontend | x86 ELF load + auxv + fault handlers |
| jit/cache.c | 639 | engine | code cache, gpc→host map, chaining, STW flush |
| os/linux/container/vfs.c | 614 | OS-container | rootfs jail, overlay, /proc synth, stat |
| os/linux/elf.c | 609 | OS-core | aarch64 ELF load + initial stack |
| jit/emit_arm64.c | 548 | engine | low-level ARM64 encoder + IBTC/IC |
| os/linux/service/{io,time,mem,signal,sysv,helpers}.c | 1913 | OS-core | split syscall families |
| frontend/x86_64/{decode,x86_ops,pcache,dispatch,forkserver,…}.c | — | x86-frontend | decoder + helpers |
| frontend/{x86_64,aarch64}/abi.h | 57/62 | **interface** | the `G_*` cpu/syscall contract |
| frontend/{x86_64,aarch64}/dispatch_hooks.h | 277/132 | **interface** | the dispatcher seam |
| include/cpu_{x86_64,aarch64}.h | 95/89 | **interface** | guest CPU layout (baked offsets) |

## Where do I change X? (task → file → interface touched)

The maintainer's index. Find your task, go to the file, mind the interface column (that's
the contract you must not break — see the next section).

| I want to… | edit | interface(s) at risk |
|---|---|---|
| add/fix an **x86 instruction** | frontend/x86_64/translate.c (+ emit.c for a new emitter) | #4 (new `R_*` → also its `G_DISPATCH_REASON`) |
| add/fix an **AVX/AVX-512 op** | frontend/x86_64/avx.c | #4 (`R_AVX`/`R_SSE3B`) |
| add/fix an **aarch64 instruction** | frontend/aarch64/translate.c | #4 |
| add/fix a **Linux syscall** | the owning `os/linux/service/<family>.c`; else the main switch in service.c | #2, #5 |
| change **socket/netns/termios** behavior | os/linux/container/netns.c | — |
| change **path jail / overlay / /proc** | os/linux/container/{vfs.c,vfs/*} | — |
| change **clone/futex/thread** behavior | os/linux/thread.c | #1 (per-thread cpu) |
| change **signal delivery** | os/linux/signal.c (+ frontend/*/sigframe.c for the frame) | #1, #8 |
| add a **guest CPU field / scratch slot** | include/cpu_*.h | #1 (append-only, update `OFF_*`) |
| change the **JIT cache / chaining / flush** | jit/cache.c | #6 |
| change the **dispatcher loop** | jit/dispatch.c (shared) + the per-arch dispatch_hooks.h | #3, #4, #9 |
| change **ELF loading / initial stack / auxv** | os/linux/elf.c (arm) · frontend/x86_64/elf.c (x86) | #7 |
| change a **guest struct layout** (stat/sigaction) | frontend/*/fill_stat.c | #8 |
| change **process startup / args / fork-server** | targets/<target>.c (+ frontend/x86_64/forkserver.c) | — |
| change **untrusted-guest isolation** | os/linux/sentry.c | — |

**Two-frontend rule:** a guest-ISA task (top rows) is x86 OR aarch64 — never both files.
An os/linux task is shared, so it must work for BOTH frontends via the `G_*` seam (#2),
never by special-casing a guest ISA.

## Two data flows (the whole engine in 12 lines)

```
TRANSLATE (cold path)                    SYSCALL (warm path)
  run_guest loop (jit/dispatch.c)          block exits, reason=R_SYSCALL
   └ map_host(pc) miss                       └ G_DISPATCH_REASON → service(c)
      └ translate_block(pc)   [frontend]         └ service_local()   [os/linux]
         └ decode → emit ARM64 [emit*.c]            ├ split modules svc_*()  (#5)
         └ map_put(pc → host)  [cache.c #6]         └ main switch(G_NR)      (#2)
   └ run_block(c, code)        [trampoline #9]    └ G_A0..A5 in, G_RET out
      └ block sets reason, returns (#4)          └ errno xlate macOS→Linux
```

## THE REAL INTERFACES (the contracts a change must not break)

These are the seams where an edit in one place silently breaks another. Each is already
implicit in the code; this table makes the contract explicit.

| # | interface | where | producer → consumer | invariant (break = silent corruption) |
|---|---|---|---|---|
| 1 | **`struct cpu` layout** | include/cpu_*.h | frontend codegen bakes `OFF_*` → engine asm + os/linux read same offsets | Field offsets are baked into emitted machine code AND into `run_block`/`block_return` asm (jit/dispatch.c `#288…`). Adding a field mid-struct shifts offsets → corrupt. Append-only past the baked region; update every `OFF_*`. |
| 2 | **`abi.h` G_* seam** | frontend/*/abi.h | frontend defines `G_NR/A0..A5/RET/PC/SP/TLS/NORMALIZE/…` → os/linux/service.c consumes | os/linux switches on the **canonical (aarch64) syscall number**; a frontend whose guest numbers differ must map them in `G_NR` (x86: `canon_x86`). Open-flag bits (`G_O_DIRECTORY/NOFOLLOW`) and `G_UNAME_MACHINE` are per-ISA — never hardcode them in os/linux. |
| 3 | **dispatch seam** | dispatch_hooks.h + jit/dispatch.c `#ifndef` defaults | frontend defines the `G_DISPATCH_*`/`G_IBTC_FILL`/`G_*` hook macros → shared `run_guest()` expands them at its call sites | Macros expand INSIDE the dispatcher loop, so they may `break`/`continue` the loop and reference engine globals (`g_ibtc`, `map_body`, `R_SYSCALL`). aarch64 hooks must stay byte-identical to the old inline code (defaults in dispatch.c reproduce it). |
| 4 | **block-dispatch contract** | `reason` + `redirect` + `rip/pc` in cpu | emitted block sets `cpu->reason` (R_BRANCH/R_SYSCALL/R_AVX/…) and exits → `run_guest` `G_DISPATCH_REASON` acts | Every `R_*` an emitter can produce MUST be handled in that frontend's `G_DISPATCH_REASON`. The syscall pc-advance convention is per-ISA (aarch64 `pc+=4`; x86 pre-advances `rip` in the emitter). |
| 5 | **service split-module dispatch** | service.c:385–389 | `service_local` calls `svc_sysv/mem/signal/time/io(...)` in order; each returns 1 if it handled `nr` → else fall to main switch | Each `svc_*` is its OWN file (no cross-file edit to add a syscall to a family). A syscall must be owned by exactly ONE place (a family module OR the main switch) — two handlers = first wins, silently. |
| 6 | **translation-cache key/hash** | jit/cache.c + `G_GPC_HASH_SHIFT` (abi.h) | frontend sets shift (x86=0 byte-granular, aarch64=2) → cache.c `map_put/map_host/map_body` hash gpc | Cache is keyed purely by guest PC. Self-modifying guest code must invalidate (`smc_icflush`); a stale gpc→host entry after a guest overwrites code = executing dead translation. |
| 7 | **ELF/loader handoff** | os/linux/elf.c (aarch64) · frontend/x86_64/elf.c (x86) | loader sets entry/base/auxv + `g_nonpie_lo/bias`; builds initial stack → run_guest enters at `cpu->pc/rip` | The loader populates the cpu’s PC/SP and the non-PIE bias the dispatcher applies. Per-ISA (auxv `machine`/`platform`, ELF e_machine) — kept per-frontend, NOT shared. |
| 8 | **per-arch struct fill** | frontend/*/fill_stat.c | frontend provides `fill_linux_stat` (stat/sigaction differ by ISA) → os/linux io/service writes guest memory | os/linux must call the frontend’s filler, never lay out a guest `struct stat` itself. |
| 9 | **trampoline ownership** | `G_OWN_TRAMPOLINES` | frontend with a non-default register model supplies its own `run_block`/`block_return` → jit/dispatch.c suppresses its pair | The register model (16 vs 31 guest GPRs, cpu-ptr pinning) is the ONE irreducible per-ISA divergence; the shared loop only CALLS `run_block`, never bakes its offsets. |

### How a syscall flows (orientation)
`run_block` exits with `reason=R_SYSCALL` → `G_DISPATCH_REASON` calls `service(c)` →
`service_local`: (a) non-PIE pointer rebase, (b) FS-epoch bump, (c) split-module dispatch
[#5], (d) main `switch(G_NR(c))` [#2], (e) errno xlate macOS→Linux. Args via `G_A0..A5`,
return via `G_RET`.
