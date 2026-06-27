# Engine dedup: lift the x86-64 host engine onto the shared `jit/` engine

**Status:** design / roadmap. Executable by an engineer next.
**Scope:** subsystem #1 — finish the jit86 engine dedup. The `os/linux/` personality is
already shared via the cpu-interface seam (`frontend/x86_64/abi.h` `G_*` + `sysmap.h`).
What is left is the **host engine**: x86-64 still carries its own
`frontend/x86_64/{cache,emit,dispatch}.c` instead of the shared
`jit/{cache,emit_arm64,dispatch}.c` that aarch64 uses.

**Goal:** the code cache + dispatcher become cpu-agnostic and live once in `jit/`.
The cpu struct, the x86 decoder, the host emitters, and x86 flag emulation stay
genuinely per-arch. Payoff: one engine, two thin frontends.

**Hard constraint observed during this design:** the build is a **unity TU** — each
target (`targets/linux_aarch64.c`, `targets/linux_x86_64.c`) `#include`s the `.c`
files directly, so every global/function has exactly one definition across the whole
TU. "Lifting a file" = the x86 TU stops `#include`-ing `frontend/x86_64/<f>.c` and
starts `#include`-ing `jit/<f>.c`. Any symbol the shared file does **not** define but
the x86 frontend still needs must be defined exactly once elsewhere in the x86 TU.

---

## 0. Ground truth (files read)

| Concern | aarch64 (shared engine) | x86-64 (frontend copy) |
|---|---|---|
| code cache + map + chaining | `jit/cache.c` (107 lines) | `frontend/x86_64/cache.c` (99 lines) |
| host emitters + IBTC/IC + trampoline glue | `jit/emit_arm64.c` | `frontend/x86_64/emit.c` |
| run_block trampoline + run_guest loop | `jit/dispatch.c` | `frontend/x86_64/dispatch.c` (loop) + `frontend/x86_64/translate.c:1884` (trampolines) |
| cpu struct + OFF_* | `include/cpu_aarch64.h` | `include/cpu_x86_64.h` |
| cpu seam (G_* contract) | `frontend/aarch64/abi.h` | `frontend/x86_64/abi.h` |
| unity TU | `targets/linux_aarch64.c` | `targets/linux_x86_64.c` |
| decoder / translate | `frontend/aarch64/translate.c` | `frontend/x86_64/{decode,translate}.c` |

Shared already (both TUs `#include` the same file): `os/linux/container/state.c`,
`thread.c` (defines `run_guest`'s caller env + sets `g_threaded=1`), `signal.c`
(`g_pending`, `SIGRETURN_PC`, `do_sigreturn`, `maybe_deliver_signal`), `vfs.c`,
`netns.c`, `fscache.c` (uses the `CLK`/`CUL` lock macros), `service.c` (`service()`).
`elf.c` is shared by aarch64 but per-arch for x86 (`frontend/x86_64/elf.c`).

---

## A. The accessor seam

The shared cache+dispatch must compile against BOTH cpu structs. The seam is a small
set of macros, each defined once per arch in `frontend/<arch>/abi.h` (or, for the
literal OFF_* and reason codes, in `include/cpu_<arch>.h` which is already per-arch and
already `#include`d before the engine files).

### A.1 Already present (no work) — used by `os/linux/`, reusable by the engine

Defined in both `frontend/{aarch64,x86_64}/abi.h`:

```
G_PC(c)      -> aarch64: (c)->pc       x86: (c)->rip      [lvalue]   guest program counter
G_SP(c)                                                              (not needed by engine; used by os/linux)
```

`G_PC(c)` is the **only** abi.h accessor the shared dispatcher needs (the loop reads
and writes the guest PC). Everything else the dispatcher touches is engine state
(`reason`, `exited`, `redirect`, `ic_*`) or already-shared globals.

### A.2 Already present (no work) — per-arch literals in `include/cpu_<arch>.h`

These are already `#define`d as integer literals in each cpu header and already in
scope when the engine files are included. The engine uses them today via the aarch64
header; the x86 header defines the same names with x86 values:

```
OFF_SP, OFF_PC/OFF_RIP, OFF_NZCV, OFF_RSN, OFF_HSP, OFF_HSAVE, OFF_HOSTV, OFF_V
R_BRANCH (=0), R_SYSCALL (=1)
```

Note the PC offset name differs: aarch64 `OFF_PC` (256) vs x86 `OFF_RIP` (128). The
shared engine code that bakes the PC store offset must use a single name. **Add an
alias** `#define OFF_PC OFF_RIP` in `cpu_x86_64.h` (or rename uses to a neutral
`OFF_GPC`). This is the only naming collision in the OFF_* set.

### A.3 NEW accessors to add (the actual seam work)

These are the genuine per-arch differences the shared cache/dispatch must hide. Each is
defined once per arch in `frontend/<arch>/abi.h`:

| Macro | aarch64 value | x86 value | Why it differs |
|---|---|---|---|
| `G_GPC_HASH_SHIFT` | `2` | `0` | `map_idx`/`map_put` hash `(gpc >> S)`. aarch64 PCs are 4-byte aligned (>>2 spreads); x86 PCs are byte-granular (>>0). Pure tuning constant. |
| `G_SHADOW_CLEAR(c)` | `(c)->ssp = 0` | `((void)0)` | On wholesale cache flush, aarch64's §B shadow stack holds host_ret pointers into the dropped cache → must reset. x86 has no shadow stack. (Note: this is the *engine*-side reset on flush, distinct from the existing `G_SHADOW_RESET(c)` which `os/linux` calls on fork/exec — same idea, reuse the name if convenient.) |
| `G_IBTC_FILL(c)` | aarch64 fill (shared hash + per-site IC literal patch, body−8 bias) | x86 fill (shared hash only, plain body) | The dispatcher's IBTC-miss handler is tightly coupled to what `emit_ibranch` emitted; see §B.3. Cleanest as a **frontend-provided function**, not a value macro. |
| `G_BLOCK_EXIT(c)` | empty (aarch64 has only R_SYSCALL/R_BRANCH) | x86 reason switch: R_CPUID/R_X87FLD/R_X87FSTP/R_DIV/R_IDIV/99 | Post-`run_block` non-syscall reason handling; see §B.4. Frontend-provided function returning "continue/break/fallthrough". |
| `G_DISPATCH_DEBUG(c)` | empty | x86 instrumentation block (`g_prevpc`, `g_disp_n`, trace cap, `g_w8` watchpoint, malloc tracking) | Optional debug hook at top of loop; see §B.5. Empty macro on aarch64. |

`run_block` / `block_return` are **not** macro-ized — they stay as whole per-arch
naked functions (§B.1).

---

## B. Function-by-function merge table

Legend: **IDENT** = byte-identical or trivially so (mergeable as-is); **ACCESSOR** =
differs only by a cpu field / tuning constant (mergeable behind an A.3 macro);
**PER-ARCH** = irreducibly per-arch (stays in the frontend).

### B.1 `cache.c`  (`jit/cache.c` vs `frontend/x86_64/cache.c`)

| Symbol | Class | Notes |
|---|---|---|
| `CACHE_SZ`, `g_cache`, `g_cp`, `g_emit_start` | IDENT | identical `64<<20` arena + bump pointer. |
| `g_map[MAP_N]` struct `{gpc,host,body}` | IDENT | `MAP_N` (65536) is defined in the shared `os/linux/container/state.c:73` — already common to both TUs. |
| `map_idx`, `map_put` | ACCESSOR | only difference is the hash shift `(gpc>>2)` vs `(gpc>>0)` → `G_GPC_HASH_SHIFT`. |
| `map_host`, `map_body` | IDENT | |
| `IBTC_N`, `g_ibtc[]` | IDENT | both `8192`, struct `{target,body}`. |
| `g_pend[]`, `add_pend`, `patch_links_to` | SUPERSET→IDENT | `jit/cache.c` is a strict superset: it carries an `is_bl` field + `add_pend2(slot,target,is_bl)` for §B host-`bl` chaining, with `add_pend(slot,target)` a 2-arg wrapper that passes `is_bl=0`. x86 only ever needs the `is_bl=0` (`b`) path, which the shared `patch_links_to` already emits. **Lift the shared one verbatim; x86 callers keep calling `add_pend` → `is_bl=0`.** |
| locks: `g_jit_lock` (+ `g_cache_lock` in shared) , `CLK`/`CUL` | ACCESSOR/reconcile | shared `jit/cache.c` has **two** mutexes: `g_jit_lock` (translation, taken in dispatch) and `g_cache_lock` (FS metadata, guarded by `CLK`/`CUL`, consumed by shared `fscache.c`). x86's copy has only `g_jit_lock` and points `CLK`/`CUL` at it. Lifting gives x86 a dedicated FS-metadata lock — strictly finer-grained and more correct. No behavior regression: single-threaded (`g_threaded==0`) takes no lock either way. |
| x86 debug/diagnostic globals declared in `frontend/x86_64/cache.c` | RELOCATE | `g_trace, g_prof, g_noibtc, g_itrace, g_disp_n, g_ibtc_fill, g_tracecap, g_diag, g_nochain, g_loadbase, g_w8, g_w8v, g_malloc_n, g_exe_path, g_self_path, g_cpu_key, g_prevpc(in dispatch)`. The shared `jit/cache.c` defines only a subset (`g_prof`, `g_cpu_key`; `g_trace`/`g_exe_path` live in `jit/emit_arm64.c`). **These must move to a new tiny x86 glue file** (`frontend/x86_64/engine_glue.c`, §C) so the x86 TU still defines them exactly once after dropping its `cache.c`. |

**Conflict to resolve when x86 includes `jit/cache.c` + `jit/emit_arm64.c`:**
`g_trace`, `g_exe_path`, `g_prof`, `g_cpu_key` get defined by the shared engine files.
The x86 frontend currently defines them in its own `cache.c`/`emit.c`. After lifting,
delete the x86 duplicates so each is defined once. (For the cache-only first PR we keep
x86 `emit.c`, so be careful which file owns each global — see §D.)

### B.2 `cache.c` verdict
Cache is the **clean** merge: one ACCESSOR (`G_GPC_HASH_SHIFT`), one SUPERSET that's
already compatible (`g_pend`/`patch_links_to`), one lock reconciliation that only
improves correctness, and a globals-relocation chore. No emitted-code semantics change.

### B.3 `dispatch.c`  (`jit/dispatch.c` vs `frontend/x86_64/dispatch.c`)

| Symbol / region | Class | Notes |
|---|---|---|
| `run_block(cpu, code)` naked trampoline | PER-ARCH | Saves host callee-saved + `q8..q15` + `host_sp` into the cpu struct at **literal** offsets. aarch64: `#288..#376` (x19..x30), `q8..q15 @#896`, `host_sp@#280`. x86: `#176..#264`, `q8..q15@#272`, `host_sp@#168`. Offsets differ AND aarch64 saves cpu-in-x0 model while x86 pins cpu in x28. Naked asm needs immediate literals (can't compute the non-contiguous `OFF_HSAVE+8k` chain from a macro), so **keep whole, per-arch.** |
| `block_return()` naked trampoline | PER-ARCH | Same. **Key irreducible difference:** aarch64 enters `block_return` with cpu in **x0** (no GPR is permanently reserved; all 31 are guest regs); x86 enters with cpu pinned in **x28** (only 16 guest GPRs, so host x28 is free to hold cpu). This is the register-model divergence — not hide-able behind an offset macro. |
| `run_guest()` loop skeleton | ACCESSOR | The while-loop body is the same shape: SIGRETURN check → lock → `map_host(G_PC)` → translate-on-miss (+ flush) → IBTC fill → unlock → trace → `run_block` → service reason. Mergeable using `G_PC(c)` + the hooks below. |
| ↳ SIGRETURN_PC check | IDENT | both `if (G_PC(c)==SIGRETURN_PC) do_sigreturn(c);` (`SIGRETURN_PC` from shared `signal.c`). |
| ↳ non-PIE redirect (`g_nonpie_lo`) | ACCESSOR/IDENT | aarch64-only today; `g_nonpie_*` come from shared `os/linux/elf.c`. For x86 (`frontend/x86_64/elf.c`) they're 0 → the guard is a no-op. Either declare the globals for both, or wrap in `#ifdef`. Lowest-risk: keep the line; ensure `g_nonpie_*` exist in x86 TU (define as 0 in `frontend/x86_64/elf.c` or glue). |
| ↳ translate-on-miss + wholesale flush | ACCESSOR | identical except aarch64's flush also does `c->ssp = 0` (drop shadow host_rets) → `G_SHADOW_CLEAR(c)`. Order of `sys_icache_invalidate` vs `patch_links_to` is identical. |
| ↳ IBTC fill after miss | PER-ARCH→hook | aarch64 keys off `c->ic_site` (0=none, 1=shared-only, else=per-site IC literal addr), stores `body−8` (indirect-entry stub restores guest x16/x17), and patches the per-site monomorphic IC literals in the W^X cache. x86 keys off `c->ic_miss` (0/1), stores plain `body`, no per-site IC (x16–x21 are free scratch, no stash/restore). This is the IBTC engine difference. **Extract to `G_IBTC_FILL(c)` provided by each frontend's emit file** (pairs the fill with the probe that emitted it). |
| ↳ post-`run_block` reason handling | PER-ARCH→hook | aarch64: `R_SYSCALL→service`, else `R_BRANCH`. x86: switch over `R_CPUID/R_X87FLD/R_X87FSTP/R_DIV/R_IDIV/99` then `R_SYSCALL`. **Extract the non-syscall part to `G_BLOCK_EXIT(c)`** (returns an action: handled-continue / fatal-break / fell-through-to-syscall). aarch64's is empty. |
| ↳ syscall service + pc advance | ACCESSOR | both `service(c); if(c->exited)break; if(c->redirect)c->redirect=0; else G_PC(c)+=4;`. Identical via `G_PC`. (x86 pre-advances rip in the emitter for some paths; verify the `+4` convention matches — see Risks.) |
| ↳ async signal delivery `if(g_pending) maybe_deliver_signal(c)` | IDENT | both call the same shared `signal.c`; only loop *position* differs (aarch64 end-of-iter, x86 top-of-iter). Pick one position; functionally equivalent (checked every iteration). |
| ↳ debug instrumentation | PER-ARCH→hook | x86-only `g_prevpc/g_curpc/g_disp_n/g_tracecap/g_nochain malloc-track/g_w8 watchpoint`. → `G_DISPATCH_DEBUG(c)` empty on aarch64. |

### B.4 `dispatch.c` verdict
Dispatch is the **harder** merge. The loop skeleton is shareable, but it needs **four
frontend hooks** (`G_IBTC_FILL`, `G_BLOCK_EXIT`, `G_SHADOW_CLEAR`, `G_DISPATCH_DEBUG`)
plus the two trampolines stay per-arch. Do it **after** cache.c.

### B.5 `emit.c` / `emit_arm64.c`  — **stays per-arch (do not merge)**

Both emit the same host ISA (arm64), so the low-level encoders (`emit32`, `e_str`,
`e_movconst`, `e_br`, NEON `e_*_q`, …) are near-identical and *look* mergeable. But the
**block ABI they emit is bound to the register model**, which is the whole point of the
two frontends:

- aarch64 `emit_prologue`/`emit_spill` save **all 31 GPRs** (x18/x28/x30 stolen,
  `is_stolen()`), recover cpu from **TLS** (`e_load_cpu`) at spill, use a red-zone
  scratch dance, and an IBTC `body_ind` stub at `body−8` to restore guest x16/x17.
- x86 `emit_prologue`/`emit_spill` save **16 GPRs**, pin cpu in **x28** for the whole
  block, and have a large per-arch surface: width-typed loads/stores, x86 **flag
  emulation** (`e_nzcv_save_ci/_c1/_setcf/_keepC` — the ARM-borrow vs x86-CF
  convention), SSE encoders, and the **x87** FPU-stack helpers.

`emit_ibranch`, `emit_chain_exit`, `emit_exit_const`, the prologue/spill, and the
`G_IBTC_FILL` body are all part of this per-arch block ABI. Keep emit fully per-arch.
The only shared-ish encoders (`emit32`, `e_movconst`, `e_br`, `sext`, the `q`-reg
load/stores) are cheap duplication; merging them would entangle the two register models
for no real payoff. **Explicitly out of scope.**

---

## C. The glue file

Create `frontend/x86_64/engine_glue.c` (new; ~30 lines) to own the x86-only globals the
shared engine files don't define, so the x86 unity TU keeps exactly-one definition of
each after dropping `frontend/x86_64/cache.c`:

```
g_noibtc, g_itrace, g_disp_n, g_ibtc_fill, g_tracecap, g_diag, g_nochain,
g_loadbase, g_w8, g_w8v, g_malloc_n, g_self_path, g_prevpc, g_curpc,
g_nonpie_lo/hi/bias (=0 for x86 if not already defined by frontend/x86_64/elf.c)
```

Globals the shared engine **does** define (delete the x86 duplicates): `g_trace`,
`g_exe_path` (in `jit/emit_arm64.c`), `g_prof`, `g_cpu_key` (in `jit/cache.c`). During
the **cache-only** first PR (x86 keeps its own `emit.c`), `g_trace`/`g_exe_path` are
still owned by x86 `emit.c`; the shared `jit/cache.c` does not redefine them, so no
collision. Re-audit ownership again at the dispatch PR (when `jit/emit_arm64.c` is NOT
pulled in — x86 keeps `emit.c` — so `g_trace`/`g_exe_path` ownership is unaffected;
only `g_prof`/`g_cpu_key` move from x86 `cache.c` to `jit/cache.c`).

`G_IBTC_FILL` and `G_BLOCK_EXIT` are defined as **functions** in `frontend/x86_64/emit.c`
and `frontend/x86_64/dispatch_tail.c` (or kept in `emit.c`), and as empty/trivial
inlines in `frontend/aarch64/`. They are wired in via the abi.h macros from §A.3.

---

## D. Safe incremental lift order

Each step is independently shippable and reversible (revert the `#include` swap). The
regression gate for every step is the **cross-engine matrix (~236 green)** — both the
aarch64 and x86 guest suites must stay green, because the shared files are compiled into
BOTH TUs.

1. **PR-1 — lift `cache.c`** (this is the EXACT first PR, §E). Lowest risk: no emitted
   code changes, only the map-hash constant + globals relocation + lock reconciliation.
2. **PR-2 — introduce the dispatch hooks on the aarch64 side only**, behaviorally inert:
   refactor `jit/dispatch.c` to call `G_IBTC_FILL`/`G_BLOCK_EXIT`/`G_SHADOW_CLEAR`/
   `G_DISPATCH_DEBUG`, with aarch64 abi.h defining them to exactly today's inline code.
   aarch64 binary is byte-for-byte equivalent; x86 untouched. Gate: aarch64 suite green.
3. **PR-3 — implement the x86 hooks** in `frontend/x86_64/` (move the IBTC-fill, the
   reason switch, and the debug block out of `frontend/x86_64/dispatch.c` into hook
   functions). x86 still uses its own `dispatch.c`. Gate: x86 suite green (pure
   code-motion).
4. **PR-4 — lift `dispatch.c`**: x86 TU swaps `#include frontend/x86_64/dispatch.c` →
   `#include jit/dispatch.c`, keeps its own `run_block`/`block_return` trampolines (in
   `translate.c`), and supplies the four hooks. Delete `frontend/x86_64/dispatch.c`.
   Gate: full cross-engine matrix.
5. **emit stays per-arch** — no PR. Done: one engine (`jit/cache.c` + `jit/dispatch.c`),
   two frontends (decoder+emit+trampolines+abi.h per arch).

Keep both engines green throughout by never changing emitted-code semantics in the same
PR as a file lift: PR-1 and PR-4 are pure plumbing; PR-2/PR-3 are inert code-motion that
*precede* the swap.

---

## E. The EXACT first PR — lift `cache.c`

Smallest reversible step, zero emitted-code change. Touches the x86 TU + two new
seam macros + one new glue file; the aarch64 build is bit-identical.

**Changes:**

1. `frontend/x86_64/abi.h`: add
   ```c
   #define G_GPC_HASH_SHIFT 0    // x86 PCs are byte-granular
   ```
   `frontend/aarch64/abi.h`: add
   ```c
   #define G_GPC_HASH_SHIFT 2    // aarch64 PCs are 4-byte aligned
   ```

2. `jit/cache.c`: replace the two hash sites
   `(uint32_t)((gpc >> 2) * 2654435761u)` → `(uint32_t)((gpc >> G_GPC_HASH_SHIFT) * 2654435761u)`
   in `map_idx` and `map_put`. (aarch64 value 2 unchanged → bit-identical aarch64.)

3. `include/cpu_x86_64.h`: add `#define OFF_PC OFF_RIP` (alias; the shared cache.c
   doesn't bake PC offsets but dispatch.c will — add now to avoid a second touch).

4. New `frontend/x86_64/engine_glue.c`: define the x86-only engine globals listed in §C
   that currently live in `frontend/x86_64/cache.c` (everything except `g_prof` and
   `g_cpu_key`, which `jit/cache.c` now provides, and `g_trace`/`g_exe_path`, which x86
   `emit.c` still owns this PR).

5. `targets/linux_x86_64.c`: change the include line
   ```c
   #include "../frontend/x86_64/cache.c"   // x86 engine: code cache + block map
   ```
   to
   ```c
   #include "../frontend/x86_64/engine_glue.c"  // x86-only engine globals
   #include "../jit/cache.c"                     // SHARED engine: code cache + block map
   ```
   Order: `engine_glue.c` and `state.c` (which defines `MAP_N`) must precede
   `jit/cache.c`. `cache.c` must still precede `emit.c` (emit references `g_cp`,
   `map_body`, `g_ibtc`, `add_pend`). Keep `state.c` before both, as today.

6. Delete `frontend/x86_64/cache.c` (or leave it unreferenced; deleting is cleaner and
   the revert is a one-line include swap-back + `git revert`).

**Verification (no build here — runner does it):** the diff must show
`map_idx`/`map_put` for aarch64 emit the identical hash (shift==2), so the aarch64
binary is unchanged; only the x86 TU is recompiled against the shared cache. Run the
cross-engine matrix; require the same ~236 green. Spot-check x86: a chained-branch
workload (busybox `sh -c` loop) to exercise `patch_links_to`, and an indirect-heavy one
(qsort / vtable) to exercise IBTC fill via the *unchanged* x86 `dispatch.c` (still keyed
on `c->ic_miss`, still stores plain `body` — proving the cache lift didn't disturb the
IBTC contract).

**Why this is safe:** `jit/cache.c`'s `g_pend`/`patch_links_to` is a strict superset of
x86's (the `is_bl` path is dormant for x86, which only ever `add_pend(slot,target)` with
`is_bl=0`). The map/IBTC structs are identical. The only behavioral knob is the hash
shift, now parameterized. The extra `g_cache_lock` only adds finer FS-metadata locking
under threads.

---

## F. Risks & how both engines stay green

1. **§B shadow stack (aarch64-only).** `jit/cache.c`/`dispatch.c` reference `c->ssp`
   (flush reset) and `g_pend.is_bl` (host-`bl` chaining for the return-address-stack).
   The x86 cpu struct has no `ssp` and x86 never sets `is_bl`. Mitigation: the flush
   reset is behind `G_SHADOW_CLEAR(c)` (no-op on x86, never touches a missing field);
   `is_bl` is plain data in `g_pend`, always 0 for x86, and `patch_links_to`'s
   `is_bl?bl:b` always selects `b`. Verified: no x86 code path sets `is_bl`.

2. **IBTC contract divergence (`ic_site` vs `ic_miss`, `body` vs `body−8`, per-site IC).**
   This is the single most delicate difference and the reason emit stays per-arch and
   the IBTC fill becomes a frontend hook (`G_IBTC_FILL`). PR-1 does **not** touch it
   (x86 keeps its own dispatch.c/emit.c). It is only confronted at PR-3/PR-4, where the
   x86 hook reproduces *exactly* today's `frontend/x86_64/dispatch.c` fill (shared-hash
   only, plain `body`, keyed on `ic_miss`). Gate each with an indirect-branch-heavy
   workload and `PROF` counters (IBTC fills) before/after — counts must match.

3. **Block chaining differences.** x86 `emit_chain_exit` disables chaining under
   `g_trace||g_nochain||g_threaded`; aarch64's `emit_chain_exit` does not gate on those
   (it relies on `patch_links_to` being skipped when threaded in `translate.c:1862`).
   Both end up not chaining live blocks under threads, by different mechanisms. Since
   **emit stays per-arch**, each keeps its own `emit_chain_exit` — the shared
   `patch_links_to`/`add_pend` in `cache.c` serve both. No conflict. Confirm the x86
   `g_threaded` chaining-off behavior is preserved (it is: x86 `emit.c` is untouched).

4. **Trampoline register model (x0 vs x28 at `block_return`).** Irreducible; trampolines
   stay per-arch (§B.1/B.3). The shared `dispatch.c` only *calls* `run_block`/
   `block_return` (forward-declared) — never embeds their offsets. Ensure PR-4 keeps the
   x86 trampolines in `frontend/x86_64/translate.c` and forward-declares them for the
   shared loop.

5. **syscall `+4` pc-advance convention.** aarch64 services with pc at the SVC and
   advances `+4`; x86 pre-advances rip past `syscall` in the emitter, then the dispatcher
   also does `pc += 4` only on the non-redirect path. Re-confirm at PR-4 that the shared
   loop's advance matches x86's expectation (the existing x86 dispatch.c does
   `if(c->redirect)c->redirect=0; /* else rip already=next */` — i.e. x86 does **not**
   `+4` post-service, it set rip at exit). **This is a real divergence in the
   post-syscall tail**: aarch64 does `c->pc += 4`, x86 relies on rip already being next.
   → fold into `G_BLOCK_EXIT`/the syscall tail hook so each arch advances per its own
   convention. Flag this explicitly in PR-4; it is the most likely correctness trap.

6. **Globals double-definition in the unity TU.** The chronic failure mode of a
   unity-include lift. Mitigation: the §C ownership table; after each lift, grep the TU
   for each engine global to assert exactly one definition. (`g_trace`, `g_exe_path`,
   `g_prof`, `g_cpu_key` are the collision-prone four.)

7. **Both TUs recompile on any shared-file edit** (`build.rs:rerun_dir` watches all
   `.c`/`.h` under `src/runtime`). So a shared `cache.c`/`dispatch.c` regression breaks
   *both* targets at once — which is exactly why the cross-engine matrix is the gate and
   why PR-2 keeps the aarch64 binary bit-identical (refactor-to-inert before any swap).

---

## G. End state

```
jit/cache.c        SHARED  (map + IBTC + chaining; hash via G_GPC_HASH_SHIFT)
jit/dispatch.c     SHARED  (run_guest loop; G_PC + 4 frontend hooks)
jit/emit_arm64.c   aarch64 frontend  (host emit + block ABI for the 31-GPR model)
frontend/aarch64/  decoder/translate + trampolines + abi.h (hooks: inert/empty)
frontend/x86_64/emit.c        x86 frontend (host emit + flags/SSE/x87 + 16-GPR model)
frontend/x86_64/translate.c   x86 decoder/translate + run_block/block_return trampolines
frontend/x86_64/abi.h         G_* contract + the §A.3 engine seam (hooks: real)
frontend/x86_64/engine_glue.c x86-only engine globals
```

One engine, two thin frontends. The cpu struct, decoder, host emitters, and x86 flag
emulation remain genuinely per-arch — by design, not by duplication.
