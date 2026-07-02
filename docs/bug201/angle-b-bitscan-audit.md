# BUG #201 — Angle B: x86 bit-scan / allocCache / address-arithmetic differential audit

Status: **RULE-OUT (high confidence).** The tiny-allocator "next free slot" computation —
the bit-scan (BSF/BSR/TZCNT/LZCNT/POPCNT), the allocCache `SHR %cl` update + shift-by-64
guard, and the `result*elemsize + base` address arithmetic — is **value-and-flag correct in
isolation** under dd for every input pattern Go's allocator produces, including high (5 TB)
heap addresses. No source change was made. The fault therefore is **context / instruction-
ordering / register-EA-interaction dependent, not a wrong-value opcode lowering.**

This document is the exhaustive record of Angle B so the hardware-watchpoint agent and future
BUG201 work don't re-audit these paths.

---

## 0. Hypothesis under test (Angle B)

Go's inlined `nextFreeFast` (Go 1.22) computes the next free tiny-allocator object as:

```go
theBit := sys.TrailingZeros64(s.allocCache)   // amd64: BSFQ + CMOVEQ-64, OR TZCNTQ under BMI
if theBit < 64 {
    result := s.freeindex + uint16(theBit)
    if result < s.nelems {
        freeidx := result + 1
        if freeidx%64 == 0 && freeidx != s.nelems { return 0 }
        s.allocCache >>= uint(theBit + 1)          // SHRQ %cl + cmp/sbb/and shift-by-64 guard
        s.freeindex = freeidx
        s.allocCount++
        return gclinkptr(uintptr(result)*s.elemsize + s.base())  // IMULQ + ADDQ (or SHL for pow2)
    }
}
```

The BUG201 mechanism (converged, see `docs/BUG201.md`) is that on ~40% of x86 runs a
16-byte SIMD memclr (`movups %xmm15,(%r11)` → dd `str q15,[x17]`) zeroes a **live, marked**
tiny block, because the block ADDRESS in `r11` (dd EA `x17`) — produced by the above path —
points at a live slot. Angle B's suspicion: dd miscompiles the **bit-scan** (wrong result, or
wrong ZF-on-src==0 / CF semantics), which would make `theBit`, hence `result`, hence the
returned address, wrong in a value/bit-pattern-dependent way — matching the ASLR/seed
flakiness (the bitmap contents vary per run).

If the bit-scan is correct, the next suspects are the allocCache `SHR` update and the
`result*elemsize + base` arithmetic. All three were audited.

---

## 1. Per-instruction static audit vs Intel SDM

### 1.1 dd's flag (NZCV) convention — required to read the code

From `dd-jit/src/runtime/translate/x86_64/emit.c:118-164`:

- `cpu->nzcv` stores the **ARM NZCV**, where the carry bit is the **ARM borrow-C = NOT(x86 CF)**.
  ARM `SUBS`/`SBCS` produce this borrow-C naturally, and the jcc consumer table assumes it.
  Therefore **x86 CF = NOT(stored C bit29)**.
- `e_nzcv_save()` — `mrs x20,nzcv; str` — stores ARM N/Z/C/V verbatim (used after SUBS-shaped
  ops where the ARM C already IS the borrow convention).
- `e_nzcv_save_c1()` — logical finalizer: clears V (OF=0), **sets** stored C bit29 (→ x86 CF=0).
- `e_nzcv_save_setcf(cfreg)` — cfreg holds the desired **x86 CF** as 0/1; it stores
  `stored C = NOT(cfreg)` (so that x86 CF = NOT(stored C) = cfreg), keeping N/Z from live nzcv.
  Internally clobbers x20/x22/x23.

Scratch registers: guest regs are `x0..x15`; engine scratch is `x16..x27` (x16=value,
x17=EA, x20=nzcv scratch, x19/x22/x23 general scratch). So bit-scan scratch never aliases a
live guest register.

Emit helpers used (emit.c): `e_rbit` (RBIT, `0x5AC00000`/`0xDAC00000`), `e_clz` (CLZ,
`0x5AC01000`/`0xDAC01000`), `e_cset` (cset via csinc), `e_csel` (`csel Rd,Rn,Rm,cond` =
cond?Rn:Rm), `A_SUBS`/`A_ANDS` (flag-setting with dest `xzr`=reg31).

### 1.2 The lowering block

`dd-jit/src/runtime/translate/x86_64/translate.c` — a single block handles all five:
- **~2786-2835**: BSF/TZCNT (0F BC) and BSR/LZCNT (0F BD). The `F3` prefix (`I.rep`, local
  `cnt`) selects the BMI/ABM count form (TZCNT/LZCNT) vs the legacy bit-scan (BSF/BSR).
- **~2836-2860**: POPCNT (F3 0F B8).

Key structural details in the code:
- `rm_load(&I, next, I.opsize, &mem)` returns the source (x16 for a memory operand, else the
  guest reg). `sf` = 64-bit operand flag.
- **dest==src clobber guard** (2799-2803): if `!mem && I.reg == rmv`, the source is copied to
  x23 *before* the dest is written, so the later flag read sees the true source. This is the
  `bsf %edx,%edx` form Go's `bytealg.IndexByteString` emits, and (mirrored) `popcnt %rax,%rax`.
- **Legacy vs count write target** (2808): `bdst = cnt ? I.reg : 22`. Legacy BSF/BSR compute
  the index into scratch **x22** and `csel` it into the dest only when src!=0 (so the dest is
  left UNCHANGED on src==0 — real-hardware behavior glibc `memrchr` relies on). TZCNT/LZCNT
  write the dest **unconditionally** (they DEFINE src==0 → operand size).

### 1.3 The audit table

Convention checks below use: x86 CF = NOT(stored C); stored Z = ARM Z.

| Insn | Opcode | Value lowering (translate.c) | ZF | CF | SDM verdict |
|---|---|---|---|---|---|
| **BSF**  | 0F BC, no F3 | `e_rbit(22,src); e_clz(22,22)` → x22 = trailing-zero index; `e_csel(reg,reg,22,EQ)` keeps dest if src==0, else index (2809-2811, 2830) | `ANDS xzr,src,src` → Z=(src==0) ✓ (2829); `e_nzcv_save()` stores it | undefined (SDM) — not consumed | **CORRECT** |
| **TZCNT**| F3 0F BC | `e_rbit(reg,src); e_clz(reg,reg)` → count; src==0 → CLZ(0)=width ✓ (2810-2811) | `ANDS xzr,I.reg,I.reg` → Z=(result==0) ✓ (2826) | `SUBS xzr,src,xzr; cset x19,EQ` → x19=(src==0); `e_nzcv_save_setcf(19)` → x86 CF=(src==0) ✓ (2824-2827) | **CORRECT** |
| **BSR**  | 0F BD, no F3 | `e_clz(20,src); movconst(19,width-1); SUB x22 = x19 - x20` → highest-set index; `csel` keeps dest if src==0 (2814-2821, 2830). Uses **x20** (not x16) so a memory src in x16 is not clobbered before the ZF read | `ANDS xzr,src,src` → Z=(src==0) ✓ (2829) | undefined | **CORRECT** |
| **LZCNT**| F3 0F BD | `e_clz(I.reg,src)` → leading-zero count; src==0 → width ✓ (2812-2813) | `ANDS xzr,I.reg,I.reg` → Z=(result==0) ✓ (2826) | `cset x19,EQ` + `save_setcf` → x86 CF=(src==0) ✓ | **CORRECT** |
| **POPCNT**| F3 0F B8 | NEON: `fmov s/d16,src` (zeroes upper lanes) → `cnt v16.8b` → `addv b16` → `fmov` to dest (2848-2855) | `ANDS xzr,src,src` → Z=(src==0) ✓ (2856) | `e_nzcv_save_c1()` → CF=0, OF=0 ✓ (2857) | **CORRECT** |

SDM semantics confirmed against:
- **BSF/BSR**: ZF=1 iff src==0; dest undefined when src==0 (real HW leaves it unchanged — dd
  matches via csel). Other flags undefined. ✓
- **TZCNT**: dest = trailing-zero count, src==0 → operand size; **CF = (src==0)**,
  **ZF = (dest==0)**; OF/SF/PF/AF undefined. ✓
- **LZCNT**: dest = leading-zero count, src==0 → operand size; **CF = (src==0)**,
  **ZF = (dest==0)**. ✓ (Note dd correctly keeps LZCNT distinct from BSR: LZCNT=CLZ,
  BSR=(width-1)-CLZ — the comment at 2787-2789 flags this as a past corruption source.)
- **POPCNT**: dest = popcount; **ZF = (src==0)**; CF=OF=SF=AF=PF=0. ✓

**32-bit forms**: value writes go to the W register, which zero-extends bits[63:32] of the X
register — matching x86's 32-bit-dest zero-extension. The BSF src==0 32-bit case re-writes
`I.reg` from itself via a 32-bit csel, which also zero-extends; consistent with x86 (low 32
kept, upper zeroed).

**Why the ZF path is load-bearing and still correct:** default `GOAMD64=v1` lowers
`sys.TrailingZeros64` to `BSFQ` followed by `CMOVEQ $64` — i.e. the CMOV *reads ZF* to
substitute 64 when src==0. dd's BSF sets ZF=(src==0) via `ANDS xzr,src,src`, so the CMOVEQ
selects correctly. This is exercised empirically below (allocCache==0 → theBit=64).

**Conclusion of static audit:** all five instructions are correct in value and in every flag
x86/Go consumes. No divergence from SDM.

---

## 2. Empirical differential binaries

Host is aarch64 (OrbStack Linux); it has `x86_64-linux-gnu-gcc` and runs x86-64 ELF via
binfmt/qemu, which serves as the **native oracle**. The dd engine was built standalone from
this worktree (never touching canonical):

```
WT=/Users/x/dd/dd/.claude/worktrees/agent-a8118bc6d0d67171a
O=$WT/target/heap201angleB
mac bash -lc "clang -O2 -o $O/ddjit-x86 $WT/dd-jit/src/runtime/targets/linux_x86_64.c \
  && codesign -s - --entitlements $WT/dd-jit/jit.entitlements -f $O/ddjit-x86"
```

Bug reproduces in this engine: `mcount` random-ASLR corrupt rate ≈ 5/20 (25%), consistent
with the documented ~40% ballpark — so a clean result here is a meaningful rule-out.

All test binaries are **self-checking**: each executes the guest instruction via inline asm
(capturing dest and, via `pushfq`, ZF/CF) and compares against a from-scratch software
reference computed without the instruction under test. `bad=0` means dd == reference == real
x86. Each was validated on the native oracle first (`bad=0`), then run under dd.

### 2.1 `flagchk` (new, instruction-level value + ZF/CF)

Source: `target/heap201angleB/flagchk.c`. Tests BSFQ, BSRQ, TZCNTQ, LZCNTQ, POPCNTQ, BSFL,
TZCNTL. For each it captures dest and the real ZF/CF via `pushfq; pop`, and checks:
- BSF/BSR: value = index (unchanged sentinel `0xDEADBEEFCAFEBABE` when src==0), ZF=(src==0).
- TZCNT/LZCNT: value (src==0 → operand size), ZF=(result==0), CF=(src==0).
- POPCNT: value, ZF=(src==0), CF=0.

Bit patterns (exactly the shapes Go's allocCache takes):
- `0, 1, 2, 0x5555555555555555, 0xAAAAAAAAAAAAAAAA, 0xFFFFFFFFFFFFFFFF,
  0x8000000000000000, 0x100000000, 0x5554, 0x123456789ABCDEF0`
- **single bit at every position** `1<<i` for i=0..63 (and 32-bit)
- **shifted alternating** `0x5555555555555555 >> i` for i=0..63 (includes ==0 at i=63)
- the **nextFreeFast walk**: start `0x5555…`, `m &= m-1` (clear lowest set bit) each step,
  TZCNT the residual — 32 steps.

```
x86_64-linux-gnu-gcc -O2 -static -mbmi -mlzcnt -mpopcnt -o $O/flagchk $O/flagchk.c
$O/flagchk                                   # native oracle
mac bash -lc "exec env $O/ddjit-x86 $O/flagchk"   # under dd
```
Result: native `flagchk done: bad=0 walksteps=32`; **dd `bad=0`** (deterministic over 3 runs).

### 2.2 `addrchk` (new, address arithmetic)

Source: `target/heap201angleB/addrchk.c`. Exercises `addr = base + (freeindex+theBit)*elemsize`
the way nextFreeFast lowers it — `IMULQ index,elemsize` then `ADDQ base,off`, plus the tiny
allocator's `SHLQ $4` (elemsize 16) and a `LEA` form — over **high heap addresses**:
- bases: `0x0000050000000000` (the 5 TB DDFIXHEAP mmap region), `0x00007f0000000000`,
  `0x0000c000abcd0000`, `0xc000000000`, `0x200000000` (the DDFIXHEAP non-PIE bias),
  `0xdeadbeef000`, `0x7ffffffff000`
- elemsizes: all Go size classes `8,16,24,…,208,256,512,1024,8192`
- idx 0..600 (spans past 512 nelems)
- plus a `freeindex + theBit` ADD sweep (fi 0..520, tb 0..63) across the 64-word allocCache
  boundary.

Reference multiply is a shift-add loop (independent of IMUL). Checks IMUL low-64 value and the
64-bit ADD carry into the high heap bits (the "no truncation for high heap addrs" property).

```
x86_64-linux-gnu-gcc -O2 -static -o $O/addrchk $O/addrchk.c
$O/addrchk ; mac bash -lc "exec env $O/ddjit-x86 $O/addrchk"
```
Result: native `addrchk done: bad=0`; **dd `bad=0`**.

### 2.3 `bitchk` (prior agent, Go-level TrailingZeros64 + BTC)

Source: `target/heap201/src_bitchk.go`. `bits.TrailingZeros64` (→ BSFQ+CMOVEQ-64, exercising
the **ZF→CMOV** path incl. src==0 → 64 when `0x5555…>>63 == 0`), a BTC toggle-vs-clear
identity, and the nextFreeFast clear-lowest-bit walk. Run under dd:
```
mac bash -lc "exec env GOMAXPROCS=1 $O/ddjit-x86 /Users/x/dd/dd/target/heap201/bitchk"
```
Result: **dd `done bad=0 loopsteps=32`.**

### 2.4 `shiftchk2` (prior agent, allocCache SHR + shift-by-64 guard) — and the shiftchk gotcha

The allocCache update `s.allocCache >>= uint(theBit+1)` compiles to `SHRQ %cl` plus Go's
shift-by-≥64 guard (`cmp $0x40; sbb reg,reg; and`), because Go defines a shift count ≥ width
as 0, whereas raw `SHR %cl` masks the count to 63.

- `target/heap201/shiftchk2` tests the **Go-level** `x<<s` / `(1<<s)-1` (which IS that
  cmp/sbb/and+shift idiom) for s ∈ {0,1,32,63,64,65,127,128}: under dd **ALL OK** — dd
  reproduces Go semantics exactly (s≥64 → 0), which validates the SBB-based guard end-to-end.
- ⚠️ **`shiftchk` broken-oracle gotcha:** the sibling `shiftchk` reports `done bad=23`, but
  that is a **bad oracle, not a dd bug.** Its reference `oshl/oshr` do `s &= 63` first (they
  emulate *hardware* masking), so for s=64/65 the oracle yields a rotate-like non-zero value
  while dd correctly yields 0 (Go semantics). The authoritative test is `shiftchk2`, which is
  ALL OK. Do not chase the `shiftchk bad=23`.

---

## 3. Conclusion

Every value-producing element of the tiny-allocator's next-free computation is **correct in
isolation under dd**, verified both by SDM cross-read and by self-checking native-oracle-vs-dd
differentials over the exact input space Go's allocCache spans:

- **Bit-scans** (BSF/BSR/TZCNT/LZCNT/POPCNT): value + ZF/CF correct for zero, single-bit at
  all 64 positions, high-bit-only, full, `0x5555`/`0xAAAA`, and the nextFreeFast walk. `bad=0`.
- **allocCache `SHR %cl` + shift-by-64 guard**: matches Go semantics exactly (`bad=0` /
  ALL OK). The `cmp/sbb/and` guard is validated.
- **Address arithmetic** `result*elemsize + base` (IMULQ, ADDQ, SHLQ $4, LEA): correct over
  the 5 TB DDFIXHEAP region and all size classes, no high-bit truncation. `bad=0`.

Therefore the BUG201 wrong-destination memclr is **NOT** a wrong-value opcode lowering in this
path. This is fully consistent with `docs/BUG201.md`: "each computation verifies correct on
non-corrupting runs." The fault is **context / instruction-ordering / register-EA-interaction
dependent** — it emerges only from mallocgc's *specific* surrounding instruction stream, not
from any instruction's isolated semantics.

### Surviving suspects (redirect for the hardware-watchpoint / per-alloc-trace agents)

1. **Scratch-register / EA-optimization fold interaction.** The isolated tests use gcc-chosen
   register allocations; the real bug may need the exact register combination and instruction
   adjacency mallocgc emits (e.g. an EA-opt fold, `NOEAOPT` was ruled out globally but not in
   this precise stream, or a scratch x22/x23/x20 lifetime overlap with a peephole-merged
   neighbor). This is the highest-probability remaining class.
2. **The tiny-bump path** `c.tiny + round(c.tinyoffset, align)` (as opposed to nextFreeFast).
   Its alignment-rounding AND/ADD masking was **NOT** in BUG201.md's verified list and was
   **NOT** exercised by Angle B. If the block address sometimes comes from the bump rather than
   nextFreeFast, audit the round-up (`(off + a-1) &^ (a-1)`) and the `c.tiny+off` add.
3. **Cross-block flag/state residual** — a stale flag or a spilled nzcv carried across a
   translation-block boundary into a conditional the allocator keys on. (#145 lists historical
   x86 bsf/imul/shift flag bugs; none reproduced here in isolation, but a cross-block spill is
   not covered by single-instruction tests.)

The per-alloc-trace instrument (first divergent returned block address, non-perturbing) remains
the right next step; Angle B's data predicts it will diverge at an instruction whose *isolated*
semantics are correct — i.e. the divergence is in register/EA interaction or the tiny-bump, not
in the scan, the shift, or the multiply.

---

## 4. Artifacts

Under `/Users/x/dd/dd/.claude/worktrees/agent-a8118bc6d0d67171a/target/heap201angleB/`
(worktree-local; regenerate if the worktree is reaped):
- `ddjit-x86` — standalone x86 engine built from this worktree's unity TU (bug reproduces:
  ~25% corrupt on random-ASLR `mcount`).
- `flagchk.c`, `flagchk` — instruction-level BSF/BSR/TZCNT/LZCNT/POPCNT value + ZF/CF test.
- `addrchk.c`, `addrchk` — `base + index*elemsize` IMUL/ADD/SHL/LEA test over high heap bases.

Prior-agent binaries reused (under `/Users/x/dd/dd/target/heap201/`):
- `bitchk` (+ `src_bitchk.go`) — Go-level TrailingZeros64 / BTC.
- `shiftchk2` (+ `src_shiftchk2.go`) — allocCache SHR + shift-by-64 guard (authoritative).
- `shiftchk` (+ `src_shiftchk.go`) — ⚠️ broken oracle (masks to 63); ignore its `bad=23`.
- `verify.sh` — corrupt-rate harness.

No canonical source was modified; no commits.
