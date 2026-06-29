# dd-jit TREE — the full target folder structure (final state)

The complete post-refactor layout (every file) + the move-map from today. Rationale is in
`REFACTOR.md`; this doc is the picture and the mechanical mapping. Two axes: a **row** =
guest OS (`os/<os>/`), a **column** = guest ISA (`translate/<isa>/` + `include/cpu_<isa>.h`),
a **cell** = one built binary (`targets/<os>_<isa>.c`). `host/` + `engine/` are shared by all.

## Final tree

```
dd-jit/
├─ Cargo.toml  build.rs  README.md  .clang-format
├─ src/
│  ├─ lib.rs                         # bindings: Guest, SpawnConfig (ONE DD_* template, validated)
│  └─ runtime/
│     ├─ host/                       # HOST primitives (host = arm64 macOS) — below the engine
│     │  ├─ arm64/asm.c              #   ARM64 assembler: emit32 + e_*        ⟵ from jit/emit_arm64.c
│     │  └─ darwin/mem.c             #   MAP_JIT arena, mmap, W^X flip, icache  (extract later; inline today)
│     ├─ engine/                     # host-ISA-AGNOSTIC JIT machinery        ⟵ was jit/
│     │  ├─ cache.c                  #   code cache, gpc→host map, chaining, STW flush
│     │  ├─ dispatch.c               #   run_guest() loop + default seams
│     │  └─ stubs.c                  #   block-ABI: prologue/spill/IBTC/trampolines ⟵ from emit_arm64.c
│     ├─ include/                    # COLUMN key — per-ISA guest CPU layout (baked offsets)
│     │  ├─ cpu_x86_64.h
│     │  └─ cpu_aarch64.h
│     ├─ translate/                  # COLUMNS — per guest ISA               ⟵ was frontend/
│     │  ├─ x86_64/
│     │  │  ├─ abi.h                 #   G_* cpu/syscall seam      (interface #2)
│     │  │  ├─ dispatch_hooks.h      #   dispatcher seam           (interface #3)
│     │  │  ├─ sysmap.h              #   x86 sysno → canonical
│     │  │  ├─ decode.c              #   instruction decode
│     │  │  ├─ translate.c           #   opcode dispatch switch + own trampolines (G_OWN_TRAMPOLINES)
│     │  │  ├─ insn/                 #   instruction classes (split out of translate.c, step 7)
│     │  │  │  ├─ alu.c  shift.c  string.c  x87.c  trace.c   ⟵ extends today's translate/{x87,repstr,trace}
│     │  │  ├─ emit.c                #   x86-specific emitters (SSE/x87/flags); base encoders via host/arm64
│     │  │  ├─ avx.c                 #   VEX/EVEX AVX/AVX-512 emulation
│     │  │  ├─ x86_ops.c             #   cpuid + x87 m80 helpers
│     │  │  ├─ fill_stat.c           #   per-ISA struct stat       (interface #8)
│     │  │  ├─ sigframe.c            #   per-ISA rt_sigframe
│     │  │  ├─ legacy.c              #   legacy → *at normalization (G_NORMALIZE)
│     │  │  ├─ loader.c              #   x86 auxv/machine + fault handlers (per-ISA loader seam) ⟵ from elf.c
│     │  │  ├─ pcache.c              #   persistent translated-code cache (opt8)
│     │  │  └─ forkserver.c  fclient.c   # resident ddjitd fork-server
│     │  └─ aarch64/
│     │     ├─ abi.h  dispatch_hooks.h
│     │     ├─ translate.c           #   transliterate + mangle + §B shadow + LSE
│     │     ├─ fill_stat.c  sigframe.c
│     │     └─ loader.c              #   aarch64 auxv/machine seam
│     ├─ os/                         # ROWS — per guest OS
│     │  ├─ linux/
│     │  │  ├─ elf.c                 #   shared Linux ELF loader core
│     │  │  ├─ thread.c              #   clone/futex/threads
│     │  │  ├─ signal.c              #   signal delivery driver
│     │  │  ├─ fscache.c             #   fd/path cache
│     │  │  ├─ sentry.c              #   untrusted-guest isolation (opt-in)
│     │  │  ├─ syscall/              #   SYSCALLS                  ⟵ was service.c + service/
│     │  │  │  ├─ dispatch.c         #     service() + service_local main switch ⟵ service.c
│     │  │  │  ├─ io.c  mem.c  signal.c  time.c  sysv.c   #   families
│     │  │  │  └─ helpers.c
│     │  │  └─ container/
│     │  │     ├─ config.c           #   SHARED validating DD_* parser (NEW)   ⟵ state.c parsers + per-target dup
│     │  │     ├─ state.c            #   container globals
│     │  │     ├─ vfs.c  netns.c
│     │  │     └─ vfs/resolve.c  vfs/overlay.c  vfs/gmap.c
│     │  ├─ darwin/
│     │  │  ├─ jitdarwin.c           #   same-ISA DBT + Mach/BSD trap intercept
│     │  │  └─ jail/jail.c           #   DYLD-interpose jail (→ darwinjail.dylib)  ⟵ darwinjail.c
│     │  └─ windows/                 #   FUTURE ROW (illustrative — empty today)
│     │     ├─ pe.c                  #     PE/COFF loader (the ELF analogue)
│     │     └─ syscall/dispatch.c    #     NT syscall dispatch
│     └─ targets/                    # CELLS — one per built binary → dd_run + main
│        ├─ linux_x86_64.c
│        ├─ linux_aarch64.c
│        ├─ darwin_aarch64.c
│        └─ windows_x86_64.c         #   FUTURE
└─ docs/
   └─ ARCHITECTURE.md  REFACTOR.md  LAUNCH.md  TREE.md
```

## Move-map (current → final) with the step that does it

| current | final | step | note |
|---|---|---|---|
| `jit/emit_arm64.c` | split → `host/arm64/asm.c` + `engine/stubs.c` | 5 | encoders vs block-ABI stubs (C7) |
| `jit/cache.c` | `engine/cache.c` | 4 | `git mv` |
| `jit/dispatch.c` | `engine/dispatch.c` | 4 | `git mv` |
| `frontend/x86_64/*` | `translate/x86_64/*` | 4 | `git mv` (dir rename) |
| `frontend/aarch64/*` | `translate/aarch64/*` | 4 | `git mv` |
| `frontend/x86_64/translate/{x87,repstr,trace}.c` | `translate/x86_64/insn/*` | 7 | + new class files |
| `frontend/x86_64/emit.c` (base `e_*`) | use `host/arm64/asm.c`; keep only SSE/x87 emitters | 5 | de-dup (C7) |
| `frontend/x86_64/elf.c` | loader core → `os/linux/elf.c`; per-ISA bits → `translate/x86_64/loader.c` | 8 | dedup |
| `os/linux/service.c` | `os/linux/syscall/dispatch.c` | 4 | rename; shrinks as families move out (step 2) |
| `os/linux/service/*.c` | `os/linux/syscall/*.c` | 4 | `git mv` |
| `os/linux/container/state.c` (parsers) | + `os/linux/container/config.c` (validating) | 3 | LAUNCH unify + validation |
| `os/darwin/darwinjail.c` | `os/darwin/jail/jail.c` | 4 | `git mv` |
| `targets/{linux_aarch64,linux_x86_64}.c` `jit_run`/`jit86_run` | `dd_run` (same files) | 0 | rename symbol |
| `targets/darwin_aarch64.c` | real entry: `dd_run`+`main`, includes the slice | 0 | match the others |
| **`frontend/x86_64/cache.c`** | **DELETE** | 4 | dead — target uses `jit/cache.c` (dedup already swapped) |
| **`frontend/x86_64/dispatch.c`** | **DELETE** | 4 | dead — replaced by `jit/dispatch.c` + `dispatch_hooks.h` |

(`*` keeps the per-arch helpers `decode.c translate.c avx.c x86_ops.c fill_stat.c sigframe.c
legacy.c pcache.c forkserver.c fclient.c sysmap.h abi.h dispatch_hooks.h engine_glue.c`.)

## Dead files found (remove during step 4)
- `frontend/x86_64/cache.c`, `frontend/x86_64/dispatch.c` — not `#include`d by any target; the
  shared `jit/` versions + `dispatch_hooks.h` superseded them (see `engine_glue.c` comment).
  Removing them eliminates "which dispatch is live?" confusion.
