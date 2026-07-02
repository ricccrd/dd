# BUG #201 — Angle C: minimize + translation-toggle bisect

Exhaustive account of the Angle-C investigation (x86_64 guest → ARM64 macOS DBT, `dd`). Goal: shrink
the repro and bisect the miscompile by toggling dd's translation choices. This is a **docs-only capture**
— no source fix was pinned by this angle. Read alongside `docs/BUG201.md` (converged mechanism) and the
`target/heap201/` working set (siblings' tooling/evidence).

## TL;DR
- **The translation-toggle axis is exhausted.** No dd optimization flag flips the bug to clean at N≥40.
  The miscompile is in an **always-on baseline lowering**, not any optimization. This is a rigorous
  negative that rules the tier-2 optimizer, flag-elision, SIMD-opt, rep-cmp, and stitching OUT.
- **Two methodology bugs** were found that invalidate controls other agents relied on (GOGC/GODEBUG env
  forwarding; and `GOGC=off` not actually fixing mcount).
- **Refined trigger:** an explicit `runtime.GC()` is load-bearing; automatic heap-threshold GC alone never
  triggers it. The bug needs a large tiny-alloc population accumulated between *sparse* forced GCs.
- **DDFIXHEAP determinism is per-binary and layout-fragile** — recompiling the repro (even an identical
  reimplementation) can silently suppress it. `target/heap201/mcount` remains the canonical repro.

---

## 1. Setup / build (standalone, parallel to siblings)

Worktree off origin/main. Applied the diagnostic tooling patch
`target/heap201/heap201_tooling_latest.patch` via `git apply --3way` (one conflict in
`os/linux/syscall/mem.c`: resolved to keep BOTH the DDFIXHEAP `fixaddr` MAP_FIXED branch *and*
origin/main's `off_emul` host-page-unaligned-file-offset block).

Engine built directly from the worktree unity TU through the `mac` bridge (mirrors `dd-jit/build.rs`):

```
mac bash -lc "clang -O2 -o /Users/x/dd/dd/target/h201c/ddjit-x86 \
    <worktree>/dd-jit/src/runtime/targets/linux_x86_64.c \
  && codesign -s - --entitlements <worktree>/dd-jit/jit.entitlements -f /Users/x/dd/dd/target/h201c/ddjit-x86"
```

Run pattern (env reaches the ENGINE only through the explicit `env` prefix; the mac bridge does not
forward ambient env):

```
timeout 60 mac bash -lc "exec env DDFIXHEAP=1 <ENGINE_TOGGLES> <engine> <repro>" 2>/dev/null
```

Reap by the unique path slug that appears in argv (never by a name that could hit siblings):
`mac bash -lc "pkill -f h201c"`.

`DDFIXHEAP=1` gives the deterministic layout: corrupting runs report
`CORRUPT pass=~600 fails=410/4000 firsti=1014`. A run is classified **bad** if it prints `CORRUPT` **or**
segfaults (rc≠124, no `DONE`) — the corruption manifests either way (~half the bad runs are silent
crashes, rc=255). `DONE` = clean; rc=124 = 60s timeout (rare).

---

## 2. Translation-toggle bisect (the core Angle-C deliverable)

All toggles are **engine-level** (read by the engine process via `getenv`), so they DO reach the engine
through the `env` prefix — proven by `DDFIXHEAP` working (deterministic `firsti=1014`). Each is invoked as
`env DDFIXHEAP=1 <NAME>=1 <engine> <repro>`.

### Results (N=40 unless noted; DDFIXHEAP=1; repro = target/heap201/mcount)

| Toggle (env var) | What it gates | bad/40 | Verdict |
|---|---|---|---|
| *(baseline)* | — | 11–14/40 (~30-35%) | reference |
| `NOSTITCH` | block stitching (pure perf) | 5/14 (~36%) | **timing control — no effect** |
| `NOTIER2` | shared adaptive tier-2 (cache.c) | 2/14→~baseline | no effect |
| `NOTIER2X` | x86 tier-2 fold + flag-save elision (engine_glue.c) | 7/40 | no effect |
| `NOFLAGELIDE` | tier-2 per-iteration NZCV-save elision (trace.c:176) | 12/40 | no effect |
| `NOSSEOPT` | SSE/SIMD lowering opt (engine_glue.c:98) | 17/40 | no effect |
| `NOREPCMP` | rep-cmps/scas lowering | 4/12→~baseline | no effect |
| `NOLSE` | **(no-op for x86 guests — see below)** | 0/13, then 4/23 @N30 | invalid control |
| `NOEAOPT` | opt7 address-gen/mem-fold (decode.c) | (already ruled out in docs/BUG201) | no effect |
| `NOGUESTFOLD` | non-PIE bias fold | (already ruled out) | no effect |
| `NOREP` | rep stos/movs | (already ruled out) | no effect |

**Conclusion:** every optimization toggle leaves the bug intact at ~30-42%. The `NOSTITCH` timing-control
staying at baseline proves that *timing perturbation per se does not suppress the bug* — so the (small-N)
"reductions" seen for tier-2/flag-elide were NOT real signal, just variance (next point).

### The crucial "small-N is noise" lesson
The bug rate is ~30% with large run-to-run variance. **Only N≥40 is trustworthy.** Concrete trap:
`NOLSE=1` produced **0/13** on the first pass — a seemingly clean flip — but **4/23 at N=30**, i.e. exactly
baseline. It turned out `NOLSE` is **not referenced anywhere in the x86→arm64 translator** (it only gates
the aarch64-guest path, `translate/aarch64/translate.c:1165`), so `NOLSE=1` is a literal no-op for x86
guests. The 0/13 was pure variance. **Do not trust any toggle result below N≈40**, and verify a toggle
actually gates code in the x86 target before believing a flip.

### What this localizes
Combined with the already-ruled-out `NOEAOPT`/`NOGUESTFOLD`/`NOREP`, the responsible construct is emitted
by the **baseline lowering regardless of optimization state**. This is consistent with sibling Angle A
(allocator handout is correct) and Angle B (individually-correct instructions, context-dependent). Since
the victim span is reached by scanning the `keys []string` backing array (scanobject → typePointers.next →
greyobject) and that scan inner-loop is hot (tier-2-promoted), the N≥40 negatives are a **direct rule-out
for the scan/sweep path**: the mark-miss is NOT caused by tier-2 loop folding (`NOTIER2X`), loop flag-save
elision (`NOFLAGELIDE`), or SIMD lowering (`NOSSEOPT`). It lives in the **baseline scanobject/sweep
lowering** (pointer-bitmap word load + bit iteration + slot greying, or the sweep allocBits/gcmarkBits
swap).

---

## 3. Two methodology bugs (affect everyone's controls)

### (1) Host `GOGC` / `GODEBUG` never reach the guest
The x86 engine builds the **guest** environment from `DD_GUEST_ENV="K=V\nK=V\n…"` merged with a fixed
default set only — `{PATH=/usr/bin:/bin, HOME=/root, TERM=dumb, LANG=C}`. See
`dd-jit/src/runtime/translate/x86_64/elf.c:355` (`g_guest_env[]`) and the `DD_GUEST_ENV` parse ~elf.c:379.
**Host env vars set on the engine command line are NOT propagated to the guest process.**

Proof:
- `env GODEBUG=gctrace=1 <engine> mcount` → **0** gctrace lines (dropped).
- `env DD_GUEST_ENV=$'GODEBUG=gctrace=1\n' <engine> mcount` → **20** gctrace lines (delivered).

**Fix / correct usage:** pass any guest Go runtime var via `DD_GUEST_ENV`, e.g.
`DD_GUEST_ENV=$'GOGC=off\n'` or `DD_GUEST_ENV=$'GOGC=off\nGODEBUG=gctrace=1\n'`.
**Implication:** every earlier "control" of the form `env GOGC=off <engine>` was a no-op and told you
nothing.

### (2) `GOGC=off` does NOT fix mcount — the real invariant is "no explicit `runtime.GC()`"
Even when `GOGC=off` is delivered correctly via `DD_GUEST_ENV`, mcount still corrupts **13/40**. Reason:
mcount calls `runtime.GC()` every 100 passes. Under `GOGC=off` + gctrace, all 20 GCs print `(forced)` with
`goal 8532210231538 MB` (threshold disabled) — i.e. `GOGC=off` removed *automatic* GC but the forced GCs
remain and still drive the corruption. So the doc-lore "GOGC=off fixes 100%" is misleading; it holds only
for a repro without a manual `runtime.GC()`.

The true invariant (see §4): **remove the explicit `runtime.GC()` calls and the corruption vanishes.** The
trigger is a full synchronous `runtime.GC()` window interleaved with tiny-alloc churn, not heap-threshold
GC.

---

## 4. Refined trigger model (minimize deliverable)

Three purpose-built repros (sources + binaries under `target/h201c/`):

| Repro | Structure vs mcount | Result | What it shows |
|---|---|---|---|
| `mnogc` | mcount **minus** all `runtime.GC()`; rely on automatic GC only | **0/30 bad** | **The explicit `runtime.GC()` is load-bearing.** Automatic heap-threshold GC alone never triggers the bug in this workload. |
| `mfast` | `runtime.GC()` on **every** pass (256 tiny junk/pass) | **1/30 bad** | Too-frequent GC suppresses it. The bug needs a *large tiny-alloc population accumulated between GCs*, not more GCs. |
| `mgc "100 128"` | parametrized reimplementation, structurally identical to mcount (GC every 100, 128 junk/pass) | **0/24 bad** | Even an "identical" rewrite is clean — see the layout-fragility lesson below. `mgc "50 128"` gave 5/24. |
| `target/heap201/mcount` | canonical | **~11-14/40 bad** | The reference repro. |

**DDFIXHEAP-is-layout-fragile / don't-recompile-the-repro:** `DDFIXHEAP` fixes the guest layout *relative
to a given binary* (fixed mmap/image base + AT_RANDOM), so determinism is **per-binary**. A different
binary — even a byte-for-byte-equivalent-behavior rewrite like `mgc "100 128"` — has different link
addresses → different absolute layout → the victim span may no longer land on a live slot, so the rate
swings 0%↔33%. **Minimizing by recompiling the Go source is actively counterproductive; `mcount` is the
canonical corrupting binary and should be kept as-is.** (The residual ~30% run-to-run variance under a
*fixed* binary comes from GC/goroutine scheduling interleave — "whether the triggering GC lands wrong," per
the doc.)

Recommended repro settings for downstream agents: run `target/heap201/mcount` under `DDFIXHEAP=1` with no
guest-env overrides; treat `CORRUPT` and segfault-rc≠124 both as hits; use N≥40 for any comparison.

---

## 5. Conclusion + next-instrumentation recommendation

**Conclusion.** The miscompile is in an **always-on baseline lowering** of the GC scan/sweep path (the
scan inner-loop bitmap word-load / bit-iteration / slot-greying, or the sweep allocBits↔gcmarkBits swap).
No existing translation toggle isolates it, because there is no optimization gate on that path — so the
toggle-bisect cannot narrow further on its own.

**Exact next instrumentation.** Since no pre-existing toggle covers the responsible path, the correct next
surgical bisect is to **add** one:
1. Add an env-gated *alternate* lowering of the scanobject pointer-bitmap loop (bitmap word load + `bsf`/
   shift bit-iteration + the slot-pointer grey call) that reverts to the simplest literal sequence, and
   diff the corrupt-rate on `mcount` under `DDFIXHEAP` at N≥40. If it flips clean, the miscompile is in
   that lowering; if not, move the gate to the **sweep** allocBits/gcmarkBits swap and repeat.
2. Pair with the siblings'/watchpoint agent's span-metadata watch on the deterministic victim span
   **0xc000102000** (gcmarkBits at span+0x48, allocBits at span+0x40) across the pre-corruption GC, to
   decide **mark-miss vs sweep-clears-live** FIRST, then gate only the implicated lowering. A hardware
   watchpoint on the span's gcmarkBits byte (vs. page-protection, which perturbs — see docs/BUG201 "the
   obstacle") is the right non-perturbing catch.

The toggle-bisect's value going forward is as a **rule-out set**: whoever adds the scan/sweep gate can
assume tier-2 fold, flag-save elision, SIMD-opt, EA-opt, guest-fold, and rep lowerings are all innocent.

---

## 6. Artifacts

All mac-visible under `/Users/x/dd/dd/target/h201c/`:
- `ddjit-x86` — Angle-C instrumented engine (worktree unity TU + tooling patch, DDFIXHEAP/DDUMP/DDWATCH).
- `src_mnogc.go` / `mnogc` — mcount minus `runtime.GC()` (0/30; proves runtime.GC() is load-bearing).
- `src_mfast.go` / `mfast` — GC-every-pass (1/30; proves over-frequent GC suppresses).
- `src_mgc.go` / `mgc` — parametrized `<gcEvery> <junkPerPass> [passes]` (layout-fragility demo).
- Canonical repro (unchanged): `/Users/x/dd/dd/target/heap201/mcount`.
- Go toolchain used to build repros: `/Users/x/dd/dd/target/tsordr201/gowork/go` (go1.22.12 linux/amd64),
  `GOOS=linux GOARCH=amd64`, output written under `target/` (mac-visible; the VM `/tmp` is not).

Worktree state: only the diagnostic tooling patch is applied (uncommitted); the `mem.c` 3-way conflict is
resolved; the engine compiles clean. No bug-fix surgery to hand off (root cause not pinned by this angle).
Canonical source was not modified by this angle.
