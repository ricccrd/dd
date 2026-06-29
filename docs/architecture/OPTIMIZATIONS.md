# Optimizations — engine internals

> Scope: this is the **core engine** optimization design (block chaining, IBTC, §B shadow-return,
> register-stealing). The wave-1–6 optimization **sweep** (20 measured opts: SSE→NEON, rep-string, lazy
> flags, traces, tier-2, inline syscalls, path/openat caches, pcache, fork-server, …) is **landed** and lives
> in [`/docs/PLAN.md`](../docs/PLAN.md) (remaining work) + [`/dd-daemon/PERFORMANCE.md`](../dd-daemon/PERFORMANCE.md)
> (developer-facing list). This doc is engine-internal reference, not published on the website.

The honest thesis: on a wide out-of-order Apple-Silicon core, **most classic DBT tricks do nothing** —
the µarch already hides them. The wins that survive measurement are (1) **use hardware the original
binary didn't** (LSE atomics, the hardware return-address stack), (2) **do less work the runtime can
observe** (cache syscall/stat results and branch targets), and (3) **remove the JIT's own overhead**
(block chaining, register stealing, depth-gated call wrappers). All numbers below are measured. The
optimizations live in `dd-jit/src/runtime/` (`jit/` + `frontend/aarch64/`).

## Block cache + direct chaining
Translated basic blocks live in one 64 MB W^X `MAP_JIT` arena (`jit/cache.c`) and are **chained**: a
block ending in a direct branch is backpatched to jump straight to the target block's host code — no
dispatcher round-trip. A guest-PC→host-block map + a pending-link table resolve forward references. A
hot loop becomes a tight chain of native code. The single most important structural optimization.

## IBTC + per-site monomorphic inline cache
Indirect branches (`br`/`ret`/`blr`) can't be statically chained (`jit/emit_arm64.c`). A shared **IBTC**
hash (guest target → host block) resolves them inline; the fallback red-zone-*saves* x16/x17 (it does
not assume them dead). Each site also caches its *last* target inline — a **monomorphic IC** — so a site
that always returns/jumps to one place hits a single compare-and-branch.

## §B — shadow-return prediction (the headline win + the depth gate)
A guest `ret` is an indirect branch, so the host CPU's **return-address stack** (which makes returns
free on real hardware) is never used. §B fixes that: steal the host link register x30 (guest x30 lives
in `cpu->x[30]`); translate a guest `bl` into a real host `bl` + push a `(guest_ret, host_ret, guest_sp)`
shadow frame; translate a guest `ret` into a classify — if the top frame matches, pop it and do a real
host `ret` the RAS has predicted; on any mismatch (longjmp, unwinding) fall back to the IBTC.

Correctness pitfalls found + fixed: **NZCV** must not be clobbered between a guest `cmp` and its branch
(push/classify use `tbnz`/`sub`+`cbnz`, never a flag-setting `cmp` — this was a multi-session
sort-corruption bug); the **guest SP** disambiguates coincidentally-equal return addresses; a **fork**
child resets its shadow stack (the host returns belong to the parent's frames).

**The depth gate** keeps §B from hurting leaf code: pushing a frame on every `bl` is pure overhead for a
leaf like `sqrt`. At each `bl`, `target_is_leaf()` statically scans the target — if it's a **leaf** or
**shallow** (all its calls go to leaves: `sin`/`pow` + their libm helpers), the `bl` uses the cheap path
(set x30, chain; the return hits the IC); only a target that calls a non-leaf, recurses, or calls
indirectly gets §B. The `ret` auto-adapts (no frame pushed → classify falls to the IC). Static, no
profiling. Measured (JIT/native, lower better):

| workload | §B every call | depth-gated |
|---|---|---|
| `floatk` (libm leaf) | 3.53× | **1.72×** |
| `recursion` | 2.47× | **1.16×** |
| `stringk` (nested libc) | 3.57× | **2.69×** |
| `qsortk` | 2.11× | **1.92×** |

## LSE atomic idiom upgrade
Baseline aarch64 binaries do atomics with an `ldxr`/`stxr` retry loop; Apple Silicon has single-
instruction **LSE** atomics. The translator recognizes the exclusive-loop idiom and emits the LSE
atomic. ~**2.3×** on atomic-heavy code, and it sidesteps livelock under contention.

## FS-metadata cache
Container workloads `stat()` the same paths relentlessly. `os/linux/fscache.c` memoizes
`stat`/`access`/`readlink` by path so repeats never cross to the host — up to ~**2500×** on lookup-bound
phases. Evicted on writes.

## x18/x28/x30 register stealing (vs spilling)
The engine takes **x28** (cpu pointer), **x30** (host link, for §B), **x18** (scratch — reserved by the
macOS ABI, so free). Only guest instructions that *use* a stolen register are mangled (spill two scratch
regs, load the guest value, rewrite, restore). **Do not** steal x9–x17 as "free scratch": they are not
reliably dead across a `bl` (crt/ld.so/`memcpy`/`setjmp` keep live values there) — a trivial static-PIE
segfaults. The two-register spill is the safe minimum.

## x86-64 extras (frontend/x86_64, jit86)
The x86 guest adds SSE (guest xmm → host v0–v15), an x87 ST stack emulated at double precision, and
flag synthesis (x86 EFLAGS computed lazily from an arm64 NZCV substrate).

## Tier-2 (planned)
The remaining JIT overhead is the per-block register spill/reload at every boundary. A trace optimizer
removes it: form hot traces over the `PROF` counters, keep guest regs in host regs across the trace
(spill only at side exits), inline the monomorphic comparator the call-IC identified (the sort/awk fix),
memoize pure hot regions via the purity gate. Honest ceiling: compute-bound code tops out *near* native.

## Duds — do NOT build (all ~1.0× on this µarch)
Dead-NZCV-flag elimination, µarch rescheduling, manual branchless conversion, naive prefetch on
dependent chains, single-load const-fold. The wide OoO core hides them; the cross-ISA-DBT intuition does
not transfer to Apple Silicon. (And dead-register §B scratch is *unsafe*, not just a dud.)
