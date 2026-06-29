# dd-jit ARCHITECTURE — the mental model

dd-jit JIT-translates guest machine code (linux/x86_64, linux/aarch64, darwin/aarch64)
to host ARM64 macOS and emulates the guest OS in userspace — no VM. One C codebase,
three Mach-O executables (one per `(guest OS, guest ISA)` target).

See also `../README.md` (Layout / Decomposition state). This doc is the **interface map**;
`REFACTOR.md` is the **change plan**.

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
