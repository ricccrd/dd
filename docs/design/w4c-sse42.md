# W4-C — SSE4.2 string ops + `rep cmps`/`rep scas` idioms

**One-line result.** Two new x86-frontend levers, both bit-exact vs a `qemu-x86_64` oracle:
(1) the **`rep cmps`/`rep scas` idiom** lowers the whole compare/scan to one host
`memcmp`/`memchr` round-trip — **36–42× faster** than the per-element loop and even **66–89×
faster than the qemu VM**, landing at native-arm speed; (2) a complete, bit-exact **SSE4.2
`pcmp{i,e}str{i,m}`** evaluator + a **CPUID `SSE42=1` experiment**. The experiment's verdict
is decisive and negative: **do NOT advertise SSE4.2** — it gives 0% win on
strlen/memchr/memcmp and a **14.8× regression on `strcmp`** (glibc switches to the `pcmpistri`
CISC mega-op, which no DBT lowering can make competitive with the already-fast SSE2 path).

Gates: **`NOREPCMP=1`** (rep cmps/scas → naive per-element oracle loop), **`SSE42=1`**
(advertise SSE3/SSSE3/SSE4.1/SSE4.2/POPCNT in CPUID). Patched tree builds clean
(`clang -O2`), aarch64 engine unaffected (PROF additions `#ifdef R_REPSTR`-guarded).

---

## 1. Ideas

The HP-Dynamo / LSE / Opt5-`rep movs` lever again: a DBT legitimately beats native/VM when it
**upgrades an ISA idiom to a host capability the static binary never used.** Two string-side idioms
were left open by Opt5 and W3-B:

1. **`rep cmpsb/cmpsd` + `rep[n]e scasb`** are the open-coded `memcmp`/`memchr`/`strlen`
   building blocks (musl's `memcmp.s`/`memmem`, hand-written asm, `gcc -minline-all-stringops`,
   and a lot of legacy/embedded code). Each was **UNIMPL → guest abort** in the baseline. Like
   Opt5 collapsed `rep movs/stos` to host `memcpy/memset`, collapse the whole
   (possibly REP/REPE/REPNE) scan to one host `memcmp`/`memchr`.

2. **SSE4.2 `pcmpistri`/`pcmpestri`/`pcmpistrm`/`pcmpestrm`** are the CISC string primitives glibc's
   IFUNC resolver selects for `strcmp`/`strcasecmp`/`strstr` **when CPUID advertises SSE4.2.** W3-B
   left them hidden ("currently zero reach"). The open research question: would *advertising* SSE4.2
   (so glibc picks the `pcmpistri` `strcmp`) be a net win over the SSE2 path?

3. **CPUID A/B** — only advertise what we implement correctly; measure both ways.

## 2. What's implemented (+ the x86 flag/pointer end-state for rep cmps/scas)

All via the engine's established **block-exit C-helper** pattern (identical to `cpuid`/`div`/`x87 m80`):
the translator emits a tiny descriptor store + `emit_exit_const(next, reason)`; the dispatcher calls a
C routine that does the whole operation on the `cpu` struct and resumes at `next`. One dispatcher
round-trip per *instruction* (not per *element*) — and the helper internally uses vectorized host
`memcmp`/`memchr`.

### 2a. `rep cmps` (A6/A7) + `rep scas` (AE/AF)  ·  `do_repstr()`  ·  gate `NOREPCMP=1`
Recognizes A6/A7/AE/AF (with/without F3/F2). Descriptor = `width | isscas | isrepne | isrep`.
The helper reproduces **exact x86 end-state**, validated against `qemu`:

- **Loop semantics (Intel REP pseudocode, exact):** execute one element (sets flags + advances
  pointers), `RCX--`, exit if `RCX==0`, else exit on the ZF condition (REPE stops at first
  *not-equal*; REPNE stops at first *equal*). A bare (non-REP) cmps/scas does exactly one step and
  ignores RCX.
- **Pointers:** forward (DF=0; `std` stays `report_unimpl`). For `k` elements consumed:
  `RCX -= k`; cmps does `RSI += k*w` **and** `RDI += k*w`; scas does `RDI += k*w` only.
- **`RCX==0` on entry (REP):** **no element executes — flags and pointers UNCHANGED** (the
  block-exit spill already saved the prior NZCV; the helper leaves it untouched).
- **Flags (ZF/SF/CF/OF), bit-exact:** the final element's compare `a-b` is converted to the
  engine's NZCV substrate by `repstr_nzcv()` in the **borrow convention** the rest of the engine
  uses (stored `C = NOT x86 CF`, exactly what `do_alu`/ARM `SUBS` produce): `N`=sign at width *w*,
  `Z`=(result==0), `C`=(a≥b unsigned), `V`=signed-overflow via the canonical
  `((a^b)&(a^r))>>(w-1)` formula (works at all widths incl. 64-bit — a naive INT64 form was the one
  bug found & fixed during bring-up, see §5). PF/AF are not modeled (neither is anywhere in the
  engine).
- **The lever (default path):** byte `rep cmps` (REPE) → `memcmp()` for the equality test, then a
  bounded scan to locate the mismatch byte; byte `rep[n]e scas` → `memchr()`; wider cmps →
  `memcmp(n*w)` + element-locate. `NOREPCMP=1` replaces all of it with the naive per-element loop
  (the oracle).

### 2b. SSE4.2 `pcmp{i,e}str{i,m}` (0F 3A 60–63)  ·  `do_pcmpistr()`  ·  reachable under `SSE42=1`
- **Decoder:** extended to the **3-byte opcode maps** `0F 38` / `0F 3A` (new `insn.esc` field;
  `0F 3A xx` carries an imm8). Previously the decoder read only one byte past `0F`.
- **Full evaluator (all control bytes):** all 4 aggregations (EQUAL_ANY / RANGES / EQUAL_EACH /
  EQUAL_ORDERED), signed/unsigned, byte/word, all polarities (positive / negative / masked-negative),
  least- vs most-significant index, both index (→ECX) and mask (→XMM0, bit-mask & element-mask)
  result forms, and implicit (`istr`) vs explicit-length (`estr`, lengths from EAX/EDX). operand2
  may be xmm or memory (staged into `cpu->mmscratch`). Flags CF/ZF/SF/OF packed into NZCV.
- **`pmuludq` (0F F4):** re-derived W3-B's 3-NEON-insn bonus fix (was UNIMPL → abort); needed by
  glibc's `strchr`/`memchr` byte-broadcast so the string battery can run at all. Strictly more
  correct (removes a guest abort).

### 2c. CPUID `SSE42=1` gate (`do_cpuid`)
`leaf 1 ecx |= SSE3|SSSE3|SSE4.1|SSE4.2|POPCNT` only when `getenv("SSE42")`, so glibc's resolver
selects `__strcmp_sse42` (confirmed: it uses `pcmpistri $0x1a`).

## 3. PoC — unified diff
`opt-reports/w4c-sse42.diff` (**7 files, +284/−100**). Core pieces:

- `include/cpu_x86_64.h`: `R_REPSTR`, `R_PCMPSTR` reason codes.
- `frontend/x86_64/engine_glue.c`: counters `g_repstr_n/_elems`, `g_pcmpistr_n`; `norepcmp()` gate.
- `frontend/x86_64/decode.c`: 3-byte `0F 38`/`0F 3A` escape (`insn.esc`) + imm8 sizing.
- `frontend/x86_64/translate.c`: recognizers for A6/A7/AE/AF → `R_REPSTR`; `0F 3A 60–63` →
  `R_PCMPSTR` (operand2 staged to `mmscratch`); `pmuludq` (0F F4).
- `frontend/x86_64/x86_ops.c`: `do_repstr()` + `repstr_nzcv()`; `do_pcmpistr()` evaluator; CPUID gate.
- `frontend/x86_64/dispatch.c`: dispatch `R_REPSTR`→`do_repstr`, `R_PCMPSTR`→`do_pcmpistr`.
- `os/linux/service.c`: x86-only PROF line (`repstr`/`repstr_elems`/`pcmpistr`), `#ifdef R_REPSTR`.

```c
// translate.c — rep cmps/scas recognizer (the whole scan in one round-trip):
if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
    int w = (op & 1) ? I.opsize : 1, isscas = (op == 0xAE || op == 0xAF), isrep = (I.rep || I.repne);
    uint64_t desc = w | (isscas<<8) | ((I.repne?1:0)<<9) | (isrep<<10);
    e_movconst(16, desc); e_str(16, 28, OFF_DIVOP);
    emit_exit_const(next, R_REPSTR); break;
}
// x86_ops.c — width-w (a-b) -> engine NZCV (borrow conv.), the canonical overflow form:
uint64_t V = (((ua ^ ub) & (ua ^ r)) >> (bits - 1)) & 1;   // works at all widths incl. 64-bit
```

## 4. Measurements

Apple-Silicon, median-of-5. JIT runs on macOS via the `mac` bridge; `qemu-x86_64` + native-arm run
on the orbstack arm64-Linux host (same silicon). Guests `x86_64-linux-gnu-gcc -O2 -static-pie`.

### 4a. `rep cmps`/`rep scas` idiom — explicit inline-asm microbench (`tests/repbench.c`, 1 MiB × 2000)
Canonical A/B = same binary, default (**idiom**) vs **`NOREPCMP=1`** (naive per-element loop).

| workload (1 MiB × 2000)            | IDIOM   | NOREPCMP (byte loop) | **speedup** | qemu VM | native-arm |
|------------------------------------|--------:|---------------------:|------------:|--------:|-----------:|
| `rep cmpsb` (memcmp, full-equal)   | 0.0420s | 1.5343s | **36.5×** | 3.752s (**89×**) | 0.0359s |
| `repne scasb` (strlen, scan to NUL)| 0.0367s | 1.5186s | **41.4×** | 2.412s (**66×**) | 0.0187s |
| `repne scasb` (memchr, byte absent)| 0.0359s | 1.5142s | **42.2×** | 2.421s (**67×**) | 0.0255s |
| `rep cmpsd` (dword memcmp)         | 0.0405s | 0.3621s | **8.9×**  | 0.912s (**22.5×**) | 0.0358s |

The idiom runs at **native-arm `memcmp`/`memchr` speed** (cmpsb 0.0420 vs native 0.0359 = within
17%) and **crushes the qemu VM 22–89×** — qemu microcodes the `rep` as a scalar loop and never
recognizes the idiom. This is the "beat-the-VM" lever, on the broadest possible primitive.

### 4b. CPUID SSE4.2 A/B (`tests/bench.c` glibc IFUNC microbench, 1 MiB × 300) — SSE2 vs `SSE42=1`

| glibc function | SSE2 (default) | SSE4.2 (`SSE42=1`) | ratio | what glibc selected under SSE4.2 |
|----------------|---------------:|-------------------:|------:|----------------------------------|
| `strlen`       | 0.0184s | 0.0195s | 1.06× slower | no SSE4.2 variant (stays `_sse2`) |
| `memchr`       | 0.0177s | 0.0189s | 1.07× slower | no SSE4.2 variant (stays `_sse2`) |
| `memcmp`       | 0.0197s | 0.0207s | 1.05× slower | `_sse4_1` ≈ same speed, no win |
| **`strcmp`**   | **0.0531s** | **0.7865s** | **14.8× SLOWER** | `__strcmp_sse42` = `pcmpistri $0x1a` |

`strcmp` under SSE4.2 fired **19,660,200 `pcmpistri` round-trips** (one per 16 bytes). Even an
optimal *inline*-NEON `pcmpistri` (≈30 host insns/16 B for the equal-each-negative strcmp control
byte: 3 `pmovmskb`-style reductions + length/index math) cannot beat the SSE2 `strcmp` loop (~12
host insns/16 B, and **even faster once W3-B's `pmovmskb`→NEON lands**). The block-exit number is an
extreme lower bound; the *direction* of the result is structural and unchangeable.

**Verdict: keep CPUID at SSE2-only.** Advertising SSE4.2 wins nothing on strlen/memchr/memcmp and
loses catastrophically on strcmp/strstr/strcasecmp.

## 5. Correctness matrix (bit-exact evidence)

Three-way: **`qemu-x86_64` (oracle) == JIT-default == JIT-`NOREPCMP=1`/`SSE42=1`**, all green.

| test (`tests/`)        | coverage                                                                 | result |
|------------------------|--------------------------------------------------------------------------|--------|
| `repcorr.c`            | repe/repne `cmpsb/cmpsl/cmpsq`, `scasb/scasl`, bare cmps/scas; lengths {0,1,…,4097}; aligns {0,1,2,3,7,8,15,16,4090} (page-cross); diff at every position; embedded-NUL; tail byte; flags via `setcc` (ZF/SF/CF/OF) | `qemu == JIT == NOREPCMP` = `417467b75997fa2b` |
| `strfns.c`             | glibc `strcmp/strncmp/strlen/memcmp/strchr/memchr/strstr` over align×len matrix, diff-at-every-position both directions | `qemu == SSE2 == SSE4.2` = `eef9a18e0eefe813` (66,562 `pcmpistri` fired) |
| `pcmptest.c`           | direct `pcmp{i,e}str{i,m}` intrinsics — all 4 aggregations, both polarities + masked-negative, byte/word, lsb/msb index, istri/istrm/estri/estrm | `qemu == JIT` = `4db82edf62279904` |
| `corr2_x86` (W3-B)     | memcmp + forward strchr (validates `pmuludq`)                              | `qemu == JIT` = `c852f60435eff27b` |

Edge cases explicitly covered & passing: empty (RCX=0 → flags/pointers unchanged), 1-byte,
page-crossing scans, mismatch at byte 0 / last byte / not-present, both directions of inequality
(SF/OF/CF), 64-bit `cmpsq` overflow (the one bug found — see below), strcmp prefix/length-mismatch
via embedded NUL, signed/unsigned & byte/word pcmp element ops.

**One bug found & fixed during bring-up:** the 64-bit `cmpsq` overflow flag was wrong (naive
`INT64_MIN` negation = UB). The first `repcorr` run was JIT-self-consistent (default==NOREPCMP) but
diverged from qemu **only on qword OF**; switching to the bitwise `((a^b)&(a^r))>>(bits-1)` form
fixed it and made all three byte-identical. (This is exactly why the qemu oracle matters — a
self-consistent JIT can still be wrong.)

**Firing/reach:** `repstr` fires on the explicit-asm bench but is **0 on the glibc battery** —
confirming W3-B's note that glibc/musl use the SSE2 (and `pcmpistri`) paths, not `rep cmps/scas`.
So the idiom's reach is hand-written-asm / legacy / `-minline-all-stringops` code, plus it removes
the prior guest-abort.

## 6. Risk

- **`rep cmps/scas` (low).** Exact x86 REP pseudocode; flag substrate reuses the engine's proven
  borrow convention; default fast path and `NOREPCMP` oracle are bit-identical across the full
  matrix and vs qemu. Block-exit reuses the battle-tested cpuid/div mechanism. DF=1 (`std`) still
  `report_unimpl` (unchanged). Adds previously-aborting opcodes → strictly more correct.
- **`pcmpistr*` evaluator (low-to-moderate, and gated off by default).** Unreachable unless
  `SSE42=1`; bit-exact vs qemu across the control-byte matrix. The decoder `0F 38`/`0F 3A` extension
  is additive (new `esc` field, falls through to existing paths). **Recommendation is NOT to ship
  the CPUID flip**, so the evaluator stays dormant — its only always-on effect is `pmuludq`, which
  is independently validated (`corr2_x86`).
- **Perf shape:** block-exit ends the block at each rep/pcmp instruction (one dispatcher round-trip
  per instruction). Negligible for the big-buffer idiom (one trip per whole scan); it is the
  *reason* SSE4.2-strcmp is slow (one trip per 16 B) — which only reinforces the "don't advertise"
  call. An inline `rep cmps/scas` is possible future work but unnecessary (already native-speed).
- **Shared `service.c`:** the only edit is the x86-only PROF line, `#ifdef R_REPSTR`-guarded so the
  aarch64 engine is untouched. Re-apply on integration alongside the other waves' PROF additions.

## 7. Recommendation

1. **SHIP the `rep cmps`/`rep scas` idiom** (gate `NOREPCMP=1`). Highest reach/lowest risk: native-arm
   speed, 36–42× over the element loop, 22–89× over qemu, bit-exact vs the oracle, removes a
   guest-abort. Composes cleanly with Opt5's `rep movs/stos` (same recognizer site, same mechanism).
2. **SHIP `pmuludq`** (0F F4) — 3 NEON insns, removes the `strchr`/`memchr` guest-abort, bit-exact
   (re-derives W3-B's bonus fix; include if W3-B itself isn't landing).
3. **DO NOT advertise SSE4.2 in CPUID.** Measured net-zero on strlen/memchr/memcmp and **14.8×
   worse on strcmp**; the `pcmpistri` CISC op cannot be lowered competitively against the SSE2 path
   (which W3-B already accelerates). Keep the `pcmpistr*` evaluator in-tree but **dormant** (gated
   `SSE42=1`) as a correctness reference / future-AVX2 groundwork — do not flip the gate.

## 8. Artifacts
- Report: `opt-reports/w4c-sse42.md`; diff: `opt-reports/w4c-sse42.diff` (7 files, +284/−100).
- Patched tree: `/Users/x/w4c-sse42` (builds clean; `ddjit-x86` signed). Mirror:
  `opt-work/w4c-sse42/` (src + tests + binary + `time5.py`).
- Tests: `tests/{repcorr,repdiag,repdiag2,repbench,repnat,strfns,pcmptest}.c`; rootfs guests in
  `/Users/x/w4c-sse42/rootfs/*_x86`.
