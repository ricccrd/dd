# A3 — §B shadow-return coverage for sqlite (dd-jit aarch64→arm64)

**Lever:** `docs/design/arm-sqlite-parity.md` §A3 — "Return-stack coverage for sqlite". The plan
hypothesised that §B (`emit_shadow_push`/`emit_shadow_ret`, the depth-gated hardware-RAS return
predictor) is **under-covering** sqlite's `bl`/`ret` chains, and that **widening the depth-gate** would
predict more returns and close the parity gap.

**Result (headline):** the hypothesis is **disproven by measurement, and inverted.** §B is **net-negative
on every return-heavy workload tested** — including the *ideal* polymorphic deep-recursion cases it was
designed for. The §B push+validate path is ~40 host instructions of bookkeeping that costs **more** than
the IBTC return path it replaces, because sqlite's returns almost all already *hit* the IBTC (≈1105 misses
in ≈97 M returns). The correct tune is therefore the **opposite of "widen": disable §B** and return every
`ret` through the proven IBTC. Doing so is **byte-exact**, fixes the longjmp/fork misprediction surface by
construction (the shadow stack is simply unused), and gives:

| guest | native | §B-on (baseline) | **§B-off (new default)** | baseline ratio | **new ratio** | Δ wall |
|---|---:|---:|---:|---:|---:|---:|
| **sqlite** | 0.330 | 0.6195 | **0.4861** | 1.88× | **1.47×** | **−21.5%** |
| qsort | 0.780 | 1.2974 | **1.0297** | 1.66× | **1.32×** | −20.6% |
| longfib (recursion) | 0.026 | 0.1529 | **0.0652** | 5.9× | **2.5×** | −57% |
| deepcall (nested) | 0.043 | 0.2679 | **0.1345** | 6.3× | **3.1×** | −50% |
| sha256 *(must-not-regress)* | 0.770 | 0.6963 | 0.6973 | 0.90× | 0.91× | +0.1% (flat) |
| int-sieve *(must-not-regress)* | 0.736 | 0.4492 | 0.4482 | 0.61× | 0.61× | −0.2% (flat) |
| float-nbody *(leaf-heavy, must-not-regress)* | 0.162 | 0.1552 | 0.1550 | 0.96× | 0.95× | flat |

Wall = median of 9 interleaved runs via the `mac` bridge (seconds); native = median-of-5 on the aarch64
Linux host. All §B-on numbers are `NOSHADOWTUNE=1` (byte-identical to the committed baseline).

---

## 1. Instrumentation (PROF=1)

Added §B counters (gated on `g_prof`, zero steady-state cost when off):

- **runtime** (bumped from emitted code via `emit_prof_bump`, x9/x10 + red-zone, like `emit_t2_counter`):
  `shadow_push` (pushes executed), `shret_hit` (predicted-return FAST host-`ret`), `shret_fb` (returns
  that fell through `emit_shadow_ret` to the IBTC). `hit_rate = shret_hit / (shret_hit + shret_fb)`.
- **translate-time:** `bl_shadow` / `bl_leaf` — how the depth-gate split `bl` sites.

New `[prof]` line: `shadow_push=… shret_hit=… shret_fb=… hit_rate=…% bl_shadow=… bl_leaf=…`.

### §B hit-rate, before vs after widening (the measurement that motivated the inversion)

| guest | baseline §B (NOSHADOWTUNE=1) | widened §B (SHADOWGATE=1) | widened (SHADOWGATE=2) |
|---|---|---|---|
| **sqlite** | **33.4%** (push 32.5M / fb 64.8M) | **51.9%** (push 50.5M / fb 46.9M) | 51.9% |
| qsort | 4.7% | 4.7% | 4.7% |
| recursion (fib/ack) | **0.0%** (43 pushes total!) | **99.9%** | 99.9% |

Two real gate bugs were found and the widen-fix corrects them:
1. **`scan_calls` window/reach** — its 64-insn entry scan with forward-reach capped at `i+off<64` makes
   any function larger than the window (fib at `-O2`, most sqlite VDBE helpers) exhaust having seen no
   `bl`, so it is misreported "leaf" → §B withheld. fib(30) thus got **0%** coverage. The fix treats
   window-exhaustion as deep (`-1`) → recursion jumps to **99.9%**, sqlite 33→52%.
2. **depth-2 "shallow" rule** — a helper that calls only leaves is treated as monomorphic; but if it is
   called from *many* sites its single return site is polymorphic. `SHADOWGATE=2` pushes for any direct
   call. (No additional benefit for sqlite — its bl coverage is already saturated at L1.)

**But higher hit-rate did not mean faster** — see §3.

## 2. Root cause: §B bookkeeping > the IBTC return it replaces

The plan estimates §B at "~4 instrs". The actual emitted code is **`emit_shadow_push` ≈ 19 insns + sstk
stores** and **`emit_shadow_ret` ≈ 22 insns** (two `stp`/`ldp` pairs to `mscratch`, `ssp`/`sstk`/`gsp`
loads, the `guest_ret` **and** `guest_sp` compares) — ≈ 40 host instructions of software bookkeeping per
predicted call/return. The hardware RAS's 1-cycle `ret` is real, but it is buried under that bookkeeping.

§B only wins when it replaces a genuine **IBTC miss** (per-site IC thrash → shared-hash probe / dispatcher
round-trip). sqlite's PROF shows only **≈1105 IBTC misses in ≈97 M returns** — i.e. ~99.999% of returns
already *hit* the IBTC cheaply (the plan itself says "the cost is the per-branch probe, not misses"). So
there is **no miss headroom for §B to recover**; every shadow push/ret it adds is pure overhead.

## 3. The decisive experiment: widen ↑, narrow ↓, off

Interleaved medians (seconds), sqlite:

| config | sqlite | qsort | longfib | deepcall |
|---|---:|---:|---:|---:|
| widened §B (SHADOWGATE=1, **52% hit**) | 0.700 | 1.359 | 0.156 | 0.281 |
| baseline §B (NOSHADOWTUNE=1, **33% hit**) | 0.617 | 1.321 | 0.153 | 0.269 |
| **§B OFF (default, no §B at all)** | **0.482** | **1.042** | **0.066** | **0.139** |

Widening (more coverage) is the **slowest**; §B-off is the **fastest** — monotonically. Even on
**longfib** (naive `noinline` recursion whose return target alternates between two call sites → exactly
the polymorphic-return pattern §B targets), §B-off is **2.3× faster** than §B-on. The shared-hash IBTC
(~15–20 insns even when the per-site IC thrashes) still beats §B's ~40-insn bookkeeping.

## 4. The change (and why it is safe)

`shadowgate()` env-resolved gate (read once), in `frontend/aarch64/translate.c`:

| value | meaning |
|---|---|
| **−1 (DEFAULT)** | **§B OFF.** No shadow push; every `ret` → bare `emit_ibranch(30)` (per-site IC + shared hash). |
| −2 (`SHADOWGATE=-2`) | §B off on the push side; `ret` keeps the (empty→IBTC) shadow-ret stub. |
| 0 (`NOSHADOWTUNE=1`) | **EXACT original §B-on gate** — byte-identical baseline codegen. A/B kill switch. |
| 1 / 2 (`SHADOWGATE=1`/`2`) | the widen-fixes (kept for the record; measured slower). |

**Why §B-off is correctness-safe — it is *strictly* the existing, validated path.** §B-off routes every
`ret` to `emit_ibranch(30)`, which is *exactly* the fallback `emit_shadow_ret` already takes on a
shadow miss (it loads guest `x30` from `cpu->x[30]` and runs the IBTC). We are not adding a new return
path — we are *removing* the speculative one and always taking the proven fallback. The §B-misprediction
surface the plan worried about (recursion/fork/**longjmp**/sigjmp) is **eliminated by construction**: with
no pushes the shadow stack stays empty, so there is nothing to mispredict.

**`G_BLOCK_ALIGN` (16-byte block-entry alignment, §B-off only).** §B-off emits smaller per-`bl` stubs,
which shifted hot NEON loops into a worse cache/predictor alignment — sha256 (which has **zero** hot
returns: `shadow_push=0`) deterministically wobbled **~+7%**. Aligning each freshly translated block entry
to 16B (padding *precedes* the entry, branch/IBTC targets the aligned body, so the nops never execute →
zero runtime cost) removes the wobble entirely: sha256/int-sieve/float-nbody all return to flat. Gated on
`shadowgate()<0`, so `NOSHADOWTUNE=1` is untouched; defined as compile-time `0` on x86 (dead-stripped).

## 5. Correctness / soak (all byte-exact vs native **and** vs the unmodified `NOSHADOWTUNE=1` engine)

| guest | native | default (§B-off) | §B-on |
|---|---|---|---|
| sqlite | 2325248 | 2325248 | 2325248 |
| qsort | 12925744094040 | = | = |
| recursion (fib/ack) | fib=832040 ack=13 | = | = |
| longjmp | longjmp r=42 | = | = |
| sigjmp | sigjmp hops=3 from=3 | = | = |
| signals | signal got=12 | = | = |
| forkwait | reaped=8 sum=36 | = | = |
| longfib / deepcall / fnptr | (see above) | = | = |

Soak: `soak_forkchurn` ×3 → `reaped=3000 acc=151500` (stable); sqlite ×8 → all `2325248`.

## 6. Recommendation

- **Adopt §B-off as the default** (`shadowgate()=-1`). It is a large, clean win on every return-heavy
  workload (sqlite **1.88×→1.47×**, qsort 1.66×→1.32×, recursion/nested-call 2–2.3×), flat on the
  must-not-regress guards, byte-exact, and *reduces* the correctness surface. Keep `NOSHADOWTUNE=1` as the
  A/B revert to the original §B behaviour.
- **Retire / do not pursue widening** (`SHADOWGATE=1/2`): higher hit-rate, slower wall.
- **Return-path cost reduction quantified:** §B contributes **negative** value at current bookkeeping
  cost. Removing it is the return-path win. The plan's premise ("returns are ~half of indirect transfers
  and highly predictable") is correct — but they are already predicted **cheaply by the IBTC**, so the
  win is to stop paying §B's software-RAS tax, not to extend it.
- **Interaction with A1 (steal x16/x17):** A1 makes the IBTC return path even cheaper (≈5 insns, 0
  mem-ops), widening the gap further in §B-off's favour — i.e. A1 and A3-as-implemented-here compound. §B
  could only become competitive if its push/ret bookkeeping were slimmed to a handful of instructions
  (closer to the plan's "~4 insn" estimate), which is an A4-class register-budget project, not gate
  tuning. Until then, §B-off is the right call.

## 7. Files changed (diff: `arm-a3.diff`)

- `frontend/aarch64/translate.c` — `shadowgate()` gate; `scan_calls`/`target_is_leaf` widen-fixes (gated);
  `ret` routing (§B-off → bare IBTC); §B PROF instrumentation (`emit_prof_bump` + bumps).
- `jit/cache.c` — §B PROF counters.
- `jit/dispatch.c` — `G_BLOCK_ALIGN` block-entry alignment hook in the translate path.
- `frontend/aarch64/dispatch_hooks.h` — `G_BLOCK_ALIGN (shadowgate()<0)`.
- `frontend/x86_64/dispatch_hooks.h` — `G_BLOCK_ALIGN 0` (x86 unity build).
- `os/linux/service.c` — §B PROF dump line.
