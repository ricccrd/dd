# Non-PIE low-address hosting on Apple Silicon: the `guest_base` bias-fold

**Status:** research / design (definitive). **Owner:** JIT runtime. **Scope:** how `dd` should host a
non-PIE Linux `ET_EXEC` (baked at a fixed low vaddr ≈ `0x400000`) inside a codesigned arm64 Mach-O
engine whose mandatory 4 GiB `__PAGEZERO` makes the low 4 GiB unmappable. This doc supersedes the
`__PAGEZERO`-shrink plan in [`fix-nonpie-crash.md`](fix-nonpie-crash.md), which is now a *proven
dead-end on Apple Silicon* (see §2). It does **not** change engine code — it is the blueprint for the
software fix.

---

## 1. Problem statement

A GNU-toolchain run inside a container (`gcc → cc1 → as → ld → a.out`) produces and executes **non-PIE
`ET_EXEC`** images. The GNU `ld` aarch64/x86_64 default links them at a *fixed* text base **`0x400000`**
with **un-relocated absolute references** — absolute branch targets *and* absolute data loads/stores —
baked at that link vaddr. A non-PIE expects to be loaded **exactly there**.

The `dd` engine is a codesigned arm64 Mach-O on Apple-Silicon macOS. The OS gives the engine's main
executable a mandatory **`__PAGEZERO` of 4 GiB** (`0x0 … 0x1_0000_0000`), reserved unmapped. So the
guest's baked low addresses (`0x4xxxxx`) point into the engine's own pagezero and are unmappable.

Today `dd` copes by mapping the image **HIGH** (`+bias`), redirecting *code* PCs through the dispatcher
(`frontend/{x86_64,aarch64}/dispatch.c`, `dispatch_hooks.h`) and materializing `adr/adrp` against the
*low* (un-biased) PC so produced pointer values match the baked low pointers (`pcrel_base()` in
`frontend/aarch64/translate.c:19`). The residual: **absolute data loads/stores to `0x4xxxxx`** are real
memory accesses, not dispatch events, so they fault. A `SIGSEGV` fixup handler (`nonpie_fixup`) serves
each one from the high mapping `+bias`. It is **correct but catastrophically slow** — one trap per low
access; `cc1` takes ~400 s — and the dense post-`fork` layout makes it fragile (see
`fix-nonpie-crash.md` §a for the `pc==fault==0x4xxxxx` signature).

The fix every mature user-mode emulator uses is **`guest_base`**: keep guest-visible addresses *low*
(= the guest vaddr the binary expects) and add a fixed bias to the *actual* host memory access. This
doc establishes that this is the only viable approach on Apple Silicon, surveys how others implement
it, and specifies the `dd` implementation.

---

## 2. Proven constraints (verified against the platform and the literature)

| Constraint | Status | Evidence |
|---|---|---|
| `__PAGEZERO` ≤ ~4 GiB on the **main** executable → kernel/dyld reject ("Malformed Mach-O") / SIGKILL on arm64 | **CONFIRMED, dead-end** | Apple: "arm64 code must be in an ASLR binary; a custom `pagezero_size` is incompatible." Building `arm64-apple-macos` with `-pagezero_size 0x10000` → *Malformed Mach-O*; `0x100010000` runs. arm64 mains load at `0x1_0000_0000` so a 32-bit-truncated pointer stays < 4 GiB and traps. [Apple DevForums 655950](https://developer.apple.com/forums/thread/655950), [676684](https://developer.apple.com/forums/thread/676684); memory note: any pagezero ≤ `0xF0000000` dies. |
| A `posix_spawn`'d helper to host the guest low → helper is **itself a main executable** → same 4 GiB pagezero | **CONFIRMED, dead-end** | Same rule applies to every main Mach-O; the helper's own pagezero re-covers `0x0…0x1_0000_0000`. |
| `mmap(MAP_FIXED, 0x400000)` → `ENOMEM`; `mach_vm_map`/`mach_vm_allocate` fixed at `0x400000` → `KERN_INVALID_ADDRESS` | **CONFIRMED, dead-end** | pagezero region is not reclaimable from inside the process (memory note; consistent with `__PAGEZERO` being a real VM reservation, [Mach-O / __PAGEZERO semantics](https://en.wikipedia.org/wiki/Mach-O)). |
| `-pagezero_size 0x400000` + `MAP_FIXED` pin (the `fix-nonpie-crash.md` plan) | **REJECTED** — relies on shrinking the main exe's pagezero, which §1 above kills on Apple Silicon | The 4 MiB-pagezero variant cannot even load. The plan's reasoning is sound *given* a shrinkable pagezero — but that premise is false here. |
| `MAP_32BIT` reclaiming low memory | **N/A** | `MAP_32BIT` maps in the low 2 GiB *if available*; the low 4 GiB is pagezero, so it cannot return `0x4xxxxx` either. |

**Conclusion:** the low 4 GiB is *irrecoverable* for any process in the engine's task. The guest's baked
low addresses can never be made real. The only solutions keep the guest *mapped high* and reconcile the
low/high mismatch — either lazily (today's fault handler) or **eagerly at translate time (guest_base
bias-fold)**.

---

## 3. How mature emulators solve it (survey)

| System | Host→guest memory model | Reclaims low mem? | Applicability to `dd` |
|---|---|---|---|
| **QEMU linux-user** | `guest_base` offset added to *every* guest access. `g2h(x) = x + guest_base`, `h2g(x) = x − guest_base`. In the **aarch64 TCG backend** `guest_base` lives in a **reserved host register `x28` (`TCG_REG_GUEST_BASE`)** and each access is a single register-offset load `ldr xd,[x28, xaddr]` — *same cost as a normal load*. `probe_guest_base()` picks the base by probing the host map with `MAP_FIXED_NOREPLACE`, honoring `mmap_min_addr`; `-R reserved_va` reserves a contiguous guest window. For a *fixed* (non-PIE) image it tries `pgb_fixed()` at the requested vaddr first. | On Linux, yes (low mem is free once `mmap_min_addr` allows). On macOS it would set `guest_base ≠ 0` and map high — exactly our case. | **Direct model for dd.** The register-held base + single register-offset load is the gold standard. dd already steals `x28` for the cpu pointer, so dd needs a *different* reserved reg (see §6). [tcg-target.c.inc](https://raw.githubusercontent.com/qemu/qemu/master/tcg/aarch64/tcg-target.c.inc), [probe_guest_base rewrite (PULL 14/14)](https://www.mail-archive.com/qemu-devel@nongnu.org/msg980793.html), [emulation docs](https://www.qemu.org/docs/master/about/emulation.html) |
| **blink** (jart) | Optional **linear translation**: identity-map guest at a fixed high skew, e.g. guest `0x400000` → host `0x400000 + 0x088800000000`; "each memory operation is a simple addition." Explicitly "works around **Apple's restriction on 32-bit addresses**" and "works great on Apple Silicon." Falls back to a PML4T+TLB software MMU (`-m`) when the linear scheme can't apply. | No — adds a skew, never uses low mem. | **Strong corroboration.** blink is an x86-64 emulator that *already runs on Apple Silicon* using precisely the bias-fold (a constant skew + one add per access). Validates feasibility and the perf model. [blink README](https://github.com/jart/blink/blob/master/README.md), [DeepWiki](https://deepwiki.com/jart/blink) |
| **Rosetta 2** | Translates **Mach-O** x86_64, which itself has a **4 GiB `__PAGEZERO` and loads high** (`__TEXT` at `0x1_0000_0000+slide`). The kernel hands x86_64 images to a Rosetta stub instead of dyld; AOT caches live in `/Library/Apple/`. There is **no fixed-low-address case** — macOS x86_64 binaries are PIE/high just like arm64. | N/A (never needs low mem). | **No help.** Rosetta sidesteps the entire problem because it never runs a non-PIE-at-`0x400000` ELF. Confirms Apple itself does not solve "low fixed address" — it avoids it. [Apple: Rosetta env](https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment), [eclecticlight explainer](https://eclecticlight.co/2022/12/10/explainer-rosetta-2/) |
| **FEX-Emu** | Linux-only (Arm64 Linux). For 32-bit guests it **reserves all host memory > 4 GiB** so the low 32-bit space is free for the guest, then identity-maps; a custom mmap allocator forces guest maps into the right band. Relies on Linux letting it own low memory (`mmap_min_addr`, personality). | Yes — on Linux. | **Mechanism not portable** (depends on owning low mem). The *allocator discipline* (force every guest map into a chosen band) is borrowable and matches `fix-nonpie-crash.md`'s `hi_mmap`. [FEX ProgrammingConcerns](https://github.com/FEX-Emu/FEX/blob/main/docs/ProgrammingConcerns.md), [32-bit syscall woes](https://wiki.fex-emu.com/index.php/Development:32Bit_Syscall_Woes) |
| **box64 / box86** | Linux/Asahi. Runs the x86_64 ELF with low memory available from the host kernel; needed 16 KiB-page support for Asahi. Not a macOS solution. | Yes — on Linux. | **Not applicable** (no macOS userland port; assumes Linux low mem). [box64 0.2.8](https://www.phoronix.com/news/Box64-0.2.8-Released) |
| **Wine / Hangover** | PE/Wine on Arm; uses Wine's own preloader to reserve the low address ranges Windows expects, again on Linux. | Yes — on Linux. | **Not applicable** to the macOS pagezero constraint. |

**Synthesis:** every system that faces "guest wants a fixed low address the host won't give" uses a
**constant bias added per memory access** (QEMU `guest_base`, blink linear skew). The ones that "reclaim
low memory" (FEX/box64/Wine) only do so on **Linux**, which lets a process own the low pages — macOS
does not. **The bias-fold is the portable, proven answer, and blink proves it works on Apple Silicon.**

---

## 4. macOS / Apple-Silicon VM feasibility (definitive)

- **`__PAGEZERO` is mandatory ≈4 GiB for main executables on arm64.** Not a default you can lower:
  arm64 mains must be ASLR; a small `-pagezero_size` yields *Malformed Mach-O* and the kernel refuses
  to exec. ([Apple DevForums 655950](https://developer.apple.com/forums/thread/655950),
  [676684](https://developer.apple.com/forums/thread/676684)). The low 4 GiB is therefore unmappable
  in-process — no `mmap`, `mach_vm_allocate`, `mach_vm_map`, or `MAP_32BIT` can return `0x4xxxxx`.

- **A secondary task / helper does not escape it.** Any `posix_spawn`'d process is a *main executable*
  and gets its own 4 GiB pagezero, re-covering the low region. You cannot make a helper whose low 4 GiB
  is usable. Even if you could, serving the guest's low accesses by `mach_vm_remap` of the helper's low
  pages into the engine would require **a remap per page and an IPC/handler boundary** — strictly worse
  than the bias-fold (and the helper still can't own low memory). The
  [`mach_vm_remap`](https://developer.apple.com/documentation/kernel/1402218-mach_vm_remap) route is a
  dead-end at its root (no task can host the low pages).

- **dylibs/bundles have `-pagezero_size 0` *by construction* (they are not the main image)** — but a
  dylib loaded into the engine shares the engine task's address space, including its 4 GiB pagezero.
  Loading the guest as a dylib changes nothing about the low region.

- **The JIT / `allow-jit` entitlement** governs W^X (`MAP_JIT`, `pthread_jit_write_protect_np`); it has
  **no bearing** on pagezero or low-address availability. Relevant to the code cache, not to this bug.

- **There is no `mmap_min_addr` knob to lower.** macOS has no equivalent the way Linux does; the low
  bound is the hardware pagezero VM reservation, not a tunable sysctl.

**Verdict:** the low region is *truly unrecoverable*. The `fix-nonpie-crash.md` `MAP_FIXED`-pin plan is
infeasible on Apple Silicon (it presumed a shrinkable main-exe pagezero). The guest must stay mapped
high; correctness must come from translation-time address folding, not from acquiring low memory.

---

## 5. Ranked solutions

### S1 — Software `guest_base` bias-fold (RECOMMENDED)

Keep guest-visible addresses **low (= guest vaddr)**; reserve a host register holding the **bias**
(`= high_map_base − link_base`); fold `+bias` into the *effective host address* of every guest
load/store **at translate time**, only when `bias ≠ 0` (i.e. only for a non-PIE `ET_EXEC`). PIE keeps
`guest_base == 0` → **zero overhead, unchanged verbatim path**. This is QEMU's model and blink's linear
skew, and it directly extends dd's *existing* invariant: dd already makes guest pointer **values** low
(`pcrel_base()` for `adr/adrp`; baked absolute pointers are low). The only missing piece is folding the
bias at the *access* instead of trapping. **Eliminates the per-access fault entirely.** Feasible;
detailed in §6.

### S2 — Faster fault handler (status-quo refinement)

Keep `nonpie_fixup`, but reduce per-fault cost (decode the faulting insn, emulate, advance PC) or
pre-fault the touched pages. **Rejected as the primary fix:** a synchronous Mach exception / `SIGSEGV`
round-trip is microseconds; `cc1` issues millions of low accesses → still tens-to-hundreds of seconds.
It is a correctness fallback, not a performance solution. Useful only as the bias-fold's safety net for
exotic addressing forms (§6.4).

### S3 — Helper co-process hosting the low pages via Mach

A second task whose low memory backs the guest, accessed via `mach_vm_remap`/shared memory. **Rejected:**
§4 proves no task can own the low 4 GiB; and even granting it, the remap-per-page + IPC cost dominates.
Not viable.

### S4 — Software MMU / TLB (blink `-m` mode)

Full page-table virtualization of guest memory (radix tree + TLB) so any guest address maps anywhere.
**Rejected for dd:** dd is a fast transliterator/recompiler, not an interpreter; a software MMU on the
hot path would regress the *entire* matrix (PIE included) by orders of magnitude. S1 gives the same
correctness for non-PIE at a fraction of the cost and **zero** PIE cost.

**Recommendation: implement S1, keep S2 as the fallback for un-foldable corner cases.**

---

## 6. S1 in detail — the bias-fold for `dd`

### 6.0 Invariant and the two frontends

Define `guest_base = bias` (the existing `g_nonpie_bias`), nonzero **only** for a non-PIE `ET_EXEC`.
Guest registers and pointer *values* stay **low** (guest vaddr). The real bytes live at `low + bias`
in the high mapping. Therefore:

- `g2h(p) = p + bias` at every guest memory access **and** at every syscall that dereferences a guest
  pointer.
- `h2g(p) = p − bias` for any host address the kernel hands back that must be returned to the guest
  (the `mmap`/`brk`/`mremap` return path).
- `adr/adrp` / baked absolute pointers already produce low values (no change — `pcrel_base()` stays).

The two frontends differ fundamentally and must be costed separately:

- **`frontend/x86_64`** is a *real decoder→emitter* (`decode.c` → `translate.c` → `emit.c`). It already
  computes an effective address for every memory operand before emitting the arm64 access. Folding is
  cheap and natural — exactly like QEMU's i386/x86_64→arm64 backend.
- **`frontend/aarch64`** is a *same-ISA transliterator*: it **copies `ldr/str/ldp/stp/ldxr/...`
  verbatim**, only MANGLE-ing instructions that name a stolen register (`is_stolen()`,
  `emit_mangled_x18()` in `translate.c`). There is no "effective address" abstraction — the addressing
  mode is baked into the copied instruction. Folding here means **de-transliterating every memory op**,
  which is the bulk of the work and the bulk of the cost. **This is the harder, load-bearing half** —
  and unfortunately it is exactly where the gcc-toolchain bug lives.

### 6.1 Register choice (aarch64 host)

QEMU reserves `x28` as `TCG_REG_GUEST_BASE`. dd **already** steals `x28` for the cpu pointer
(`cpu_aarch64.h:3`, `OFF_*`), plus `x18`, `x30`, and (with A1) `x16/x17`. Options:

1. **Dedicate one more host GPR as `GUESTBASE`** (e.g. `x27`), materialized once per context switch
   into the JIT, only meaningful when `bias ≠ 0`. Best per-access cost (the access becomes a single
   register-offset `ldr xd,[GUESTBASE, xaddr]`), but spends a register the guest could use → must be
   added to `is_stolen()` and mangled like the others. Since this is only needed for non-PIE runs, dd
   can make the stolen-set **mode-dependent**: steal `GUESTBASE` *only* when a non-PIE image is loaded,
   leaving the PIE matrix on today's exact stolen-set (no PIE regression).
2. **Keep `bias` in the cpu struct** (`cpu->guest_base`) and load it into a scratch per access. No extra
   stolen reg, but adds a dependent load before every memory op (worse, and more scratch pressure).

**Recommend option 1, mode-gated:** reserve `GUESTBASE` (x27) as a fifth stolen register *iff*
`g_nonpie_lo != 0`. The cost (one fewer guest GPR, extra mangles) is paid only by non-PIE guests, which
are already the slow/rare path. Materialize it once at JIT entry and after any `g2h`-relevant context
switch.

### 6.2 aarch64 fold — addressing-mode handling

For each guest memory instruction, when `bias ≠ 0`, rewrite to access `[GUESTBASE + effective_guest_addr]`.
The arm64 ISA gives one free lane — the **register-offset** form `LDR/STR Xt,[Xn,Xm{,ext}]` — but most
forms aren't that shape, so they need a scratch. Cases (reuse the existing `emit_mangled_x18()` scratch
machinery — pick a free GPR, spill to `cpu->mscratch`, compute, restore):

| Guest form | Rewrite |
|---|---|
| `LDR/STR Xt,[Xn]` (no offset) | `LDR Xt,[GUESTBASE, Xn]` — **1 insn, free** (register-offset form) |
| `LDR/STR Xt,[Xn,#imm]` | `S = Xn + imm` (or fold imm into the access); `LDR Xt,[GUESTBASE, S]` |
| `LDR/STR Xt,[Xn,Xm{,ext}]` | `S = Xn + (Xm ext)`; `LDR Xt,[GUESTBASE, S]` |
| pre/post-index (writeback) | compute `S`, do `[GUESTBASE,S]`, then apply the writeback to `Xn` (still the *low* value) |
| `LDP/STP` | one base computation `S`; pair access `[GUESTBASE,S]` / `[GUESTBASE,S+8]` |
| `LDXR/STXR/LDAXR/...` (exclusives) | **No register-offset form.** Must compute `S = Xn + bias` into a scratch and use `[S]` directly. The reservation granule must be on the *host* (high) address — using `[GUESTBASE,X]` is illegal here, so the scratch-add is mandatory. |
| LSE atomics (`LDADD`, `SWP`, `CAS`, ...) | base-register only (`[Xn]`) → compute `S = Xn + bias`, use `[S]`. (Interacts with the existing LSE-upgrade opt — keep the upgrade, fold the base.) |
| `PRFM`, `DC`/`IC` maintenance with address | fold the base into a scratch as above (or skip — prefetch is hint-only). |

**Cost:** a verbatim 1-insn load becomes ~2–4 host insns (scratch spill + add + access + restore) for
the offset/exclusive forms, and stays 1 insn for the bare `[Xn]` form. Register pressure rises (one
scratch per memory op, plus the stolen `GUESTBASE`). **But this is only for non-PIE blocks** — and it
replaces a *fault per access*. Net: cc1 goes from ~400 s to "a few× a clean transliteration," i.e.
seconds, not minutes.

Optimization opportunities (later): (a) when a guest base register is provably already biased within a
block (rare, since values must stay low for identity), skip re-adding; (b) peephole the
`add S,Xn,#imm` + `ldr [GUESTBASE,S]` into the register-offset form when `imm` fits a scaled offset and
`Xn` is free to clobber — generally it isn't (the low value is reused), so the scratch path dominates.

### 6.3 x86_64 fold — at emit time

In `frontend/x86_64`, the ModRM/SIB effective-address computation already lands in a host register
before the arm64 access is emitted (`emit.c`). Add `+GUESTBASE` to that computation when `bias ≠ 0`:
the resulting host address is `base + index*scale + disp + bias`, then a single `ldr/str [host_addr]`.
This is the QEMU x86→arm path and costs **one extra `add`** (or folds into the existing address add)
per memory operand — cheap. RIP-relative operands already resolve against the low (un-biased) RIP via
the dispatcher redirect (`dispatch.c:54`), so their computed pointer is low and gets `+bias` like any
other. The Go `firstmoduledata` rebasing (`frontend/x86_64/elf.c:90`) becomes unnecessary under a true
fold — but keep it until the fold is proven, then retire it.

### 6.4 Syscall boundary (g2h/h2g) — shared by both frontends

dd already has the right hook: `os/linux/service.c:52-60` translates non-PIE pointer args
(`a >= lo && a < hi ? a + bias : a`). Under the explicit-fold model this must be applied **consistently
to every guest pointer that the sentry/service dereferences** (iovecs, `struct` args, path buffers,
`mmap` hints) — i.e. promote that ad-hoc redirect to a single `g2h()` used at every guest-memory touch
in the service layer, and add the inverse `h2g()` on the return path of `mmap`/`mremap`/`brk` so the
guest sees a *low* address it can then re-bias. This is exactly QEMU's `g2h`/`h2g` consolidation
([guest-host.h](https://www.mail-archive.com/qemu-devel@nongnu.org/msg1085102.html)). Keep
`nonpie_fixup` registered as the **safety net** for any memory form the fold misses (S2), so a missed
case degrades to "slow but correct," never to a crash.

### 6.5 Address-space reservation (borrow QEMU/FEX discipline)

To make the fold robust, the high mapping must be *contiguous and stable*, and no other guest mapping
may collide with the band the guest's low addresses fold into. Adopt the `hi_mmap` cursor from
`fix-nonpie-crash.md` §B.3 (force every non-fixed guest map high) **without** the pagezero shrink:
reserve a single contiguous high window for the non-PIE image `[high_base, high_base+span)` (QEMU's
`-R reserved_va` analogue), and route `mmap(NULL)`/brk/stack high so the guest's own allocations live in
a predictable band. Pick `high_base` so `bias = high_base − link_base` is large and constant for the
process lifetime. Probe it with the QEMU technique — try the desired base, fall back by scanning the
host map — adapted to macOS (`mach_vm_region` to find a hole; `mmap` without `MAP_FIXED` then verify, or
`VM_FLAGS_FIXED` via `mach_vm_map` at a chosen high address).

### 6.6 Correctness notes

- **Self-modifying code / SMC:** unchanged — the existing `ic ivau`/translation-cache-drop path
  (`translate.c:815`, `NOSMC`) keys on guest addresses; biasing the *access* doesn't change which guest
  page is dirtied (still tracked by guest vaddr).
- **Signals / `ucontext`:** a guest fault's reported fault address must be reported in the **guest
  (low)** space (`h2g` the host fault addr) so the guest's handler sees the address it expects — mirror
  QEMU's `host_signal_handler` translation.
- **`fork`:** the bias is a constant per image; the child inherits `g_nonpie_*` and `GUESTBASE`. The
  fold removes the dense-layout fragility entirely (no low access ever faults), which *is* the original
  bug.

---

## 7. Phased implementation plan

**Phase 0 — g2h/h2g consolidation (no behavior change, de-risk).**
Promote `service.c`'s ad-hoc non-PIE pointer redirect to a single `g2h()`/`h2g()` used at every guest
pointer touch and on the `mmap`/`brk`/`mremap` return path. Keep `nonpie_fixup` active. Matrix stays
240-green (PIE has `bias==0`, so `g2h` is identity). Files: `os/linux/service.c`,
`os/linux/container/vfs.c`.

**Phase 1 — x86_64 emit-time fold (the easy, high-value half).**
Add `+GUESTBASE` to the effective-address computation in `frontend/x86_64/emit.c` when `bias ≠ 0`;
reserve/​materialize a host base reg for the x86 path. Gate behind `DD_GUESTFOLD=1`. Validate the jit86
non-PIE path with `nonpie_fixup` *disabled* under the gate (faults must be zero). Files:
`frontend/x86_64/{emit.c,translate.c,decode.c}`, `frontend/x86_64/elf.c` (retire `go_rebase_nonpie`
once proven). Expect: non-PIE x86 workloads correct with no faults; PIE unchanged.

**Phase 2 — aarch64 transliterator fold (the hard, load-bearing half).**
Add a `GUESTBASE` stolen register (mode-gated on `g_nonpie_lo != 0`; extend `is_stolen()`), and a memory
-op rewriter alongside `emit_mangled_x18()` that folds the base per §6.2 for every load/store/pair/
exclusive/atomic form. Gate behind `DD_GUESTFOLD=1`. Files: `include/cpu_aarch64.h` (GUESTBASE off /
stolen set), `frontend/aarch64/translate.c` (memory-op detection + fold; reuse the `mscratch` spill),
`frontend/aarch64/dispatch_hooks.h` / `jit/emit_arm64.c` (materialize GUESTBASE on entry),
`targets/linux_aarch64.c` (set GUESTBASE = bias at context setup). Validate: `compile/{hello,c-primes,
cpp-stl}` XPASS on `LinuxAarch64`; `CRASHDBG` gcc-bundle fork-churn shows **no** `pc==fault==0x4xxxxx`.

**Phase 3 — reservation hardening + remove the fault path.**
Wire the `hi_mmap` high-cursor + a contiguous reserved window for the non-PIE image (§6.5, no pagezero
change). Once Phases 1–2 are green across `make test` / `test-realsw` / `test-docker` / `soak/forkchurn`,
make `DD_GUESTFOLD` the default, keep `nonpie_fixup` only as the un-foldable-form safety net, and drop
the `compile` `.xfail` (`dd-tests/src/cases/mod.rs:186`). Update `docs/PLAN.md` (move the non-PIE bug
out of *Deep bugs* / *Platform limitations*) and mark `fix-nonpie-crash.md` superseded.

**Build (`build.rs`):** **no `-pagezero_size` flag** — that was the dead-end. The fold is pure codegen;
`build.rs` is unchanged except possibly a `-DDD_GUESTFOLD` default once Phase 3 lands.

---

## 8. Performance expectations

| Path | PIE (common case) | Non-PIE (gcc toolchain) |
|---|---|---|
| **Today (fault handler)** | native-quality, 0 overhead | ~400 s for `cc1` — one trap per low access; fragile post-fork |
| **x86_64 fold (Ph.1)** | unchanged (bias 0) | +1 `add` per memory operand ≈ QEMU user-mode level; **no faults** — seconds |
| **aarch64 fold (Ph.2)** | **unchanged** (mode-gated; stolen-set and verbatim path identical to today) | memory ops expand ~2–4× host insns (offset/exclusive forms), bare `[Xn]` stays 1 insn via register-offset; register pressure up by 1–2 — **but no faults**. Realistic: low-single-digit× a clean transliteration, i.e. **seconds, not 400 s** |

Residual vs native: the aarch64 fold's memory-op expansion is the price of not owning low memory — the
same price QEMU/blink pay, and far below the OoO core's ability to hide a couple of extra ALU ops on the
load path. Crucially the **PIE matrix sees zero change** because the fold and the extra stolen register
are gated on `g_nonpie_lo != 0`. blink demonstrates this exact model running x86-64 Linux on Apple
Silicon at usable speed ([blink](https://github.com/jart/blink/blob/master/README.md)), which is the
empirical proof that the bias-fold is both correct and adequately fast on this platform.

---

## 9. Sources

- QEMU aarch64 TCG backend (`TCG_REG_GUEST_BASE` = x28, register-offset load): https://raw.githubusercontent.com/qemu/qemu/master/tcg/aarch64/tcg-target.c.inc
- QEMU `probe_guest_base` rewrite (MAP_FIXED_NOREPLACE, pgb_fixed/pgb_dynamic, mmap_min_addr): https://www.mail-archive.com/qemu-devel@nongnu.org/msg980793.html · https://www.mail-archive.com/qemu-devel@nongnu.org/msg980442.html
- QEMU emulation / user-mode model: https://www.qemu.org/docs/master/about/emulation.html
- QEMU TCG memory accesses (softmmu vs user, guest_base): https://airbus-seclab.github.io/qemu_blog/tcg_p3.html
- QEMU g2h/h2g consolidation (guest-host.h): https://www.mail-archive.com/qemu-devel@nongnu.org/msg1085102.html
- blink linear translation / Apple-Silicon 32-bit-address workaround: https://github.com/jart/blink/blob/master/README.md · https://deepwiki.com/jart/blink
- Rosetta 2 translation environment / loader: https://developer.apple.com/documentation/apple-silicon/about-the-rosetta-translation-environment · https://eclecticlight.co/2022/12/10/explainer-rosetta-2/
- FEX address-space reservation: https://github.com/FEX-Emu/FEX/blob/main/docs/ProgrammingConcerns.md · https://wiki.fex-emu.com/index.php/Development:32Bit_Syscall_Woes
- box64 on Apple Silicon (Asahi, 16K pages): https://www.phoronix.com/news/Box64-0.2.8-Released
- macOS arm64 `__PAGEZERO` mandatory / `pagezero_size` rejection: https://developer.apple.com/forums/thread/655950 · https://developer.apple.com/forums/thread/676684
- Mach-O `__PAGEZERO` semantics: https://en.wikipedia.org/wiki/Mach-O
- `mach_vm_remap`: https://developer.apple.com/documentation/kernel/1402218-mach_vm_remap
