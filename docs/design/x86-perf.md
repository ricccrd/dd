# x86-perf — optimizing the jit86 translator toward native

**Subsystem #5** of `docs/PLAN.md` ("Optimize the x86 (jit86) translator toward native"). This is the
**x86-frontend-specific** design: lazy/elided EFLAGS synthesis and tighter SSE/x87 lowering. The
*shared-engine* wins — block chaining, IBTC, §B shadow-return, and the tier-2 trace optimizer — are
**inherited** once the engine dedup (#1) lifts the x86 frontend onto the shared `jit/` host engine; they
are not re-designed here (see `docs/design/engine-dedup.md` and `docs/design/tier2-optimizer.md` when
those land). This doc stands on its own for the parts the aarch64 frontend never needed, because the
aarch64 guest's flags *are* the host's flags and an x86 guest's are not.

All file/line references are to the tree as of this writing; treat them as anchors, not promises.

---

## 0. TL;DR

* The x86 frontend keeps an **ARM-NZCV substrate** in `cpu->nzcv` (`OFF_NZCV`, `include/cpu_x86_64.h:10`).
  After **every** flags-affecting op it spills that substrate to memory (`e_nzcv_save`,
  `frontend/x86_64/emit.c:70`); before **every** consumer (Jcc/SETcc/CMOVcc/ADC/SBB) it reloads it
  (`e_nzcv_load`, `emit.c:74`). That store+`mrs` / `ldr`+`msr` round-trip — **4 host instructions plus
  two NZCV system-register moves** — is emitted whether or not the flags are ever read, and the
  add/adc/logical variants (`e_nzcv_save_ci`/`_c1`/`_setcf`/`_keepC`, `emit.c:81–122`) add **3–5 more**.
* The substrate is *already* the right value: a `cmp`/`sub` leaves exactly the ARM NZCV a `b.cond` wants,
  and chained blocks (`emit_chain_exit`, `emit.c:345`) and IBTC hits (`emit_ibranch`, `emit.c:365`, which
  deliberately uses `sub` not `subs` to **preserve live NZCV**) never clobber the live flag register.
  So the spill+reload is **redundant in the common case** — the live ARM NZCV survives from producer to
  consumer with nothing in between.
* **Design:** defer flag materialization. Carry a translate-time **pending-flags** record (op kind +
  width + which operands/result reg are live), leave the raw ARM NZCV live, and at the consumer emit
  **only** the bits that consumer reads — mapping x86 condition → ARM condition directly off the live
  flags. Materialize the canonical `cpu->nzcv` only at a **block/dispatch boundary** (keeping the
  cross-block ABI byte-identical, so the change is intra-block and provably safe).
* This is **not** the "dead-NZCV-flag elimination" dud from `docs/OPTIMIZATIONS.md`. That dud removed a
  *native* flag-set the wide OoO core already hides. Here we remove **explicit `str`/`ldr` memory traffic
  and `mrs`/`msr` system-register moves** — real instructions with real latency and serialization that
  the core cannot hide. Different cost, different verdict. (§5 makes the argument in full.)
* **SSE** is already near-1:1 NEON (xmm0–15 → v0–v15). Two real warts: `pmovmskb` is a 48-instruction
  byte loop (`translate.c:1409`), and packed/scalar ops reload memory operands they could fold. **x87**
  is the big one: the ST stack lives in `cpu->st[]` and every `ST(i)` access recomputes `fptop` and does
  a memory load/store (`e_st_addr`, `emit.c:219`) — a one-instruction `fadd` becomes ~14 host
  instructions. Keep the stack top in NEON regs with a translate-time `fptop`.

---

## 1. Current cost — measured by reading the emitter

### 1.1 The flag substrate

`cpu->nzcv` holds an **ARM** flag word (N/Z/C/V in bits 31/30/29/28), not packed x86 EFLAGS. PF (parity)
and AF (aux-carry) are **not** modeled except for the FP-compare idiom (`x86cc_to_arm` maps x86 PF → ARM
V because `comis*`/`fcomi` leave V=1 on unordered, `translate.c:147–153`). So dd already avoids the most
expensive part of classic EFLAGS synthesis (PF popcount, AF nibble carry). The remaining cost is the
**save/reload round-trip and the carry-convention fixups**.

The **carry convention** (`emit.c:78–80`): `cpu->nzcv` stores the ARM **borrow** C (= `NOT` x86 CF),
which ARM `SUBS`/`SBCS` produce natively and which `x86cc_to_arm`'s table assumes. ARM `ADDS`/`ADCS`
produce C = x86 CF (the opposite), so an add/adc must **flip C** before storing (`e_nzcv_save_ci`); a
logical op must force C=1,V=0 (`e_nzcv_save_c1`); inc/dec must preserve the stored C (`e_nzcv_save_keepC`).

### 1.2 Per-op overhead, counted in host instructions

Producer side (`do_alu`, `translate.c:82–123`), in addition to the 1 essential `ADDS`/`SUBS`/`ANDS`:

| producer | helper | extra host insns | what they are |
|---|---|---|---|
| sub / cmp / sbb | `e_nzcv_save` | **2** | `mrs x20,nzcv` ; `str x20,[x28,#OFF_NZCV]` |
| add / adc | `e_nzcv_save_ci` | **5** | `mrs` ; `movz` ; `eor` ; `str` ; `msr` |
| and / or / xor / test | `e_nzcv_save_c1` | **7** | `mrs` ; `movz`;`bic` ; `movz`;`orr` ; `str` ; `msr` |
| inc / dec | `e_nzcv_save_keepC` | **7** | `mrs` ; `ldr` ; `movz`;`bic`;`and`;`orr` ; `str` ; `msr` |
| shl/shr/sar (imm, exact CF) | `e_nzcv_save_setcf` | **6** + setup | extract shifted-out bit, `bic`/`orr`/`str`/`msr` |
| byte/word width | as above **+** | **+3** | `lsl`,`lsl` operands into the high bits, then `lsr`+`bfi` merge |

Consumer side, every Jcc/SETcc/CMOVcc/fcmovcc (`translate.c:681, 1038, 1792, 1812/1816, 1831`):

| consumer | code | extra host insns |
|---|---|---|
| any | `e_nzcv_load` | **2** : `ldr x20,[x28,#OFF_NZCV]` ; `msr nzcv,x20` |

ADC/SBB are *both* — they `e_nzcv_load_ci`/`load` to get carry-in **and** `e_nzcv_save*` the result
(`do_alu` kinds 2/3, `translate.c:86–97`).

### 1.3 Worked examples (host instruction counts)

```
  x86            today                                   lazy (this design)
  ----           -----                                   ------------------
  cmp eax,ebx    subs wzr,w0,w1 ; mrs;str        (3)     subs wzr,w0,w1            (1)
  je   L         ldr;msr ; b.eq                  (3)     b.eq L                    (1)
                 ----------------------------- total 6   ------------------- total 2   (-67%)

  add rax,rcx    adds x0,x0,x1 ; mrs;movz;eor;str;msr (6) adds x0,x0,x1           (1)
  jne  L         ldr;msr ; b.ne                 (3)      b.ne L                   (1)
                 ----------------------------- total 9   ------------------- total 2   (-78%)

  add edi,eax    adds ... ; +5 flag insns       (6)      adds w7,w7,w0            (1)   <- flags dead
  and edi,0xff   ands ... ; +7 flag insns       (8)      ands w7,w7,#0xff         (1)
  jz   L         ldr;msr ; b.eq                 (3)      b.eq L                   (1)
                 ----------------------------- total 17  ------------------- total 3   (-82%)
```

The third example is **dead-flag elimination**: the `add`'s flags are overwritten by the `and` before any
read, so all 6 of its flag instructions vanish; the `and`'s flags reach a `jz` that reads only Z, so the
`and` needs *zero* fixup (Z is identical between ARM and x86 for a logical op).

### 1.4 How often are flags actually read?

This is the load-bearing empirical question and we cannot run here (shared mac bridge; design-only).
The DBT literature is consistent — QEMU's lazy `CC_OP`, Box64, HQEMU, and Pin all report that the large
majority of flag *definitions* are **dead** (typical figures 70–90%): compilers emit `add`/`sub`/`and`/
`inc` for address and counter math whose flags nobody reads, and only `cmp`/`test`-before-`jcc` and the
occasional `adc` chain consume them. dd should **instrument this directly** before committing to the full
build — see the counter in §7 (Phase 0). The first PR's gate does not depend on the exact number: even
the *non-dead* `cmp→jcc` fast path (§1.3 example 1) is a clean −67% with no liveness analysis at all.

---

## 2. Target model — what the aarch64 frontend does (and we should approach)

`frontend/aarch64/translate.c` mentions `nzcv` **once** (`:54`, for `ccmp`). It synthesizes nothing: a
guest `cmp` is a host `subs`, a guest `b.eq` is a host `b.eq`, the host NZCV *is* the guest NZCV. No
memory, no fixups. That is the asymptote. The x86 frontend can never be fully identity (x86's CF borrow
sense, the byte/word-in-high-bits trick, PF/AF corner cases), but for the **dominant `sub/cmp/add/and →
jcc/setcc/cmovcc`** paths it can get to **one host op per guest op** — the same as aarch64.

---

## 3. Lazy / elided flag synthesis

### 3.1 The pending-flags state machine

Add a translate-time (compile-time, per-block) struct — **no new guest state**, it never exists at
runtime:

```c
// lives in translate_block()'s frame; reset at block entry and after every materialize.
struct pflags {
    enum { PF_NONE, PF_SUB, PF_ADD, PF_ADC, PF_SBB, PF_LOGIC,
           PF_INCDEC, PF_SHIFT, PF_OTHER } op;   // producer class
    int   width;          // 1/2/4/8  (byte/word producers compute flags in the high bits)
    int   res_reg;        // host reg holding the result (for re-deriving CF/OF/PF if a rare consumer asks)
    int   a_reg, b_reg;   // operands, when cheap to keep (for exact CF/OF reconstruction)
    bool  live;           // true => the raw ARM NZCV register currently holds THIS op's flags
    bool  membank_valid;  // true => cpu->nzcv currently holds the normalized (borrow-convention) flags
};
```

**Transitions:**

* **Producer** (`do_alu` and the shift/inc/dec/mul sites): set `pf = {op, width, regs, live=true,
  membank_valid=false}`. Emit the bare `ADDS`/`SUBS`/`ANDS`/… and **stop** — no `mrs`, no `str`, no
  carry fixup. The byte/word high-bits trick (`translate.c:109–122`) stays; it is part of producing
  correct N/Z/V and is independent of materialization.
* **NZCV-clobbering instruction with no consumer in between** → the previous pending def is **dead**.
  Overwrite `pf` with the new producer; emit nothing for the old one. (This *is* dead-flag elimination,
  and it falls out of the state machine for free — see §3.4.)
* **Instruction that needs a scratch flag-set** (e.g. an internal `subs`/`adds`/`ands`, the `e_tst` in
  the shift path `translate.c:416`, or any helper that touches NZCV): if `pf.live`, first **materialize**
  to keep the flags recoverable (or note that this op overwrites a dead def). The emitter must declare
  whether it clobbers NZCV; a one-line `CLOBBERS_NZCV` annotation per emit helper drives this.
* **Block boundary** (Jcc tail, `jmp`, `call`/`bl`, `ret`/indirect, syscall, any `emit_exit_const` /
  `emit_spill` / `emit_chain_exit`): **materialize to membank** if `pf.live && !pf.membank_valid` (§3.3),
  so the spilled `cpu->nzcv` is in the canonical borrow convention the next block's prologue
  (`emit_prologue`, `emit.c:317`, does `e_nzcv_load`) and any non-chained entry expects. This single rule
  keeps the **cross-block flag ABI byte-identical to today** — the optimization is purely intra-block.

### 3.2 Consumer materialization — emit only the bits read

At a consumer we know exactly which EFLAGS bits it reads. Map (`pf.op`, x86 condition) → action:

**(a) N/Z/V-only conditions** — `je/jne` (Z), `js/jns` (N), `jo/jno` (V), and the signed
`jl/jge/jle/jg` (N,V,Z). These bits are **identical** between an ARM `ADDS` and `SUBS` and the x86 op of
the same class, *provided V is valid*. For sub/add/inc/dec, `pf.live` NZCV is directly usable — emit just
`b.cond` / `cset` / `csel`, **dropping `e_nzcv_load` entirely**. (V after a **logical** op needs the
clear — see (c).)

**(b) C-sense conditions** — `jb/jc` (CF), `jae/jnc` (!CF), `ja` (!CF&!ZF), `jbe` (CF|ZF), and CF *as a
value* (`setc`, `adc`/`sbb` carry-in, `rcl`/`rcr`, `lahf`/`pushf`). Here the producer class matters
because ARM C-sense differs:

| pf.op | live ARM C means | x86 CF = | jb/jc | jae/jnc | ja | jbe |
|---|---|---|---|---|---|---|
| `PF_SUB`/`PF_SBB` | borrow (= !CF) | `!C` | `b.lo` (CC) | `b.hs` (CS) | `b.hi` | `b.ls` |
| `PF_ADD`/`PF_ADC` | carry (= CF) | `C`  | `b.hs` (CS) | `b.lo` (CC) | — see ¹ | — see ¹ |
| `PF_LOGIC` | (stale) | `0` | never (CF=0): fold to const | always | `!ZF` → `b.ne` | `ZF` → `b.eq` |
| `PF_INCDEC` | (CF untouched) | prior CF | must read membank C (§3.3) | | | |

  ¹ For an **add** consumed by `ja`/`jbe` (the "C set OR Z set" shape, which has no single ARM cond when
  C=CF), the cheapest fix is a **one-time C flip** on the live flags right before the branch:
  `mrs;eor #1<<29;msr` (3 insns) → then the SUB-row condition applies. This is *only* emitted for the
  rare add→ja/jbe; the common add→jne/js needs nothing. (Most adds are consumed by N/Z conditions or not
  at all.)

  For **CF as a value** (adc/sbb/setc): `PF_SUB` → `cset w,cc` with the borrow-aware cond; `PF_ADD` →
  `cset w,cs`; `PF_LOGIC` → `mov w,#0`. For inc/dec, CF comes from the membank (§3.3).

**(c) Logical-op V/C** — x86 `AND/OR/XOR/TEST` set CF=0, OF=0. ARM `ANDS` sets N/Z but leaves C/V in a
state the signed conditions can't use, which is exactly why `e_nzcv_save_c1` force-sets C=1,V=0 today.
In the lazy model we emit that fixup **only if the consumer reads C or V**: `jl/jge/jle/jg` need V=0
(one `mrs;bic #1<<28;msr`, or better, fold: with OF=0, `jl`≡`js`→`b.mi`, `jge`≡`jns`→`b.pl`,
`jle`≡`ZF|SF`, `jg`≡`!ZF&!SF` — pure N/Z forms, **no V needed at all**). `je/jne/js/jns` need nothing.
So a logical op followed by *any* common consumer emits **zero** fixups — strictly better than today's
unconditional 7.

### 3.3 Materialize-to-membank (the boundary spill)

`materialize_to_membank()` is what today's `e_nzcv_save*` did, but called **once at a boundary** instead
of after every op. It converts the live raw ARM NZCV of `pf.op` into the canonical borrow-convention word
and `str`s it to `OFF_NZCV`:

* `PF_SUB`/`PF_SBB`/`PF_INCDEC`(C kept): `mrs;str` (the raw flags already are borrow convention; inc/dec
  additionally merges the kept C as `e_nzcv_save_keepC` does).
* `PF_ADD`/`PF_ADC`: `mrs; eor #1<<29; str` (flip C to borrow) — i.e. `e_nzcv_save_ci` minus the
  redundant `msr` (we are exiting; no live consumer follows).
* `PF_LOGIC`: write N/Z with C=1,V=0 (`e_nzcv_save_c1` body).

After it, `pf.membank_valid=true`. `emit_spill` (`emit.c:327`) calls it instead of `e_nzcv_save`. Because
the stored word is identical to today's, **every other block, the dispatcher, signal delivery
(`os/linux/signal.c`), and `ptrace`/coredump readers see the same `cpu->nzcv` they see now.** Zero ABI
change. This is the safety crux.

### 3.4 Dead-flag elimination — falls out for free

A pending def that is overwritten by another producer before any consumer reads it is **never
materialized** — the state machine simply replaces `pf`. No separate pass, no liveness lattice. This is
the cheap 80% of classic dead-flag elimination, scoped to a basic block, and it removes **memory traffic**
(the killer `str`), not just a hidden native flag-set. (Cross-block dead-flags — a def live out of a block
that the successor overwrites before reading — are left to tier-2's trace scope; not worth a CFG pass in
the frontend.)

### 3.5 Why this is safe where the aarch64 dud was not

`docs/OPTIMIZATIONS.md` lists "Dead-NZCV-flag elimination" as a ~1.0× dud and warns the cross-ISA-DBT
intuition doesn't transfer to Apple Silicon. That verdict is about the **aarch64** guest, where flags are
the host's flags: eliding a dead native `cmp` saves one instruction the 8-wide OoO core was already
hiding behind other work. **The x86 case is categorically different:** each x86 flag def today carries an
explicit `str` to `cpu->nzcv` and each use an explicit `ldr`+`msr`. Removing them removes (1) **store/load
µops and a real round-trip through L1** (a genuine dependency the consumer's `ldr;msr` serializes on), and
(2) **`mrs`/`msr NZCV` system-register moves**, which on Apple cores are not pipelined like ALU ops and
can serialize. We are not deleting hidden ALU work; we are deleting memory and system-register traffic.
That is squarely in `OPTIMIZATIONS.md`'s surviving category "do less work the runtime can observe / remove
the JIT's own overhead." The risk is **correctness**, not whether it helps — hence the matrix gate.

---

## 4. SSE / x87 tightening

### 4.1 SSE — already near-native, two real warts

xmm0–15 are pinned to v0–v15 (`emit_prologue` ldp/stp at `emit.c:318–319`), and the arithmetic/convert
opcodes lower 1:1 to NEON (`translate.c:1441–1483`: FADD/FMUL/FSUB/FDIV/FMIN/FMAX/FSQRT scalar and
packed; `0x2A`/`0x2C`/`0x2D` cvt; pack/unpack at `:1383–1408` → ZIP/SQXTN). Little to win in the hot FP
math itself. Targets:

1. **`pmovmskb` (0x D7, `translate.c:1409–1418`)** — currently spills 16 bytes to `cpu->mmscratch` and
   runs a **16-iteration `ldrb`+`ubfx`+`orr` loop ≈ 48 host instructions**. Replace with the standard
   branchless NEON reduction: mask each byte's sign bit, weight by a per-lane bit position, horizontal-add
   per 64-bit half (`USHR/AND` with a `{0x80…}` mask then `ADDV`/paired adds, or the `sshr #7` + `and`
   bit-position trick), `umov` two bytes to the GPR. **~8 instructions, no memory.** Highest-ROI SSE fix;
   `pmovmskb` is the core of every SSE `strlen`/`memchr`/`strcmp`, so it's hot in libc-bound code.
2. **Memory-operand folding** — packed/scalar ops with a memory source do `emit_ea`+`e_ldr_q/d/s` into
   v16 then operate (`:1447–1456`). NEON can't fold an arbitrary x86 EA, so the load stays, but the EA
   computation (`emit_ea`) is re-emitted per use; when the same `[base+disp]` feeds several ops in a
   block, cache the EA in a scratch. Minor; mostly subsumed by tier-2.
3. **Redundant `movaps`/`movdqa` reg→reg** (`0x28`/`0x6F`/`0x10` reg form) → already a single `e_vmov`;
   tier-2 copy-propagation will delete the survivors. Nothing frontend-specific to do.

### 4.2 x87 — the memory-resident stack is the real cost

`ST(0..7)` lives in `cpu->st[8]` (double precision) with `cpu->fptop` the top index
(`cpu_x86_64.h:29–30`). **Every** `ST(i)` touch goes through `e_st_addr` (`emit.c:219–224`): `ldr fptop`;
`add #i`; `and #7`; `add base`; `add base,idx,lsl#3` — **5 instructions of pointer math** — then a
`ldr_d`/`str_d`. So `fadd st,st(1)` (`translate.c:871–888`) is `e_fp_ld(18,0)` + `e_fp_ld(16,1)` + `FADD`
+ `e_fp_st(18,0)` ≈ **14 host instructions** for one arithmetic op, all of it address arithmetic and L1
traffic. Pushes/pops additionally `ldr/str fptop` (`e_fp_settop`, `emit.c:226`).

**Tightening — translate-time stack tracking:**

* Track `fptop` **statically** within a block. Almost all compiler-emitted x87 (and the `float-nbody`
  bench, `dd-tests/guests/bench/b_float.c`) has a **statically balanced** stack: each `fld`/`fild` push
  and `fstp`/`faddp` pop is visible at translate time, so the concrete slot index of every `ST(i)` is a
  compile-time constant. Maintain a `int st_depth` / rotation in the translator.
* Keep the top **N** stack slots (N≈4 covers essentially all real code) in **NEON callee-saved regs**
  v8–v15 (already spilled/filled by the prologue via `cpu->host_v`, `cpu_x86_64.h:16`, `emit.c` ldp/stp).
  With a static `fptop`, `ST(i)` resolves to a **fixed host vector register** — `fadd st,st(1)` becomes a
  single `FADD vX,vX,vY`, matching SSE. The `cpu->st[]` array becomes the **boundary spill home** only
  (materialize the modeled stack to `cpu->st[]` + write back `cpu->fptop` at any block exit / call /
  exception, exactly as flags materialize to `cpu->nzcv`).
* **Fallback:** if a block's `fptop` is *not* statically resolvable (indirect stack motion — rare:
  computed `fxch`, `ffree`, or a `fldcw`-driven path), fall back to the current memory model for that
  block. Same structure as the lazy-flags `live` bit.
* **`fcom`/`fcomi` → fnstsw/sahf idiom** (`e_fcom_setfpsw`, `emit.c:250`): already routes x87 compare
  through ARM `FCMP`+`cset` into `cpu->fpsw`, then `fnstsw;sahf` lands it in NZCV. With lazy flags, the
  `sahf` consumer (`translate.c:720`) and the FP-compare-into-NZCV path become a pending `PF_OTHER` that
  feeds the following `jcc`/`fcmovcc` directly off ARM `FCMP` flags — eliding the `fpsw` round-trip for
  the common `fcomi;jcc`. (`ucomisd`/`comisd` at `:1549–1560` already leave ARM FCMP flags in the
  substrate — make them pending producers so the following branch reads them live.)
* **80-bit `fld`/`fstp m80`** (`translate.c:817–826`) exits to a C helper (`R_X87FLD`/`R_X87FSTP`) — a
  full spill + `block_return` round-trip per long-double load/store. Genuinely rare (only actual
  `long double`); leave it. Double-precision ST modeling is the existing, accepted accuracy/▲speed
  trade (`cpu_x86_64.h:27`).

x87 is lower priority than flags+`pmovmskb` (modern code is SSE2, x87 is legacy/long-double), but
`b_float.c`-style code and old binaries hit it hard, so the static-stack win is worth a phase.

---

## 5. What's gated on the engine dedup (#1) vs standalone

**Standalone — needs nothing from #1, ship now against the current per-frontend engine:**

* Lazy flags §3 in its entirety. It is purely a property of `do_alu` + the consumer sites + the boundary
  `materialize_to_membank`, all inside `frontend/x86_64/{translate,emit}.c`. The cross-block ABI is held
  fixed (§3.3), so it composes with today's `emit_chain_exit`/`emit_ibranch`/`emit_spill` untouched.
* `pmovmskb` rewrite §4.1(1) and the x87 static-stack §4.2 — both frontend-local.

**Inherited from #1 (do NOT re-implement here):** block chaining, IBTC + monomorphic IC, §B
shadow-return + depth gate, LSE atomic upgrade — once the x86 frontend rides the shared
`jit/{cache,emit_arm64,dispatch}.c` (PLAN #1), it gets these for free. The x86 frontend already has *local*
chaining/IBTC (`emit_chain_exit`/`emit_ibranch`); dedup replaces them with the shared, more-optimized
versions and adds §B to x86. **Lazy flags must be #1-aware in one spot:** §B's push/classify and the IBTC
probe must not clobber NZCV between an x86 producer and consumer. The shared engine *already* guarantees
this for aarch64 (`OPTIMIZATIONS.md`: "NZCV must not be clobbered between a guest cmp and its branch …
use `tbnz`/`sub`+`cbnz`, never a flag-setting `cmp`") and `emit_ibranch` here already uses `sub` not
`subs` (`emit.c:379`). So the invariant lazy flags needs is the **same** invariant §B already maintains —
they compose. Verify it explicitly when #1 lands.

**Tier-2 (#4):** cross-block/trace register allocation will additionally keep `cpu->nzcv` (and the x87
stack, and EAs) **live in registers across trace edges**, eliding even the boundary materialization on a
hot trace, and will do cross-block dead-flag elimination. Lazy flags is the **enabling substrate**: once
flags are a translate-time pending record rather than mandatory memory, tier-2 can extend its lifetime
across trace edges trivially. Build lazy flags first; it makes tier-2's flag handling fall out.

---

## 6. Expected speedup

Honest framing, in the spirit of `OPTIMIZATIONS.md` (most classic DBT tricks die on this µarch; only
"less observable work / less JIT overhead" survives — this is the latter):

* **Flag-bound integer code** (`int-sieve`/`b_int.c`, `sha256`/`b_hash.c`, the `cmp/test/add/inc`-dense
  inner loops of `sqlite`): today every such op pays 2–7 extra host instructions plus an `str`, and every
  branch pays `ldr`+`msr`. Removing 4–9 host instructions from a 6–17-instruction sequence (§1.3) on the
  hottest ops should move these from their current JIT/native ratio toward the aarch64 frontend's ratio
  on the same shapes. Anticipate **mid-teens to ~30% on flag-dense loops**; smaller on memory-bound code.
  **State the number as a hypothesis to be measured by `bench`, not a promise.**
* **`pmovmskb` rewrite**: 48→~8 instructions on every SSE `strlen`/`memchr`/`strcmp`/`strcspn` —
  large local win on string-bound workloads (libc, parsers, `sqlite`'s tokenizer).
* **x87 static-stack**: ~14→~3 instructions per x87 arithmetic op on statically-balanced blocks; big on
  `b_float.c`-style and legacy code, ~0 on SSE2-only code.
* **Ceiling** (echoing `OPTIMIZATIONS.md`'s tier-2 note): compute-bound x86 tops out *near*, not *at*,
  native — the per-block boundary spill survives until tier-2 removes it. Lazy flags closes the
  *frontend-specific* gap; tier-2 closes the *engine* gap.

Measurement harness already exists: `dd-tests/src/bin/bench.rs` (`int-sieve`, `float-nbody`, `sha256`,
`sqlite`), times the *same* Linux binary under qemu-ref vs dd-jit. Add an x86 column; gate on ratio
improvement with **no** matrix regression.

---

## 7. Phased roadmap

### Phase 0 — instrument (no codegen change)
Add a translate-time counter: at each producer record a def; at each consumer mark the live def read;
at block end count un-read defs as dead. Dump aggregate `flag-defs / flag-reads / dead%` per benchmark.
Confirms the §1.4 dead-flag fraction on dd's actual corpus and sizes the prize. Cheap, throwaway-able.

### Phase 1 — **FIRST PR: the `sub/cmp → Jcc` fast path only** (smallest shippable slice)
Scope deliberately tiny and the highest-traffic shape:

* Introduce `struct pflags pf` in `translate_block` (`translate.c:156`), reset at entry.
* In `do_alu` (`translate.c:82`), for **`PF_SUB`/`PF_CMP` (kinds 5/7), width ∈ {4,8} only**: skip
  `e_nzcv_save`; set `pf={PF_SUB,width,live=true}`. (Leave add/adc/logical/inc/byte-width **exactly as
  today** — they still `e_nzcv_save*`; `pf` stays `PF_NONE` for them, and a `PF_NONE` consumer falls back
  to `e_nzcv_load`. Fully back-compatible, op-by-op.)
* At the rel8 and rel32 **Jcc** sites (`translate.c:673`, `:1786`): if `pf.live && pf.op==PF_SUB` and the
  ARM NZCV hasn't been clobbered since (it can't be — nothing between a `cmp` and its block-terminating
  `jcc` clobbers it; assert via the per-emit `CLOBBERS_NZCV` annotation), **drop `e_nzcv_load`** and emit
  `b.cond` straight off the live flags. Else keep `e_nzcv_load` (today's path).
* **Boundary:** the Jcc tail already calls `emit_chain_exit` for both edges. Insert
  `materialize_to_membank()` *before* the first chain exit so the spilled `cpu->nzcv` is canonical for the
  successor blocks. For `PF_SUB` that is just `mrs;str` — identical bytes to today, so the cross-block ABI
  is provably unchanged. (For this PR, since we only made `PF_SUB` lazy and a `cmp`'s result reg is `wzr`,
  the membank value equals what `e_nzcv_save` would have written.)
* **Gate:** `make test` cross-engine matrix stays **~236 green** (zero regression — the darwin/x86 rows
  are the relevant ones), `make coverage dynamic` shows 0 new `UNIMPL`, and `bench` shows the `cmp→jcc`
  loops (`int-sieve`, `sha256`) improve. Matrix-green is the merge gate.

This PR touches ~3 sites, changes no guest state, no cross-block ABI, and is trivially revertible. It
captures the §1.3-example-1 −67% on the single most common flag shape.

### Phase 2 — extend producers + N/Z/V consumers
Add `PF_ADD/PF_LOGIC/PF_INCDEC` producers and `SETcc`/`CMOVcc`/`fcmovcc` consumers; implement the full
producer→consumer table §3.2 (the two-column C-sense matrix and the logical-op fold-to-N/Z). Add byte/word
widths. Now dead-flag elimination (§3.4) is active across all ALU ops.

### Phase 3 — carry-value consumers + boundary completeness
ADC/SBB carry-in, `setc`, `rcl`/`rcr`, `lahf`/`sahf`/`pushf`/`popf` reading CF from the pending def or
membank. Audit **every** block-exit path (`emit_exit_const`, `emit_ibranch` hit/miss, syscall, the C-helper
exits `R_DIV`/`R_X87*`, signal entry) to ensure `materialize_to_membank` runs before any state escape.
This phase is where correctness is won or lost — pair it with a fuzz/differential run (the jit86
differential harness) against the amd64 oracle.

### Phase 4 — `pmovmskb` NEON reduction (§4.1.1)
Independent, parallelizable with Phases 1–3. Gate: matrix green + a `pmovmskb` differential vector test.

### Phase 5 — x87 static-stack (§4.2)
Translate-time `fptop`, top-N slots in v8–v15, boundary materialize to `cpu->st[]`, memory fallback for
non-static blocks. Gate: `float-nbody` + the x87 matrix rows green, `bench` shows the x87 improvement.

### Phase 6 — fold into engine dedup / tier-2
When #1 lands, re-verify the §B/IBTC "no-NZCV-clobber between producer and consumer" invariant holds for
x86 (it already holds structurally — `emit_ibranch` uses `sub` not `subs`). When #4 lands, extend pending
flags' lifetime across trace edges (drop the boundary materialize on hot traces) and add cross-block dead
elimination.

---

## 8. Risk register

* **Hidden NZCV clobbers** between a producer and a deferred consumer (a helper that emits `subs`/`adds`/
  `ands`/`tst` for its own purposes — e.g. the shift `e_tst` `translate.c:416`, address math, the
  byte-merge path). Mitigation: a `CLOBBERS_NZCV` flag on each emit helper; `pf.live` is forced false the
  moment a clobbering emit runs, falling back to `e_nzcv_load`/materialize. **Conservative by default.**
* **Cross-block flag liveness** (flags defined in block A, consumed in block B). Held safe by always
  materializing to membank at boundaries (§3.3); intra-block only. No CFG analysis in the frontend.
* **Signals / ptrace / coredump** reading `cpu->nzcv` mid-block: impossible — they only run at a block
  boundary (syscall/exception exit), where membank is already materialized. Audited in Phase 3.
* **C-sense table bugs** (the add-vs-sub borrow flip, the logical V-fold): the single most error-prone
  part. Covered by the differential oracle (every `setcc`/`jcc`/`cmovcc` × every producer class) before
  Phase 2/3 merge.
* **x87 static-stack mis-tracking** an indirect/unbalanced block: explicit memory fallback per block;
  never guess.

---

## 9. File / function index (anchors)

| concern | file:func / line |
|---|---|
| guest CPU state, `OFF_NZCV`, `st[]`, `fptop` | `dd-jit/src/runtime/include/cpu_x86_64.h:7–60` |
| width-correct ALU + flag save dispatch | `frontend/x86_64/translate.c:82 do_alu` |
| flag save/load + carry-convention helpers | `frontend/x86_64/emit.c:70–122 e_nzcv_*` |
| x86 cc → ARM cond table | `frontend/x86_64/translate.c:147 x86cc_to_arm` |
| Jcc rel8 / rel32 consumers | `translate.c:673`, `translate.c:1786` |
| SETcc / CMOVcc consumers | `translate.c:1804`, `translate.c:1823` |
| ADC/SBB carry-in/out | `translate.c:86–97 do_alu kinds 2/3` |
| LOCK rmw flags | `translate.c:129 lock_rmw` |
| shift exact-CF flag set | `translate.c:384–425`, `emit.c:103 e_nzcv_save_setcf` |
| inc/dec keep-C | `translate.c:507`, `emit.c:113 e_nzcv_save_keepC` |
| lahf/sahf | `translate.c:720–745` |
| x87 stack addressing | `emit.c:219 e_st_addr`, `:226 e_fp_settop`, `:235–247 e_fp_ld/st/push` |
| x87 arith / fcom | `translate.c:858–937`, `emit.c:250 e_fcom_setfpsw` |
| 80-bit m80 C-helper exit | `translate.c:817–826` (`R_X87FLD`/`R_X87FSTP`) |
| SSE arith/cvt/pack | `translate.c:1383–1560` |
| **pmovmskb byte loop** | `translate.c:1409–1418` |
| ucomisd/comisd → FCMP flags | `translate.c:1549–1560` |
| prologue / spill / exits / chaining / IBTC | `emit.c:315 emit_prologue`, `:327 emit_spill`, `:334 emit_exit_const`, `:345 emit_chain_exit`, `:365 emit_ibranch` |
| aarch64 native-NZCV reference | `frontend/aarch64/translate.c:54` (only nzcv mention) |
| bench harness (perf gate) | `dd-tests/src/bin/bench.rs` (`int-sieve`/`float-nbody`/`sha256`/`sqlite`) |
| dud note to reconcile | `docs/OPTIMIZATIONS.md` ("Duds — Dead-NZCV-flag elimination") |
