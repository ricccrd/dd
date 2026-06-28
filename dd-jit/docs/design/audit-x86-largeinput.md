# Static audit — x86 frontend vs. the `busybox sort` LARGE-input SIGSEGV

Scope: read-only audit of `runtime/frontend/x86_64/{translate.c,emit.c,dispatch.c}`
(+ `decode.c`/`emit_ea`, `elf.c`), cross-checking the differential-tracing agent.
Baseline bug: `busybox sort` on large input → **exit 139 (SIGSEGV)**; same class as
`busybox ls -lR /`; identical on base + every opt build (PLAN.md:145-163). Date 2026-06-27.

## Bottom line (the cross-check)

**The instruction-translation classes the differential agent is tracing are 64-bit-clean
and correct for the forward/common path.** I found no mistranslation in string ops, SSE
bulk ops, SIB/RIP addressing, stack/RSP, or the flag substrate that produces a *wild
address* that scales with input. The few real gaps in these files **abort cleanly
(`report_unimpl` → reason 99 → exit 70), they do not SIGSEGV.**

A SIGSEGV (exit 139) that scales with *data* size and hits both `sort` (heap) and
`ls -lR` (recursion/stack) is therefore almost certainly **not** an opcode mistranslation.
It is the **guest memory model in `elf.c` / `linux_x86_64.c`**: fixed-size stack/heap plus a
**global, monotonic, never-reset lazy-fault budget** that large inputs exhaust → the next
legitimate fault is re-raised as a fatal signal. Details + repro below. **Tell the
differential agent: the faulting host instruction will be a perfectly-correct guest
load/store to an address the runtime failed to keep mapped — look at the fault address vs.
the stack/heap region bounds and `g_lazymaps`, not at the opcode.**

---

## Per-class verdict (the audited files)

### 1. String ops `rep movs/stos/lods` (A4/A5, AA/AB, AC/AD) — CORRECT (forward)
`translate.c:635-666`. The rep COUNT is handled at **full 64-bit**: the loop is
`cbz x_rcx,done / body / sub x_rcx,#1 (sf=1) / b top`, with a 64-bit `CBZ` and a 64-bit
`SUBS`-free `SUB` on RCX. Pointers `RSI/RDI` advance via `e_addi(.,.,w,1)` with `sf=1`
(64-bit). Per-element size `w` is correct (`(op&1)?opsize:1`, REX.W→8). No counter/temp
overflow; the emitted loop iterates at runtime and cannot run off the end if the guest's
RCX is correct. No code-size growth with input.
- **Gap (aborts, not segv):** `0xA6/A7 cmps` and `0xAE/AF scas` are **unimplemented**;
  `std`/DF=1 (`0xFD`) is **unimplemented** (`translate.c:671-674`) and forward movs/stos
  *assume DF=0* (`0xFC cld` is a nop, `:667`). musl's `memmove.s` uses `std;rep movsb;cld`
  for overlapping backward copies — if `sort`/`qsort` element moves hit that, we
  `report_unimpl` → **exit 70, not 139**. Consistent (DF can only be set via std/popf, both
  of which abort), so we never silently run a wrong-direction copy.

### 2. SSE/AVX bulk (movups/movdqu 0x10/0x6F, pcmpeqb 0x74, pmovmskb 0xD7) — CORRECT
`translate.c:1144-1604`. 128-bit loads/stores use `e_ldr_q/e_str_q` (`emit.c:191-192`),
which are ARM `LDR/STR Q` — **unaligned-safe** on normal memory, base = EA in x17 (full
64-bit `base+index*scale+disp`). `pcmpeqb` → `CMEQ.16b` (`:1358`), correct.
- **`pmovmskb` (`:1446-1455`) is CORRECT** despite the "48→8 NEON" note flagged elsewhere:
  this is a *scalar* 16-iteration implementation (spill xmm to `OFF_MM`, `ldrb`+`ubfx
  #7,#1`+`orr lsl#i`), `e_movz` zeroes the dest first. It yields the exact 16-bit mask with
  upper bits cleared. A wrong strlen/memchr length cannot come from here. (There is only one
  `0xD7` impl in the tree — grep confirms no buggy duplicate.)
- **Gap (aborts, not segv):** `movntdq` (`66 0F E7`) is **unimplemented** → falls through to
  `report_unimpl` (exit 70). Not the segv.

### 3. Addressing (SIB / RIP-rel / fs-gs / 0x67) — CORRECT, fully 64-bit
`decode.c:169-252`, `emit_ea` at `decode.c:234`. `base` (`e_mov_rr sf=1`),
`index<<scale` (`e_rrr A_ADD ... lsl#scale`, 64-bit, full index reg), `disp`
(sign-extended int32 → `e_movconst` 64-bit add), RIP-rel (`next_rip+disp` as a 64-bit
constant), and fs/gs base (`OFF_FS/OFF_GS` add) are **all 64-bit, no truncation**. Correct
even at PIE-high (`base≈0x1_0xxxxxxx > 4 GB`) addresses — the stack-canary `%fs:0x28` idiom
and GOT `lea [rip+x]` work at high addresses.
- **Latent gap (NOT this bug):** the `0x67` address-size prefix is decoded
  (`I->addr32`, `decode.c:116`) but **never applied in `emit_ea`** — a 32-bit EA is not
  truncated to 32 bits. Wrong for any `addr32` memory op, but `busybox sort` does not emit
  `0x67`, so it does not cause this crash. Worth fixing opportunistically.

### 4. Stack / large frames — CORRECT, RSP stays 64-bit
`push/pop` (`translate.c:264-277`), `call/ret` (`:681-738`), `leave` (`:741`) all use
`e_subi/e_addi(RSP,RSP,8,sf=1)`. `sub rsp,imm32` and `and rsp,-16` go through group1/`do_alu`
width-8 (`:327`, `e_movconst`+64-bit `SUBS`/`ANDS`) — no 12-bit-immediate truncation, no
32-bit truncation of RSP. Prologue/spill LDP/STP offsets (`OFF_V=400`, max `+224` → field
`39`) are inside the signed imm7 range, so they are not a hidden corruptor.

### 5. Flags driving sort comparisons — CORRECT substrate
`do_alu` (`translate.c:91-135`) operates byte/word cmp/sub in the high bits (`<<sh`) so ARM
NZCV matches x86 byte/word flags exactly; the borrow-convention C and the lazy
`g_pf_sub_live` path (PR1) materialize identically at any non-Jcc/boundary
(`translate.c:174-200, 708-718`). The bug **predates** PR1 (PLAN.md:148) and a wrong
comparison branch in `qsort`/`strcmp` yields wrong *order*, not a wild pointer — it would
not SIGSEGV. No finding here.

---

## TOP SUSPECT (cross-checks the differential trace)

### #1 — Finite, never-reset lazy-fault budget + fixed stack/heap  (`elf.c` / `linux_x86_64.c`)
This is the only mechanism I found that (a) produces a real **SIGSEGV (exit 139)**, (b)
**scales with input/recursion size**, (c) explains the **threshold** behaviour, and (d) hits
**both** `sort` and `ls -lR` and is **identical on base+opt** (it is runtime, not codegen).

- The guest **stack is a fixed 8 MB** mmap with the guard mapped *above* the top for
  over-reads, **none below** (`elf.c:97-103`, `SZ = 8<<20`, `GUARD = 0x10000`). Deep
  recursion (`ls -lR /`) or large stack buffers grow *down past* `stk` into unmapped space.
- The guest **brk heap is a fixed 256 MB** mmap (`linux_x86_64.c:216-218`,
  `brk_hi = brk_lo + (256u<<20)`).
- The default SIGSEGV handler is **`jit86_lazyguard`** (`elf.c:176-195`), which maps the
  faulting page (so SSE over-reads and stack-growth both get papered over) **but only while
  a single global atomic counter `g_lazymaps < 4096`** (`elf.c:175,179`). The counter is
  **monotonic and never reset** for the life of the process.
- **Consequence:** every distinct page lazily faulted in — vectorized over-reads past the
  ends of mmap-backed buffers, *and* stack pages below the 8 MB region — permanently
  consumes one of 4096 (≈16 MB) global credits. A **small** input stays under budget and
  works; a **large** input (2M lines / deep `ls -lR`) touches > 4096 distinct guard pages,
  and on exhaustion `jit86_lazyguard` does `signal(sig,SIG_DFL); raise(sig)` → **fatal
  SIGSEGV, exit 139** (`elf.c:193-194`). If a stack write below `stk` is what overflows the
  budget, the fault address will be just below the stack region; if a heap over-read, it
  will be one page past an mmap chunk — in both cases a *correctly-translated* guest store.

**Why it looks like an "engine/state bug, not an opt regression":** it lives entirely in the
runtime memory model, independent of the JITed instruction stream — exactly PLAN.md:148-151.

**Minimal repros**
- Budget/over-read exhaustion (heap): `busybox sort` on a file just above the ~20k-line
  threshold, then well above 2M lines via stdin — bisect the line count; the threshold will
  track total distinct over-read/stack pages, not algorithmic structure.
- Stack growth (no codegen needed): `busybox ls -lR /` on a deep tree, or any guest doing
  recursion deeper than 8 MB of frames → faults below `stk`, lazily mapped until the same
  4096 budget is spent.
- Instrument: log `g_lazymaps` and `si_addr` vs. `[stk, stk+SZ)` / `[brk_lo, brk_hi)` at each
  lazy-guard entry; the crash will be the first fault with `g_lazymaps == 4096` **or** the
  first `si_addr < stk` once the budget is gone.

**Proposed fix (ranked)**
1. **Give the guest stack a proper auto-grow region** instead of relying on the lazy budget:
   reserve the stack with a large `PROT_NONE` guard below `stk` and grow on fault *without*
   charging `g_lazymaps`, or map the stack `MAP_STACK` with a generous size (e.g. 64–128 MB
   reserved, committed on demand). `elf.c:97-103`.
2. **Stop charging legitimate growth against the over-read budget.** Split the two concerns:
   a small bounded budget only for *over-reads adjacent to a known mapped region*, and an
   *unbounded* (or per-region-bounded) path for stack/heap growth faults. `elf.c:175-195`.
3. **Grow the brk region on demand** rather than capping at a fixed 256 MB
   (`linux_x86_64.c:216`), or ensure the brk syscall cleanly fails over to mmap (verify the
   service path) so a > 256 MB working set does not fault.

### #2 — (in-scope, latent, NOT the segv) `0x67`/addr32 EA never truncated
`emit_ea` ignores `I->addr32` (`decode.c:234`), so a 32-bit effective address is computed at
full 64-bit. Real correctness bug for `addr32` memory ops, but `sort` emits none; flagged so
it is not mistaken for the crash and can be fixed alongside. **Fix:** when `I->addr32`, mask
the computed EA in x17 to 32 bits before the load/store (and before adding fs/gs base).

---

## What I explicitly ruled OUT as the large-input SIGSEGV
- `rep movs/stos` count/loop (64-bit clean), SSE unaligned 16B loads (LDR Q is fine),
  `pmovmskb` length (scalar impl is exact), SIB/RIP/fs-gs addressing (64-bit clean), RSP
  (64-bit clean), flag substrate (wrong order ≠ wild pointer; predates PR1).
- Code-cache flush "under heavy translation" (PLAN.md:154 candidate): the cache is
  data-size-*independent* (a fixed program translates to a few MB; `CACHE_SZ=64 MB` is never
  approached by `sort`), so the flush path in `dispatch.c:42-60` does not fire on input size.
- Unimplemented opcodes (`std`, `cmps`, `scas`, `movntdq`, `popf`): these `report_unimpl` →
  **exit 70**, not 139, so they are not the observed crash.
