# Tier-2 Trace Optimizer — Engineering Design + First-PR Roadmap

Subsystem #4 in `docs/PLAN.md`. This document is the executable design for the tier-2
trace optimizer that sits **on top of** the existing tier-1 block engine. It cites real
files/functions and ends with a phased roadmap whose first PR is a small, flag-gated,
matrix-green slice.

> **Hard constraints (non-negotiable, repeated throughout):**
> 1. **Never use the "dead-register §B scratch."** Treating `x9`–`x17` as dead across a
>    call is *unsafe* (crt/ld.so/`memcpy`/`setjmp` keep live values there — a trivial
>    static-PIE segfaults; see `docs/OPTIMIZATIONS.md` "x18/x28/x30 register stealing").
>    The trace allocator may only own `x9`–`x17` inside a span it has proven call-free.
> 2. **Never drop the §B `gsp` check.** The shadow-return fast path validates the guest
>    return address **and** `gsp[idx] == current guest SP`
>    (`translate.c` `emit_shadow_ret`, the `sub x1,x1,x2; cbnz x1,Lfb` on the
>    `OFF_GSP` load). Tier-2 must leave that comparison reachable and unchanged for every
>    return it does not fully own.

---

## 1. Where tier-1 stands, and what actually costs

### 1.1 The block engine (recap of the real code)

- **Entry/exit trampolines** — `jit/dispatch.c` `run_block()` / `block_return()`.
  `run_block` saves host callee-saved state into `cpu->host_save`/`host_v`/`host_sp` and
  `br`s to a block's host entry; the block tail-calls `block_return` to restore host
  state and return to the dispatcher loop `run_guest()`.
- **Dispatcher** — `jit/dispatch.c` `run_guest()`. Per crossing: `map_host(c->pc)`;
  translate on miss (`translate_block`); fill IBTC on `c->ic_site`; `run_block`; on
  `R_SYSCALL` service + advance `pc`.
- **Code cache + chaining** — `jit/cache.c`. One 64 MB `MAP_JIT` arena, a guest-PC→host
  map (`g_map`, holding both `host` and `body`), a pending-link table (`g_pend` /
  `patch_links_to`), and the shared **IBTC** hash (`g_ibtc`).
- **Per-block codegen** — `jit/emit_arm64.c`:
  - `emit_prologue()` — entered with `x0=cpu`. Reloads guest `sp`, `nzcv`, **all 32 V
    regs** (paired `ldp_q`), **all GPRs `x1..x30`** except stolen `x18`/`x28`, then
    `mov x28,x0` (cpu) and `x0` last. The `body` label is emitted **after** the prologue.
  - `emit_spill()` — the inverse: store every guest GPR/V/sp/nzcv back to the `cpu`
    struct. Invoked by `emit_exit_const/_reg`, `emit_ibtc_miss`, the ret-IC miss
    (`emit_ret_ic`), etc.
- **Translator** — `frontend/aarch64/translate.c` `translate_block()`. Streams host
  instructions in place (no IR). Same-ISA: most guest instructions are copied verbatim;
  only **stolen-register** users are mangled (`emit_mangled_x18`). Block enders:
  `emit_chain_exit` (direct `b`), `emit_bl_ras`/`emit_shadow_ret` (§B), `emit_ibranch`
  (IBTC), `emit_exit_const` (syscall).

### 1.2 The chaining invariant (why "per-block spill" is subtle)

A block has **two** entry points (`emit_arm64.c` / `translate.c`):

```
host = g_cp; emit_prologue();   // full reload of all guest regs from cpu
body = g_cp;                    // <-- chained edges land HERE, skipping the reload
... block code ...
map_put(start, host, body);
```

`emit_chain_exit(target)` emits `b target.body` when the target is already translated, so
**a direct-chained edge does not spill or reload** — guest registers stay live in their
host homes across the edge. The standing **ABI between cached blocks** is therefore a
**fixed identity mapping**: guest `xN` lives in host `xN` at every block boundary (except
the stolen trio). The prologue's full reload only runs on a *fresh* dispatcher entry into
a block; the spill only runs on a real exit (dispatcher round-trip, syscall, IBTC miss).

So the honest accounting of what tier-1 still pays on hot paths is **not** "spill at every
block boundary" — chaining already removed that. The residual costs a trace removes are:

1. **Stolen-register mangle, per instruction.** Guest `x18`/`x28`/`x30` can never live in
   a host register (they're stolen for macOS-x18 / cpu-ptr / §B-host-link). *Every* guest
   instruction that names one is rewritten by `emit_mangled_x18()` into: spill 2 scratch
   regs to `cpu->mscratch`, load the guest value from `cpu->x[stolen]`, run, store back,
   restore scratch (≈5 extra memory ops/instr). Guest `x30` (the link register) and `x28`
   (AAPCS callee-saved, common frame/base) are *frequent*, so this is the dominant
   residual overhead in real loops. **This is the win the prompt calls "the per-block
   spill": the identity-mapping ABI forces these guest values to memory.**
2. **The full `emit_prologue` reload** on every *non-chained* re-entry — i.e. indirect
   control flow (vtables, interpreters, `ret` to a not-yet-IC'd site) and the first entry
   into each block.
3. **Side-exit spills** that, with traces, can be hoisted off the hot path: a trace turns
   N blocks into one superblock so the only spills are at cold guards.

A trace breaks the identity-mapping ABI **locally**: inside a trace, the allocator may
place guest `x18`/`x28`/`x30` (and everything else) in whatever host registers it owns,
killing the mangle (#1) and the re-entry reloads (#2), and pushing spills to cold guards
(#3). It must re-establish the canonical state at every side exit so tier-1 is unaffected.

### 1.3 Substrate that already exists (do not rebuild)

- **`region_pure(code, n)`** — `translate.c`. Returns 1 iff a region has no `svc` and no
  load/store ⇒ output is a pure function of input registers. This is the purity gate.
- **§B shadow stack** — `shadow_push` / `shadow_classify` (translate.c, the C model) and
  the emitted `emit_shadow_push` / `emit_shadow_ret` (with the `OFF_GSP` store + compare).
- **Per-site monomorphic IC** — the inline `Lsite_tgt`/`Lsite_body` literals filled by the
  dispatcher (`dispatch.c`, the `c->ic_site` block). This is exactly the signal that tells
  us an indirect call site is **monomorphic** — the input to comparator inlining (§5.4).
- **PROF counters** — `g_prof_cross/_miss/_xlate/_sys`, `g_lse_n` (cache.c), reported on
  `exit_group` in `os/linux/service.c`. These count *crossings*, not block heat; tier-2
  adds heat counting (§5.1).
- **`TIER2_SELFTEST`** — `targets/linux_aarch64.c` already exercises `region_pure` and the
  shadow model. Tier-2 extends this with regalloc/trace self-tests (§7, Phase 0).
- **Unity build** — `targets/linux_aarch64.c` `#include`s the engine `.c` files in order:
  `cache.c` → `emit_arm64.c` → `translate.c` → … → `dispatch.c`. A new `jit/trace.c` is
  included **after `translate.c` and before `dispatch.c`** (it uses the emitters,
  `region_pure`, and the map; the dispatcher calls into it).

---

## 2. Architecture overview

Tier-2 is a **trace JIT layered over tier-1**, sharing the same code cache, map, chaining,
IBTC, and §B machinery. Nothing in tier-1 changes semantically; tier-2 only *adds* a hotter
code path that the dispatcher prefers when present.

```
            ┌─────────────────────── run_guest() dispatcher ───────────────────────┐
            │  map_host(pc) ──hit──> run_block(host or trace head)                   │
            │       │miss                                                            │
            │  translate_block (tier-1)                                              │
            │       │                                                                │
            │  heat[pc]++ at loop-back edges ── trips T ──> record + form trace ─────┼──> trace.c
            └───────────────────────────────────────────────────────────────────────┘
                                                  │
   tier-1 blocks  <── side exits (restore canonical cpu state) ──  TRACE superblock
   (chained / IBTC / §B unchanged)                                 (cross-trace regalloc)
```

Key properties:

- **One arena, two tiers.** Traces are emitted into the same `g_cache`. A trace registers
  its head guest-PC in `g_map` (or a parallel `g_trace_map`) so the dispatcher and chained
  edges prefer it over the tier-1 block.
- **Superblock, single entry, many side-exits.** A trace is straight-line host code with
  *guards*: each in-trace conditional branch falls through on its hot (recorded) direction
  and side-exits on the cold direction. Side exits restore the tier-1 ABI.
- **In-place emission, two passes, small trace buffer.** No heavyweight SSA IR. Pass 1
  decodes the guest trace into a bounded array + computes liveness and the register
  allocation (linear scan). Pass 2 emits host code using the allocation, reusing the
  existing `e_*`/`emit_mangled_x18`-style emitters but with a **trace-wide** guest→host map
  instead of the identity map.
- **Conservative fallback everywhere.** Any condition the trace cannot prove (purity,
  monomorphism, frame identity, call-free span) ⇒ side-exit to tier-1, which is always
  correct. A trace can never *mis*execute; worst case it bails to the block engine.

---

## 3. The trace buffer (the "IR")

A trace is recorded into a fixed-capacity buffer; there is no separate optimizer IR — the
guest instruction words plus per-slot metadata are the IR.

```c
// jit/trace.c
#define TRACE_MAX 256            // guest insns per trace (bounded; bail if exceeded)
struct tinsn {
    uint64_t  gpc;               // guest PC of this insn
    uint32_t  in;                // raw guest instruction word
    uint8_t   kind;             // TI_PLAIN / TI_GUARD / TI_CALL / TI_RET / TI_SVC / TI_IND
    uint8_t   cond;             // for TI_GUARD: the arm64 condition / cbz-tbz encoding
    uint64_t  off_target;        // for TI_GUARD/TI_CALL: the OFF-trace (cold) guest target
};
struct trace {
    uint64_t  head;              // entry guest PC
    int       n;                 // number of tinsn
    struct tinsn ti[TRACE_MAX];
    // filled by analysis:
    uint32_t  defmask, usemask;  // guest-reg def/use bitmaps (x0..x30 = bits 0..30, sp=31)
    int8_t    g2h[32];           // guest reg -> allocated host reg (-1 = stays in cpu home)
    uint8_t   liveout[TRACE_MAX];// per-slot live-out bitmap index (for spill scheduling)
    int       pure;              // region_pure() over the straight-line span
};
```

`defmask`/`usemask`/live ranges come from a single linear pass that reuses the field
decoder already in `translate.c`: `gpr_field_mask(in)` tells which of the 4 register
fields are GP registers, and the field shifts `{0,5,16,10}` extract the guest regs. This is
the **same** decode tier-1 already trusts; tier-2 does not add a second, divergent decoder.

---

## 4. Trace selection (over PROF)

### 4.1 What PROF gives us today, and the gap

`g_prof_*` count *dispatcher crossings*, not block execution frequency. A hot loop that is
fully chained executes millions of times with **zero** crossings — invisible to PROF. So
tier-2 adds **heat counting at loop-back edges only** (the classic DynamoRIO/NET trace-head
heuristic): backward branches are the cheapest, highest-signal trace heads, and most hot
code is loops.

### 4.2 Loop-back heat counters (zero cost off-loop)

A backward edge is detected at emission time: in `emit_chain_exit(target)` (and the
conditional-branch enders), a target `<= start` of the current block is a **loop-back
candidate**. Today such an edge becomes a plain `b target.body`. Under `TIER2`, it instead
routes through a **counter stub** in the cache:

```
;; loop-back edge counter stub (emitted once per backward edge; ~4 insns)
ldr   w9, [x28, #HEAT_off(head)]      ; heat is a small table keyed by head gpc
add   w9, w9, #1
str   w9, [x28, #HEAT_off(head)]
cmp   w9, #T                          ; T = TIER2_HOT threshold (env-tunable)
b.lo  target.body                     ; cold: behave exactly like tier-1
;; hot: fall into a spill-exit with reason=R_TRACE_HEAD, pc=head
```

Cost on the hot loop: 4 ALU/mem ops per iteration *until promotion*, then the trace
replaces the edge entirely (the stub is patched to jump to the trace head). Off-loop code
emits **no** counter — zero overhead, honoring "remove the JIT's own overhead."

> **First-PR simplification:** the counter table can be a flat open-addressed
> `g_heat[gpc]` array (mirroring `g_ibtc`'s direct-mapped style), not a new `cpu` field, to
> avoid touching the baked `cpu` offsets. Heat is process-global and approximate — exactly
> like the IBTC, races are benign (a missed/double count only mis-times promotion).

### 4.3 Promotion

When the stub trips `T`, it spill-exits with a new reason `R_TRACE_HEAD`. The dispatcher
(`run_guest`) sees `c->reason == R_TRACE_HEAD`, calls `trace_form(c->pc)`, and on success
patches the loop-back edge (via the existing `patch_links_to`/`g_pend` mechanism, or a
direct backpatch of the stub's terminal `b`) to jump to the trace head. On failure
(unsupported insn, exceeded `TRACE_MAX`, impure where purity required, etc.) it **disables**
that head (heat saturates to a sentinel) so we never thrash re-forming, and falls back to
tier-1 forever for that head.

---

## 5. Trace formation, register allocation, specialization

### 5.1 Recording the linear path

`trace_form(head)` records straight-line guest insns starting at `head`, following the
**recorded-hot direction** at each conditional, until a stop condition:

- **Loop close:** the next guest PC `== head` ⇒ a cyclic trace (the common case). Stop;
  the trace's tail branches back to its own entry (after the parallel-move epilog, §5.5).
- **Call / return / syscall:** `bl`/`blr`/`ret`/`svc`. **In the first phases, stop the
  trace here and side-exit to tier-1**, so §B (`emit_bl_ras`/`emit_shadow_ret` incl. the
  `gsp` check) and syscall servicing run *unchanged* in tier-1. (Comparator inlining in
  §5.4 is the only controlled exception, and it preserves §B.)
- **Indirect branch** not handled by inlining ⇒ stop, side-exit (tier-1 IBTC).
- **`TRACE_MAX` reached** ⇒ stop, side-exit to the fall-through PC.

Which conditional direction is "hot"? First cut: **trust the structure** — for a loop-back
edge head, the hot direction is the one that stays inside the loop (target back toward
`head`); the exit direction becomes the guard's cold side. Later (Phase 3): a 1-bit
per-edge bias recorded by a second tiny counter, or simply the direction taken during the
recording run (record-what-you-execute, like a tracing JIT).

Recording **does not execute** guest code; it decodes statically from guest memory (the
guest pages are mapped and readable — `translate.c` already reads ahead, e.g.
`try_lse_atomic`, `scan_calls`). This keeps formation a pure analysis with no interpreter.

### 5.2 Liveness + the host-register pool

One backward pass over `ti[]` computes each guest register's live interval `[firstdef-or-use
.. lastuse]` within the trace, plus the trace-level `defmask`/`usemask` (for the entry
shuffle and side-exit restores).

**Host register pool (the §B-safe set):**

| host reg | status | usable by trace allocator? |
|---|---|---|
| `x28` | **cpu pointer (CPUREG)** | never |
| `x30` | **§B host link** | never |
| `x18` | macOS asynchronously zeroes it | **never** |
| `x16`,`x17` | §B / IBTC scratch | only if the trace has **no** indirect-branch/IBTC emission |
| `x0`–`x15`, `x19`–`x27`, `x29` | general | **yes — but only across a call-free span** |

The pool is `{x0..x17, x19..x27, x29} \ {reserved}` — up to ~27 GPRs, comfortably more than
the ~28 guest GPRs, and far more than the *live* set of a typical hot loop body (a handful).

> **§B-safety of using `x9`–`x17` (the load-bearing argument).** The forbidden pattern is
> assuming `x9`–`x17` are dead *across a call*. The trace allocator owns them only within a
> span it has **proven call-free** — and the trace is, by construction, terminated at every
> `bl`/`blr`/`ret`/`svc` (§5.1). Therefore the entire trace body is one call-free span, and
> `x9`–`x17` are genuinely the trace's to use; no value of ld.so/`memcpy`/`setjmp` can be
> live in them across an instruction we emitted. We **never** rely on the dead-register
> assumption the §B docs reject. (When comparator inlining splices a callee, §5.4 re-checks
> this: the inlined region must itself be call-free and not use `x16/x17` outside our
> control, or we don't inline.)

### 5.3 Linear-scan allocation, biased to identity

Standard linear scan over intervals sorted by start point:

1. **Bias to identity.** When assigning guest `xK`, prefer host `xK` if free. This makes
   the entry shuffle and the side-exit restores nearly empty (most guests already sit in
   their identity homes when the trace is entered from a chained tier-1 edge).
2. The headline cases — guest `x18`/`x28`/`x30` — **cannot** take their identity home
   (stolen), so they get the first free *non-identity* host reg from the pool. **This is
   the mangle-elimination win:** inside the trace, a guest `x30`/`x28`/`x18` user is a plain
   verbatim instruction with the field rewritten to the allocated host reg — no
   `cpu->mscratch` spill, no `cpu->x[]` reload.
3. **Spill on pressure.** If the pool is exhausted, spill the interval whose next use is
   furthest, to its `cpu->x[]` home (a real `str`/`ldr`). Rare in hot loop bodies; correct
   always.
4. **`sp`** is special: keep guest SP in the host SP (as tier-1 does) unless the trace
   proves SP-invariance; first cut keeps SP in host `sp` and treats it as fixed.
5. **NZCV / V-registers.** First phases keep V regs and `nzcv` in the tier-1 identity
   convention (the prologue/spill handle them); the GPR mangle is the measurable win and
   the safe place to start. V-register cross-trace allocation is a later phase.

The result is `trace.g2h[0..31]`: guest→host for this trace, with `-1` meaning "stays in
its `cpu` home, accessed by explicit ld/st" (spilled or untouched).

### 5.4 Specialization #1 — monomorphic-comparator inlining

**What it means here.** Hot indirect *calls* — a `qsort` comparator reached via `blr x16`,
or a C++ vtable call — are translated by `emit_ibranch` with a **per-site monomorphic IC**
(`Lsite_tgt`/`Lsite_body`). When that IC has been filled and is stable, PROF/the IC tells
us the site is **monomorphic**: one guest target, every time. (This is the "sort/awk fix"
referenced in `OPTIMIZATIONS.md` §Tier-2.)

**The optimization.** When a trace would stop at such a `blr`, instead of side-exiting,
**inline the callee's body into the trace**, guarded:

```
;; guard: actual target must equal the inlined target (else bail to tier-1 IBTC)
<materialize actual target in a pool reg>          ; from guest reg / IC literal
cmp   <reg>, #inlined_target_gpc                   ; (movconst + sub; no flag clobber issues)
b.ne  Lsidexit_ibtc                                ; cold: real indirect call via tier-1
;; hot: the callee's instructions, decoded + allocated as part of THIS trace
```

Effects:

- The IBTC compare+branch and the call/return round-trip vanish on the hot path.
- The comparator body is exposed to the **trace register allocator** — its args
  (guest `x0`/`x1`) are already in host regs, no spill/reload across the call boundary.
- **§B stays intact.** Two cases: (a) if we inline a *leaf* callee (no `bl`/`blr` inside —
  checked with the existing `is_leaf0`/`scan_calls` from `translate.c`), there is no return
  prediction to preserve; we splice the leaf's body and continue. (b) If the callee is
  non-leaf, we **do not** invent a return — we keep the trace's `bl` going through
  `emit_bl_ras` (§B shadow push, including the `gsp` store) and end the trace at the call,
  or only inline up to the callee's own first call. In neither case is the `gsp` check
  dropped: a real `ret` always reaches `emit_shadow_ret`'s `gsp` compare.
- **Conservative gate:** inline only when (i) the per-site IC is filled and stable, (ii) the
  callee region is call-free and `x16/x17`-clean (so §5.2's pool argument holds), and
  (iii) the callee fits the remaining `TRACE_MAX`. Otherwise side-exit. A guard miss is
  always safe (falls to the tier-1 indirect call).

### 5.5 Specialization #2 — purity-gate memoization

**What it means here.** `region_pure(code, n)` already proves a straight-line region is
side-effect-free (no `svc`, no load/store) ⇒ its output registers are a pure function of its
input registers. For a hot, *expensive*, pure sub-region with a small recurring input
domain, **memoize**: cache the last input-register tuple → output-register tuple and skip
recomputation on a hit.

```
;; monomorphic (1-entry) memo, guarded — only when region_pure()==1 over the sub-region
<compare live-in regs of the region to cached inputs>   ; pool regs, no mem side effects
b.ne  Lrecompute                                         ; miss -> run the region, update memo
<load cached outputs into their allocated host regs>     ; hit -> skip the region
b     Lafter
Lrecompute: <the region>; <store inputs+outputs to memo>
Lafter:
```

This is the most speculative brick and ships **last**, hard-gated:

- Applied **only** where `region_pure()==1` (a wrong gate is a miscompile — the gate
  refuses any load/store/`svc`).
- 1-entry (monomorphic) memo: pays off only when the region is costly relative to the
  guard and the inputs recur; otherwise the guard is pure overhead, so gate on region
  length / estimated cost and keep a runtime hit-rate check that *disables* the memo if it
  isn't hitting (auto-adapt, like the §B depth gate).
- The memo store is in plain engine data (not the cache), no `cpu`-offset churn.

### 5.6 Side exits — restoring the canonical state (and §B)

Every guard, every trace stop, every inlining-guard miss is a **side exit**. A side exit
must hand tier-1 a `cpu`/register state indistinguishable from what a tier-1 block would
have produced at that guest PC. Two flavors:

- **Dispatcher side-exit (always-correct, used first).** Emit the equivalent of
  `emit_spill()` *for the trace's allocation*: for each guest reg with `g2h[r] >= 0`, store
  the allocated host reg to `cpu->x[r]`; store `sp`, `nzcv`, V regs per the identity
  convention; set `cpu->pc = off_target`, `cpu->reason = R_BRANCH`; `b block_return`. The
  dispatcher then resolves `off_target` normally (translate/chain/IBTC). This is just
  `emit_spill` generalized from the identity map to `g2h`.
- **Chained side-exit (optimization, Phase 3).** If `off_target` is already a tier-1 block,
  restore the **identity** mapping in host registers (a parallel move `g2h → identity` for
  live-out guests) and `b off_target.body`. No `cpu` spill. Because the allocator is
  identity-biased, this parallel move is usually a handful of `mov`s.

**§B at side exits.** A side exit never performs a guest `ret` or `bl` itself — it restores
state and hands control to tier-1, which owns §B. The `gsp` check therefore remains the
*only* path by which a §B fast-return is taken, and tier-2 never bypasses it. If a future
phase inlines a §B `bl`+`ret` pair wholesale into a trace, it must splice the **exact**
`emit_shadow_push` (with the `OFF_GSP` store) and `emit_shadow_ret` (with the `sub; cbnz`
`gsp` compare) sequences — dropping the `gsp` compare is forbidden by constraint #2.

---

## 6. Interaction with chaining, IBTC, and the map

- **Map preference.** A formed trace registers its head in a parallel `g_trace_map[head]`
  (or sets a high bit / separate body pointer in `g_map`). The dispatcher and
  `emit_chain_exit` consult the trace map first: a chained edge whose target is a trace head
  jumps to the **trace** body, not the tier-1 block. Backpatch via the existing `g_pend` /
  `patch_links_to` plumbing (cache.c) — no new patch mechanism.
- **Cache flush.** On the wholesale cache flush (`dispatch.c`, `g_cp` reset path), tier-2
  state is dropped alongside tier-1: clear `g_trace_map`, `g_heat`, and any memo tables next
  to the existing `memset(g_map,…)` / `memset(g_ibtc,…)` / `c->ssp = 0`. Traces hold no
  state that outlives the arena.
- **IBTC untouched.** Indirect branches the trace doesn't inline fall to the existing IBTC
  (shared hash + per-site IC). Tier-2 *reads* the per-site IC (to detect monomorphism) but
  does not change its protocol.
- **Threads.** Trace formation runs under the same `g_jit_lock` as `translate_block`
  (`dispatch.c` already takes it around translation + chaining). Heat counters are benign
  races (like `g_ibtc`). First-PR keeps formation single-threaded-safe by forming only
  inside the locked region; under `g_threaded`, tier-2 can be disabled (like the IBTC
  per-site fill is) until the lock-free story is designed — correctness first.

---

## 7. The §B-safety + correctness argument, consolidated

1. **No dead-register scratch.** The allocator's pool is `{x0..x17,x19..x27,x29}` minus
   reserved, and it owns `x9`–`x17` **only within a proven call-free span** (traces
   terminate at `bl`/`blr`/`ret`/`svc`; inlined callees are re-proven call-free). We never
   assume `x9`–`x17` are dead across a call — the exact pattern `OPTIMIZATIONS.md` forbids.
   `x18` is never allocated; `x28`/`x30` are never allocated.
2. **`gsp` always checked.** Tier-2 emits no guest `ret`/`bl` of its own in the early
   phases; every return reaches tier-1's `emit_shadow_ret`, whose `gsp[idx] == guest SP`
   compare is intact. Any later in-trace call/return splices the §B sequences verbatim,
   `gsp` store + compare included. Dropping the compare is a hard-blocked constraint.
3. **Conservative gates compose.** Purity (`region_pure`), monomorphism (per-site IC
   stable), frame identity (`gsp`), and call-free spans are each checked before the
   corresponding optimization; any failure ⇒ side-exit to tier-1, which is always correct.
   A trace can never land at the wrong target — at worst it bails to the block engine.
4. **State coherence.** Every side exit reproduces the canonical `cpu`/identity-register
   state (generalized `emit_spill`), so tier-1, signals, `fork`, and the dispatcher observe
   exactly the state they would without tier-2.

**Self-tests (extend `TIER2_SELFTEST`).** Add: (a) a liveness/linear-scan unit test on a
synthetic trace asserting `g2h` correctness and identity bias; (b) a side-exit
state-equivalence test (form a trace, run it and the tier-1 path on the same inputs, assert
identical `cpu`); (c) a guard-miss test (inlining guard mismatch falls back correctly).
These run with no guest, like the existing purity/shadow checks in `targets/linux_aarch64.c`.

---

## 8. The measurable win

- **Primary:** elimination of the stolen-register mangle (`emit_mangled_x18`) for guest
  `x18`/`x28`/`x30` **inside hot traces** — ≈5 memory ops removed per such guest
  instruction. Loops that touch the link register or callee-saved `x28` (string/recursion/
  sort kernels) are the headline beneficiaries. Measure with `PROF` + a new
  `g_prof_mangle` counter (bump in `emit_mangled_x18`) before/after, plus wall-clock on the
  existing `floatk`/`stringk`/`qsortk`/`recursion` microbenchmarks already tabulated in
  `OPTIMIZATIONS.md`.
- **Secondary:** removal of `emit_prologue` reloads on non-chained re-entry into hot heads;
  spills hoisted from the hot path to cold guards.
- **Specialization:** comparator inlining removes the IBTC compare+branch and call/return on
  monomorphic sort/awk dispatch; purity memoization skips recomputation on the narrow class
  of hot pure regions.
- **Honest ceiling** (`OPTIMIZATIONS.md`): compute-bound code tops out *near* native; the
  wide OoO core hides micro-scheduling, so the win is concentrated in the structural
  overheads above, not instruction-level cleverness.

---

## 9. Phased roadmap

Each phase is independently shippable, flag-gated by `TIER2` (env), and must keep the
matrix green (`~236` cross-engine cases) with `TIER2` **off by default**.

- **Phase 0 — substrate (mostly exists).** `region_pure` ✓, shadow model ✓. Add: `jit/trace.c`
  skeleton (the `struct trace`/`tinsn`, `region_pure` already available), `g_heat`/
  `g_trace_map` tables, `R_TRACE_HEAD` reason, the `TIER2`/`TIER2_HOT` env flags, and the
  `TIER2_SELFTEST` extensions (liveness/regalloc unit tests, no guest). No codegen path
  enabled yet.
- **Phase 1 — FIRST PR (see §10).** 2-block linear trace, **identity** register mapping,
  single dispatcher side-exit, `TIER2`-gated, matrix-green.
- **Phase 2 — cross-trace regalloc (the win).** Linear-scan allocation over N-block traces,
  identity-biased, dispatcher side-exits, **mangle elimination** for guest `x18/x28/x30`.
  Add `g_prof_mangle` and measure on the `OPTIMIZATIONS.md` microbenchmarks.
- **Phase 3 — chained side-exits + loop closure + NET selection.** Parallel-move restores +
  `b target.body`; cyclic traces that branch back to their own entry; per-edge hot-direction
  bias.
- **Phase 4 — monomorphic-comparator inlining.** Guarded callee splice using the per-site
  IC signal + `is_leaf0`/`scan_calls`; §B preserved.
- **Phase 5 — purity-gate memoization.** 1-entry monomorphic memo behind `region_pure`,
  auto-disabling on low hit-rate. Ship last.

---

## 10. The first PR (smallest correct slice)

**Goal:** prove the trace path end-to-end with the least surface area, fully fenced behind a
flag, matrix-green. **No regalloc, no inlining, no memoization** — those are Phase 2+.

**Scope:**

1. **New file `dd-jit/src/runtime/jit/trace.c`**, `#include`d in
   `targets/linux_aarch64.c` between the `translate.c` (line ~48) and `dispatch.c`
   (line ~63) includes. It defines `struct trace`/`tinsn`, the `g_heat`/`g_trace_map`
   tables, and `trace_form(uint64_t head)`.
2. **Selection:** add the loop-back heat counter stub in `emit_chain_exit` (emit_arm64.c)
   **only when `getenv("TIER2")`** — otherwise emit today's plain `b target.body`
   unchanged. Threshold `T` from `TIER2_HOT` (default e.g. 50). On trip, spill-exit with
   `reason = R_TRACE_HEAD`.
3. **Formation (degenerate):** `trace_form(head)` records exactly **two** straight-line
   tier-1 *bodies*: the head block and its single hot successor, joined at the head block's
   terminating direct/conditional branch. Stop immediately at any `bl`/`blr`/`ret`/`svc`/
   indirect branch (side-exit to tier-1). **Use the identity register mapping** — i.e. emit
   the two blocks' bodies back-to-back with the existing per-instruction emitters (verbatim
   + `emit_mangled_x18` exactly as tier-1), **no prologue between them** (registers already
   live), and a **single side-exit** at the join's cold direction:
   - The merge eliminates one `emit_prologue` reload and the intermediate chain edge — a
     real, if modest, win, and it exercises the whole pipeline.
4. **Side-exit:** one **dispatcher** side-exit (generalized `emit_spill` with the identity
   map — i.e. literally the existing `emit_spill` since Phase 1 is identity-mapped) at the
   cold edge of the joined conditional; set `cpu->pc`, `reason=R_BRANCH`, `b block_return`.
5. **Dispatcher hook:** in `run_guest` (`dispatch.c`), handle `R_TRACE_HEAD` by calling
   `trace_form`; on success register the trace head in `g_trace_map` and backpatch the
   loop-back stub to the trace; on failure saturate `g_heat[head]` to disable.
6. **Map preference:** `emit_chain_exit` and the dispatcher's `map_host` lookup consult
   `g_trace_map` first **only under `TIER2`**.
7. **Flush:** clear `g_trace_map`/`g_heat` next to the existing `memset(g_map,…)` in the
   cache-full path (`dispatch.c`).
8. **Self-test:** extend `TIER2_SELFTEST` (`targets/linux_aarch64.c`) to form a synthetic
   2-block trace over a hand-built guest snippet and assert the side-exit reproduces the
   same `cpu` as running the two tier-1 blocks.

**Explicitly out of scope for PR 1:** linear-scan regalloc, mangle elimination,
comparator inlining, purity memoization, chained side-exits, loop-closing cyclic traces,
threaded formation (disable tier-2 when `g_threaded`).

**Acceptance:**
- `TIER2` unset ⇒ byte-identical tier-1 behavior; full matrix green (`~236`).
- `TIER2=1` ⇒ matrix still green; `TIER2_SELFTEST` passes the new 2-block equivalence check;
  `PROF` shows fewer crossings / `g_prof_xlate` on a loop microbenchmark.
- Constraints audited in review: pool excludes `x18/x28/x30`; no `x9`–`x17` dead-scratch
  assumption (PR 1 is identity-mapped, so trivially satisfied); every return still reaches
  `emit_shadow_ret`'s `gsp` check (PR 1 stops traces before any call/return).

---

## 11. File/function index (where each piece lands)

| Concern | File / function (existing or new) |
|---|---|
| Dispatcher hook (`R_TRACE_HEAD`, trace map lookup) | `jit/dispatch.c` `run_guest()` |
| Heat counter stub, trace-map-preferring chain | `jit/emit_arm64.c` `emit_chain_exit()` (+ cond enders) |
| Generalized spill at side exit | `jit/emit_arm64.c` `emit_spill()` (reused; later parameterized by `g2h`) |
| Trace buffer, formation, regalloc, specialization | **new** `jit/trace.c` |
| Heat / trace map / memo tables + flush | **new** in `jit/cache.c` neighborhood (`g_heat`, `g_trace_map`) |
| Field decode reused by liveness | `frontend/aarch64/translate.c` `gpr_field_mask()` |
| Purity gate | `frontend/aarch64/translate.c` `region_pure()` |
| Monomorphism signal | per-site IC literals filled in `jit/dispatch.c` (`c->ic_site` block) |
| Leaf/call scan for inlining | `frontend/aarch64/translate.c` `scan_calls()` / `is_leaf0()` |
| §B push/ret (must stay intact) | `frontend/aarch64/translate.c` `emit_shadow_push()` / `emit_shadow_ret()` (the `OFF_GSP` store + `sub;cbnz` compare) |
| Self-tests | `targets/linux_aarch64.c` `TIER2_SELFTEST` block |
| Flags | `TIER2`, `TIER2_HOT` (env, parsed near `g_prof`/`g_trace` in `targets/linux_aarch64.c`) |
| PROF accounting | `jit/cache.c` `g_prof_*` + new `g_prof_mangle`; report in `os/linux/service.c` exit_group |
