# ARM SQLite — reaching, then beating, VM/native parity

**Goal:** make `sqlite` on the aarch64 (same-ISA) engine **≥ native (= the ARM VM)**. Today it's **1.93×
slower**; this is the plan to close it and then beat it.

On ARM a "VM" runs the guest natively, so our target is **native aarch64**. A same-ISA transliterator has
*no translation reason* to be slow — so parity is purely about removing DBT overhead, and beating native
requires the HP-Dynamo move: doing something the static compiler **couldn't**.

> **A1 MEASURED — reframing (2026-06-28).** A1 (steal x16/x17) is implemented & bit-exact but moved sqlite
> only **1.90→1.87× (−1.5%)**. Decisive finding: a monomorphic indirect loop is **already 1.01× native with
> the optimization OFF** — the OoO core fully hides the red-zone mem-ops this doc blamed. **Probe length is
> NOT the bottleneck.** The residual sqlite/qsort gap is **indirect-branch misprediction + VDBE dispatch**.
> → Ship A1 as a cheap, correctness-positive cleanup (gate `NOSTEAL1617`), but the parity effort belongs to
> **A3 (RAS return coverage → fewer mispredicts)** and **B1 (VDBE meta-trace → remove dispatch)**. A2/A4
> (probe tightening) are deprioritized; B3 (scalar idioms) dropped — the gap isn't scalar compute.
>
> **A3 MEASURED — inverted, and it's the big win (2026-06-28).** A3's premise was backwards: §B shadow-return
> prediction is a **net pessimization** here. Its push (~19) + validate-ret (~22) ≈ 40 host insns vs the cheap
> IBTC return it replaces, and returns already hit the IBTC (~1,105 misses in ~97M returns) — no headroom.
> Widening the gate raised hit-rate (sqlite 33.4%→51.9%) but was *slower*. **Fix: §B-OFF by default** (every
> `ret` → `emit_ibranch(30)`) + 16-byte block-entry alignment. **sqlite 1.88×→1.47× vs native (−21.5%)**, qsort
> −20.6%, deep recursion −50–57%, straight-line flat; byte-exact and *strictly safer* (no shadow stack → no
> longjmp/fork mispredict surface). Gate `NOSHADOWTUNE=1`. **This is the biggest ARM-sqlite win so far.** The
> remaining gap to <1.0× is now VDBE dispatch → **B1** is the last lever standing.
>
> **B1 MEASURED — feasible, and it reframes A1 (2026-06-28).** The VDBE dispatch is a single `br x0`
> (`sqlite3VdbeExec` switch) — the #1 indirect site (20.9M = **13.86%** of all 150.7M indirect transfers) with
> a *dead* per-site IC (6.2% mono → full polymorphic IBTC ~94%). Path-context stability: order-0 6.2% →
> order-3 **98.1%**, so guards hit ~100% with trace context. The `VDBETRACE` prototype speculatively inlines
> the stable dispatch target (exact 64-bit guard + fallback), **bit-exact**, 3.1M threaded chains, **+0.77% at
> ~15% coverage** (topology-capped, not mechanism-capped). B1 alone ≈ **5%** at full coverage — but its hit
> path is **capped by the x16/x17 red-zone stash, which is exactly what A1 removes.** So A1 is the **enabler
> for B1**, not a standalone micro-opt.
>
> ## ROADMAP (revised, evidence-based)
> 1. **A3 (§B-off + align) — SHIP NOW.** 1.93×→**1.47×**, bit-exact, safer. Biggest single win.
> 2. **A1 (steal x16/x17) — SHIP** as cleanup *and* as the B1 enabler (uncaps the threaded hit path).
> 3. **B1 (VDBE meta-trace) — build out** the real superblock recorder (path-context order-3 specialization)
>    for coverage; removes the dispatch branch native itself pays.
> 4. **B2 (inter-opcode fold)** on the trace → the final push to **< 1.0× (beat native)**.
> Stack A1+B1+B2 on the A3 baseline. The gap is now provably **dispatch + misprediction**, not codegen/probe/idioms.

---

## 1. The problem, measured

| guest | native | dd JIT | ratio | nature |
|---|---|---|---|---|
| sha256 | 0.768 | 0.774 | **1.01×** | straight-line NEON |
| int-sieve | 0.749 | 0.539 | **0.72× (faster)** | tight compute loop |
| qsort | 0.789 | 1.332 | **1.69× slower** | indirect comparator callbacks |
| **sqlite** | 0.336 | 0.654 | **1.93× slower** | VDBE interpreter + callbacks |

**Codegen is already native-quality** (sha256 1.01×, int-sieve *faster*). The gap is **indirect-branch /
call / return overhead** — qsort (pure comparator callback) isolates it; sqlite stacks the VDBE on top.

`PROF=1` for the 600k-row run: `crossings≈7957  ibtc_miss≈1105  branch_cross≈3963  translations≈4984
xlate_ms≈10.7  wx_toggles=0`. Every boundary metric is **tiny** for the work done — translation is ~3% of
the gap, dispatcher round-trips are negligible (hot loops stay chained/IBTC-cached). **The overhead is
in-code, on indirect transfers that *hit* the IBTC** (the cost is the per-branch probe instructions, not
misses).

**The existing trace levers don't help sqlite:** `NOTIER2=1` == default (0% — tier-2 captures nothing),
`NOSTITCH=1` only +5%. Confirms W6-C: single-block/call-free traces can't capture interpreter dispatch, so
the whole 1.93× is unrecovered per-indirect-branch cost.

---

## 2. Root cause: the indirect-branch hit path

Every guest `br`/`blr`/`ret` runs the inline IBTC probe (`emit_ibranch`, `jit/emit_arm64.c`). Even the
**monomorphic-hit** (common) case is, vs native's single `br`:

```
stur x16,[sp,#-16]   ; stash scratch pair        (mem)
stur x17,[sp,#-24]   ;                            (mem)
ldr  x16, Lsite_tgt  ; cached target (literal)
sub  x16, x16, xRn   ; compare
cbnz x16, Lhash      ; miss -> shared hash
ldr  x16, Lsite_body ; cached body
br   x16             ; -> body_ind:
                     ;   ldr x16,[sp,#-16]        (mem)
                     ;   ldr x17,[sp,#-24]        (mem)
```
≈ **9 instructions + 4 stack memory ops** per indirect branch; **returns add +1** (`ldr x30,[cpu,#30*8]`
— x30 is stolen). The 4 red-zone stores/loads (and the address dependency into the final `br x16`) are the
expensive part.

**Why SQLite is the worst case:** its VDBE is a bytecode interpreter — `switch(aOp[pc].opcode)` compiles to
a **jump-table `br` per opcode** — plus B-tree comparator callbacks and deep `bl`/`ret` chains. The 600k-row
INSERT alone runs the same ~15-opcode VDBE program 600k times → tens of millions of indirect transfers ×
~8–12 extra host instructions = the 1.9×.

---

## 3. Tier A — reach parity (remove DBT overhead) → target ≈ 1.0×

### A1. Steal host `x16`/`x17` for the engine → kill the per-branch stash/restore  *(P0, biggest lever)*
The IBTC uses `x16`/`x17` as scratch, but they're **guest-visible** (IP0/IP1), forcing the 4 red-zone
mem-ops every branch. Make them **engine-private** (mangle guest `x16`/`x17` like the already-stolen
`x18`/`x28`/`x30`). The hit path collapses to ~**5 instructions, 0 memory ops**, and the `body_ind` restore
stub disappears:
```
ldr x16, Lsite_tgt ; sub x16,x16,xRn ; cbnz x16,Lhash ; ldr x16,Lsite_body ; br x16
```
- **Trade:** guest `x16`/`x17` data uses now mangle (spill/reload). In hot interpreter/callback loops these
  are far rarer than indirect branches, so net should be a large win — but it **must be measured** (compilers
  use IP0/IP1 for veneers + large-offset addressing). The mangle machinery already exists for x18/x28/x30.
- **Expected:** the bulk of the parity gap (removes 4 mem-ops + the store→load→`br` dependency chain that
  serializes every indirect transfer). Validate on sqlite **and** qsort/redis.
- Gate + bit-exact A/B (WS1 gate-sweep + WS2 differential cover correctness).

### A2. Tighten the monomorphic compare  *(P1, after A1)*
With x16/x17 free, fold the compare/load (e.g. keep `{target,body}` so a hit is `ldr`+`sub`+`cbnz`+`br`),
and align indirect-branch sites so the hit path doesn't straddle a cache line. Minor vs A1.

### A3. Return-stack coverage for sqlite  *(P1)*
Returns are ~half of indirect transfers and are highly predictable. §B (`emit_shadow_push`/`emit_shadow_ret`)
already predicts them via the hardware RAS (~4 instrs) — but it's **depth-gated** and a non-fired return
falls to the full IBTC path. **Action:** measure §B hit-rate on sqlite's call depth; widen/auto-tune the gate
so sqlite's `bl`/`ret` chains stay predicted. A1 also makes the §B-miss fallback cheaper.

### A4. Re-examine the stolen set on the return path  *(P2)*
`ret` reloads `cpu->x[30]` every time (x30 stolen). Once A1 changes the register budget, reconsider whether
the return path can keep the guest return address in a host reg across a predicted chain.

---

## 4. Tier B — beat native (what AOT couldn't): meta-trace the VDBE → target < 1.0×

Native sqlite is **itself an interpreter** — it pays VDBE dispatch overhead too. A DBT that traces the hot
guest opcode stream can remove dispatch that *both* currently pay. This is the legitimate beat-native lever.

### B1. Trace the hot VDBE opcode sequence into a threaded superblock  *(P2, highest ceiling)*
A prepared statement re-executes the **same immutable bytecode** (the INSERT loop = ~15 fixed opcodes ×
600k). Detect the hot dispatch loop and form a **superblock that threads the observed opcode-handler
sequence**, replacing the per-opcode jump-table `br` with **direct chaining + a cheap guard** on
`aOp[pc].opcode` (immutable → guards essentially never fail). This eliminates the dispatch indirect branch
entirely on the hot path — overhead native cannot remove. (Dynamo / PyPy-meta-tracing principle: trace
*through* the guest interpreter loop.)
- Hard part: the dispatch `br` target is data-dependent (`aOp[pc].opcode`); we speculate the sequence and
  guard. Builds on the W6-C trace substrate but must extend across indirect branches (speculative inlining of
  a stable indirect target), which today's call-free/single-block constraint forbids.

### B2. Cross-opcode optimization inside the trace  *(rides B1)*
Once threaded: fold redundant VDBE register (Mem cell) load/stores between adjacent opcodes, hoist invariant
cursor/pointer setup out of the per-row loop, devirtualize the comparator. Standard trace optimizations the
static VDBE can't do.

### B3. Idiom upgrades in sqlite's portable routines  *(P2, incremental)*
sqlite's portable hashing, `memcmp`, varint decode, and the sorter's comparisons → wider NEON / LSE where
bit-exact. The other legitimate beat-native source (same class as the landed LSE/rep-string wins).

---

## 5. Phasing, risk, validation

| Phase | Lever | Expected | Risk |
|---|---|---|---|
| **P0** | A1 steal x16/x17 | most of parity | guest x16/x17 mangle cost — **measure first** |
| **P1** | A3 §B coverage + A2 | close to 1.0× | low |
| **P2** | B1 VDBE meta-trace (+B2/B3) | **< 1.0× (beat native)** | high complexity (speculative indirect tracing) |

- **Benchmarks:** the `make bench` / `.bench-cache` harness — sqlite, qsort (callback proxy), redis (the
  twin server case), against native aarch64; plus sha256/int-sieve as the "must-stay-native" guard.
- **Correctness:** every lever bit-exact — `make test-diff` (qemu/native oracle) + `make test-gates`
  (gate on==off, incl. stacked). A1 especially: the x16/x17 mangle must be byte-identical on the full corpus.
- **Cross-workload:** A1/A3 scale to *all* indirect-heavy real software (redis event loop, qsort, awk, any
  interpreter), so they pay back well beyond sqlite. B1 is sqlite/interpreter-specific but generalizes to
  any guest-interpreter workload.

## 6. Why this is achievable
- The engine already runs straight-line code at/above native (proven) — there is no codegen deficit to fight.
- The overhead is concentrated in one well-understood path (the IBTC probe) → A1 is a targeted, high-ROI cut.
- Native sqlite leaves dispatch overhead on the table (it's an interpreter) → B1 is a real opening to pass it.
