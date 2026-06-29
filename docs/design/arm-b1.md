# ARM-B1 / B2 — meta-tracing the SQLite VDBE on the aarch64 same-ISA engine

**Lever:** Tier-B "beat-native" from `docs/design/arm-sqlite-parity.md` §4 (B1 thread the hot VDBE
opcode sequence into a superblock; B2 cross-opcode optimization).
**Status:** feasibility study + working, bit-exact, gated **prototype** with a **measured speedup**.
**Verdict (up front):** Meta-tracing the VDBE is **feasible and correct** — the dispatch is the single
largest indirect-branch site (13.86% of all indirect traffic) and its target stream is highly
path-predictable (order-3 Markov = **98.1%**). The prototype proves the new capability (speculatively
inline a stable indirect-branch target with a guard) end-to-end and is bit-exact across the whole bench
corpus. **But B1 *alone* has a modest ceiling (~5% on sqlite): the per-indirect-branch cost is dominated
by the x16/x17 red-zone stash, which is A1's job, not B1's. B1 unlocks its real value only stacked on A1.**

All work is in `/Users/x/arm-b1` (mirror `…/opt-work/arm-b1`), built/run on the macOS host via the `mac`
bridge against the native aarch64 guest `dd-tests/.bench-cache/aarch64/sqlite`. Diff: `arm-b1.diff`
(6 files, +347/−7, everything behind `IBPROF` / `VDBETRACE` gates; default path byte-identical).

---

## 0. Baseline reproduced

| | native aarch64 (this Linux host) | dd JIT (mac) | ratio |
|---|--:|--:|--:|
| sqlite (600k-row INSERT + index + GROUP BY) | 0.340 s (min 0.328) | 0.624 s (min 0.618) | **1.83–1.88×** |

Matches the doc's 1.93×. `PROF=1`: `crossings≈7963 ibtc_miss≈1107 branch_cross≈3967 translations≈4990`
— every dispatcher-boundary metric is tiny. The overhead is **in-code, on indirect transfers that *hit*
the inline IBTC**, exactly as the doc states; the existing counters are blind to it.

---

## 1. VDBE dispatch characterization (the instrumentation: `IBPROF=1`)

`ibtc_miss≈1107` proves the dispatch never misses to the C dispatcher — it hits the inline IBTC every
time, so its traffic is invisible to the existing counters. To see it I added **`IBPROF`** (gated,
measurement-only): it routes *every* guest indirect transfer (br/blr/ret) through the C dispatcher
(reason `R_IBLOG`) and records, per branch **site**, the executed **target stream** — traffic, a
last-value (per-site monomorphic-IC) hit rate, and order-1/2/3 Markov hit rates.

### The dispatch is exactly one instruction

`sqlite3VdbeExec` is a `switch(pOp->opcode)` (no computed-goto in 3.45 at `-O2`) → clang emits a single
jump-table dispatch. Disassembly (`objdump`), guest file offset `0xa1f68`:

```
a1f4c  ldrb  w25,[x21]            ; w25 = pOp->opcode   (x21 = pOp, immutable bytecode)
a1f54  cmp   w25,#0xb8 ; b.hi …   ; range check -> default
a1f5c  ldrh  w0,[x20,w25,uxtw#1]  ; w0  = jumptable[opcode]   (x20 = table base)
a1f60  adr   x1, a1f6c            ; x1  = handler-block base
a1f64  add   x0,x1,w0,sxth#2      ; x0  = base + offset*4
a1f68  br    x0                   ; <-- THE dispatch. exactly one `br` in all of VdbeExec.
```

### Per-site traffic + stability (full 600k run, top sites)

```
rk   off      func+off                 count        %traf  mono%   o1%    o2%    o3%
0    a1f68    sqlite3VdbeExec+e4     20,897,762     13.86    6.2  75.63  92.37  98.12   <-- VDBE dispatch
1    4c20c    vdbeSorterCompareInt   11,565,995      7.67  100.0 100.00 100.00 100.00
2    f6e0     sqlite3GetVarint       11,435,319      7.59  100.0  99.98  99.98  99.98
3    10b68    vdbeSorterMerge         9,275,295      6.15  100.0 100.00 100.00 100.00
5    de9cc    __memcpy_generic ret    4,124,426      2.74   30.8  96.87  96.81  96.84
10   4c81c    getAndInitPage          2,950,970      1.96   59.1  63.65  99.92  99.92
18   fbc4     pcacheManageDirtyList   1,752,222      1.16   30.2  39.04  99.34  99.37
...  (total indirect transfers over the run = 150,732,514; 742 distinct sites)
```

**Findings**

1. **The VDBE dispatch is the #1 indirect-branch site: 20.9 M executions = 13.86% of *all* 150.7 M
   indirect transfers.**
2. **Its per-site monomorphic IC is dead (`mono%` = 6.2%).** Because the site is polymorphic (consecutive
   dispatches go to *different* opcode handlers), the engine's per-site inline cache almost always
   mispredicts, so **the dispatch pays the full polymorphic shared-hash IBTC probe on ~94% of executions**
   — the most expensive indirect path the engine has. Every other top site (ranks 1–4, 6–9, …) has
   `mono%≈100`: those are returns/calls already served cheaply by the per-site IC / §B shadow-RAS, so
   they are **not** the problem. The dispatch is.
3. **Stability climbs sharply with path context** — the crux feasibility number:

   | predictor (keyed on …) | dispatch hit rate |
   |---|--:|
   | order-0 last-value (= today's per-site IC) | 6.2% |
   | order-1 (last 1 handler) | 75.6% |
   | order-2 (last 2 handlers) | 92.4% |
   | order-3 (last 3 handlers) | **98.1%** |

   The aggregate is *diluted* by the one-time query-plan/sort/btree-build phases and by genuine
   data-dependent VDBE branches (`OP_Next` loop-exit, comparisons). A **trace** carries the *entire*
   observed path (depth ≫ 3), so within an immutable prepared statement the dispatch target is fixed
   except at real VDBE control-flow divergence. The order-k climb is the lower bound; **a threaded
   superblock drives the dispatch guard hit rate toward ~100%.** This is the legitimate beat-native
   opening: native sqlite pays this dispatch (`ldrb…br`, ~6 insns) on every opcode and *cannot* remove
   it; a meta-trace can.

---

## 2. Achievable-win estimate

Host instructions for ONE dispatch (the guest `ldrb/cmp/b.hi/ldrh/adr/add` table-lookup is emitted
verbatim in *all* cases and is shared with native):

| | extra host insns over native | memory ops | terminator |
|---|--:|--:|---|
| **native** (the 6 guest dispatch insns) | 0 | 1 (table load, predicted) | `br x0` (HW-predicted, stable) |
| **dd baseline** (per-site IC miss → shared-hash hit) | **+15** | 5 (`stur×2`, `ldp`, `ldur×2`) | `br x16` (data-dependent, mispredicts) |
| **B1 prototype** (SDC guard hit) | **+6** | 3 (`stur`, `ldr`-lit, `ldur`) | `b body` (direct, predicted) |
| B1 + A1 (stolen x16/x17), projected | ~+3 | ~1 | `b body` |
| B1 + opcode-guard + A1, projected | ~+1 | ~1 | `b body` (≈ native) |

So B1's SDC cuts the dispatch's over-native overhead from **+15 to +6 host insns** and removes the
mispredicting indirect branch. Empirically each SDC hit saves ~1.5 ns (measured: 0.77% of 0.624 s over
3.1 M hits). Scaling to full coverage of the 20.9 M dispatches → **~5% wall-clock on sqlite for B1
alone**. The ceiling is set by the **3 remaining red-zone memory ops**, which are A1's target (steal
x16/x17 → 0 mem ops). **B1+A1 together** turn the threaded dispatch into a near-free `cmp; b.eq` and is
the real "< 1.0× beat-native" combination; B2 (cross-opcode Mem-cell load/store folding) rides on top.

---

## 3. The prototype: speculative direct-chaining (SDC) + path-specialization (`VDBETRACE=1`)

Today's traces stop at indirect branches (W6-C: single-block/call-free). The new capability is to
**speculatively inline a stable indirect target behind a guard**. The prototype does this for the VDBE
dispatch in two parts.

### 3a. SDC — the threaded indirect branch (`emit_vdbe_sdc`, emit_arm64.c)

A clang jump-table `br` is recognized structurally (`is_jt_dispatch_br`: `ldrh …,uxtw#1 ; adr ; add
…,sxth#2 ; br`, generic, not sqlite-specific). For such a `br` under `VDBETRACE`, instead of the IBTC
probe we emit:

```
stur x16,[sp,#-16]            ; one scratch for the guard
ldr  x16, Lspec_tgt           ; speculated guest target (0 until first fill)
sub  x16, x16, xRn            ; guard: computed target == speculated?
cbnz x16, Lhash               ; NO  -> shared-hash IBTC (in-cache fallback, unchanged)
ldur x16,[sp,#-16]            ; YES -> restore, then ...
b    Lspec_body               ;        DIRECT chain into the handler body (back-patched; predicted)
Lhash: <byte-identical shared-hash IBTC from emit_ibranch>
```

`Lspec_tgt`/`Lspec_body` are filled lazily: on the first miss the site exits to the dispatcher with
`ic_site` tagged (bit0=1); `sdc_fill` writes the guard literal and back-patches the in-cache `b` to the
target body, then *also* fills the shared hash (so non-speculated targets still hit in-cache). The
fallback is the **existing** shared-hash IBTC, so a miss is never worse than baseline and adds **no**
dispatcher round-trips in steady state (confirmed: `ibtc_miss` and `crossings` unchanged with the gate
on).

### 3b. Path-specialization — make the shared `br` per-predecessor (translate.c)

A single shared dispatch `br` is order-0 (6.2% SDC hit) — useless. The prototype **force-inlines a
private copy of the dispatch block into each predecessor handler** (`is_jt_dispatch_block` +
`g_vdbetrace` override of opt4's `!map_body` guard), so each handler's `br` sees only *its* successor
(order-1+, 75–98% stable). This is exactly "form a superblock that threads the observed opcode-handler
sequence," reusing the W4-E/opt4 region machinery.

### Measured result

```
VDBETRACE=1:  sdc_sites=20  sdc_fills=166  force_inlined_dispatch=11   (output bit-exact: 2325248)
VTHITCOUNT=1: guard_hits=3,100,167   (= 14.8% of the 20.9 M dispatches taken as threaded direct chains)
```

- **Speedup (clean, interleaved A/B, 25 reps):** median **0.77%**, min **0.66%** — small but real and
  reproducible, at **only ~15% dispatch coverage**.
- The coverage cap is **topology, not the mechanism**: only 11 predecessors terminate with a *direct*
  `b dispatch`; the rest reach the (shared) loop top through shared intermediate blocks
  (`check_for_interrupt`, `jump_to_p2` setup) that single-edge force-inlining does not duplicate.
  Projecting the per-hit saving to 100% coverage → ~5% (matches §2).

---

## 4. Guard-failure analysis & correctness

- **The guard is an exact 64-bit equality on the *real* computed target** (`xRn`). A misspeculation can
  therefore never land wrong — it falls into the normal shared-hash IBTC. The mechanism is **bit-exact by
  construction**.
- `Lspec_tgt` starts at 0 and no guest code address is 0, so the direct chain is unreachable until
  `sdc_fill` arms it.
- Guard *failures* (different next opcode) are absorbed in-cache by the shared hash; on a genuinely-new
  target the dispatcher re-specializes (last-value adapt). Steady state: the speculated successor goes
  direct (fast), others hit the shared hash (= baseline cost), no extra round-trips.
- **Verified bit-exact** vs the default engine on the whole corpus under both gates:
  `int-sieve, qsort, base64, text-scan, mandelbrot, matmul, sha256` and `sqlite` all produce identical
  output (`IBPROF`, `VDBETRACE`, baseline). The generic JT detector fires **only** where a real compiler
  switch exists (sqlite); `sdc_sites=0` on the compute guests — no perturbation.
- Cache-relocation hazards (wholesale flush, fork) are not introduced: SDC records live in the code
  cache, are dropped+rebuilt with it, and no SDC fill is pending across a flush/fork (it completes in the
  same dispatcher visit as the miss).

---

## 5. Concrete path to full implementation

1. **Stack on A1 first.** B1's ceiling is the 3 red-zone mem ops; with x16/x17 engine-private the
   threaded hit is `ldr-lit; sub; cbnz; b` (≈3 insns, 1 mem). A1 is the prerequisite multiplier.
2. **Real superblock recorder (full coverage).** Replace single-edge force-inlining with a hot-path
   trace recorder (extend the W4-E tier-2 substrate): when the dispatch `br` is hot, record the executed
   handler chain (predecessor→dispatch→successor→…) and emit it as one threaded superblock with one SDC
   guard per dispatch step. This lifts coverage from ~15% to ~100% and is where the projected ~5% (B1
   alone) / sub-1.0× (B1+A1+B2) is realized.
3. **Cheaper guard (opcode-guard, the doc's suggestion).** Guard on `aOp[pc].opcode` (the immutable byte)
   *before* the table lookup, letting the trace skip the guest `ldrh/adr/add` entirely — approaching the
   native ~6-insn dispatch. Needs care with guest NZCV liveness across the dispatch (use a flag-free
   compare or a dead scratch).
4. **B2 cross-opcode optimization.** Once threaded, fold redundant VDBE Mem-cell load/stores between
   adjacent opcodes and hoist invariant cursor/pointer setup out of the per-row loop — the trace
   optimizations the static VDBE cannot do.
5. **Polymorphic SDC (2-way).** For the binary-branch handlers (`OP_Next`, comparisons) a 2-entry SDC
   removes the last in-cache shared-hash hits.

---

## 6. Bottom line

- **Is meta-tracing the VDBE feasible?** Yes. The dispatch is one identifiable, dominant (13.86%),
  highly path-predictable (o3=98.1%) indirect branch, and the prototype threads it **bit-exactly** with a
  guard that cannot misfire.
- **Beat-native potential:** B1 **alone** ≈ 5% on sqlite (measured 0.77% at 15% coverage, projected to
  full coverage). The genuine **< 1.0× beat-native** result needs **B1 + A1 (stolen x16/x17) + B2** —
  B1 removes the dispatch indirect branch native can't, A1 removes the red-zone stash that otherwise caps
  B1, B2 removes the redundant inter-opcode memory traffic.
- **What the prototype proved:** the core new engine capability — *speculatively inline a stable
  indirect-branch target across the dispatch `br`, path-specialized, behind a cheap exact guard, with a
  correct in-cache fallback* — works, is bit-exact on the full corpus, takes 3.1 M threaded direct chains
  on the real workload, and yields a measured (if currently coverage-bounded) speedup. The remaining work
  is coverage (a real superblock recorder) and stacking on A1, both well-defined.

## Appendix — gates / repro
- `IBPROF=1` — indirect-branch traffic + order-0/1/2/3 stability log (forces round-trips; counts only).
- `VDBETRACE=1` — the SDC + path-specialization prototype (the optimization).
- `VTHITCOUNT=1` — inline SDC guard-hit counter (diagnostic; perturbs timing).
- Build: `mac bash -lc "cd /Users/x/arm-b1 && clang -O2 -o ddjit-arm src/runtime/targets/linux_aarch64.c && codesign -s - --entitlements jit.entitlements -f ddjit-arm"`
- Run:   `mac bash -lc "cd /Users/x/arm-b1 && env VDBETRACE=1 ./ddjit-arm /Users/x/dd/dd/dd-tests/.bench-cache/aarch64/sqlite"`
