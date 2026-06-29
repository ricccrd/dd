# jit86 x86_64-translator bug cluster — root-cause diagnosis

Diagnostic only. Do NOT treat as a patch. All file:line refs are
`dd-jit/src/runtime/frontend/x86_64/`. Translator entry: `translate.c`
(`translate_block`), one-byte ops under `if (!I.two)` (line 353), two-byte
(`0F`) ops in the `else` arm including the SSE block (line 1413). The single
`UNIMPL` print is `translate.c:2291` (`report_unimpl`).

Encodings verified with `x86_64-linux-gnu-gcc`/`objdump` against the actual
guest sources in `dd-tests/guests/ext_abi/`.

---

## 1. jit86-cvtdq2ps — UNIMPL 0F 5B (CVTDQ2PS)

- **Symptom:** `[jit86] UNIMPL 0F opcode 0x5b` abort.
- **Repro:** `cargo run -p dd-tests -- struct-hfa -e x86_64`
  (also `union-pun`). Both guests emit `cvtdq2ps`.
- **Encoding:** `0f 5b c1` = `NP 0F 5B /r` (CVTDQ2PS xmm,xmm/m128, packed
  int32→float32). Prefix variants share opcode 0x5B: `66 0F 5B`=CVTPS2DQ
  (float→int32, MXCSR rounding), `F3 0F 5B`=CVTTPS2DQ (float→int32, truncate).
- **Translator location:** SSE dispatch `translate.c:1416–1899`. There is **no
  `op == 0x5B` case**; control reaches `else handled = 0;` at `translate.c:1898`,
  the `if (handled)` continue at 1900 is skipped, and the block falls through to
  `report_unimpl` at `translate.c:2291`.
- **Root cause:** missing opcode-table entry. CVTDQ2PS/CVTPS2DQ/CVTTPS2DQ are
  simply not decoded.
- **Fix direction:** add an `op == 0x5B` arm in the SSE block (near the other
  cvt handlers, ~`translate.c:1809`). NP path: `SCVTF Vd.4S, Vm.4S`
  (s32→f32, packed). `66` path: round-per-MXCSR convert f32→s32 (FCVTNS .4S, or
  the rounding-mode-respecting form once bug #5 is addressed). `F3` path:
  `FCVTZS Vd.4S, Vm.4S` (truncate). Load `vm` from memory via `e_ldr_q` when
  `I.is_mem`, mirroring op 0x5A/0x58. Only the NP variant is needed for the two
  failing guests, but encode all three since they share 0x5B.
- **Confidence:** high.

---

## 2. jit86-psadbw — UNIMPL 0F F6 (PSADBW)

- **Symptom:** `[jit86] UNIMPL 0F opcode 0xf6` abort.
- **Repro:** `cargo run -p dd-tests -- charmath -e x86_64` (guest emits
  `psadbw`).
- **Encoding:** `66 0f f6 c1` = `66 0F F6 /r` (PSADBW xmm,xmm/m128). Sum of
  |a−b| over each group of 8 bytes; two 16-bit results land in word0 of each
  64-bit lane (rest zero).
- **Translator location:** SSE dispatch `translate.c:1416–1899`. No
  `op == 0xF6` case in the two-byte/SSE arm → `handled = 0` at
  `translate.c:1898` → `report_unimpl` at `translate.c:2291`.
  Note: the one-byte `op == 0xF6` group3 handler at `translate.c:616` is inside
  `if (!I.two)` and is unrelated (TEST/NOT/NEG/MUL/DIV r/m8); PSADBW is two-byte.
- **Root cause:** missing opcode-table entry.
- **Fix direction:** add `op == 0xF6` in the SSE block. NEON lowering: load src
  (`e_ldr_q` if mem), `UABD Vt.16B, Vd.16B, Vs.16B` (absolute byte diffs), then
  reduce each 64-bit half: `UADDLV H?, Vt.8B` per half (or `UADDLP`→`UADDLP`
  chains) and place each 16-bit sum into word0 of the corresponding qword lane,
  zeroing the rest — matching the sse2neon `_mm_sad_epu8` sequence. Result
  written back to `vd`.
- **Confidence:** high.

---

## 3. jit86-rol-cl — UNIMPL D3 /0 (ROL r/m32 by CL)

- **Symptom:** `[jit86] UNIMPL 1B opcode 0xd3` abort (one-byte D3, reg field /0).
- **Repro:** `cargo run -p dd-tests -- rotate -e x86_64` (rol32 by variable n).
- **Encoding:** `d3 c0` = `D3 /0` (ROL r/m32 by CL); also D3 /0 with REX.W for
  r/m64. The 8/16-bit ROL-by-CL and all immediate/by-1 rotates already work.
- **Translator location:** group2 shift/rotate handler `translate.c:522–613`.
  The wide (w≥4) by-CL path explicitly bails: `translate.c:572–576`
  ```
  if (bycl) {
      if (k == 0) { report_unimpl(gpc, &I); break; } // ROL by CL: defer
  ```
  (`k==0` is ROL.) ROR-by-CL (`k==1`) is handled via `S_RORV` at 577–578.
- **Root cause:** deliberate `// ROL by CL: defer` stub — the 32/64-bit ROL-by-CL
  case was never filled in. Everything else (immediate ROL at 593–594 using
  `e_ror_i(width-cnt)`, and the 8/16-bit by-CL ROL at 537–546) exists.
- **Fix direction:** in the `k == 0 && bycl` branch emit a variable rotate:
  `cnt = CL & (width-1)`; `rot = (width - cnt) & (width-1)`; then
  `e_shv(S_RORV, dst, src, rot_reg, ssf)` — exactly the 8/16-bit by-CL idiom at
  `translate.c:537–546` but at the native 32/64-bit width (no replication
  needed). x86 ROL leaves SF/ZF unchanged and only writes CF/OF; the existing
  `e_tst`/`e_nzcv_save` tail at 597–610 would clobber SF/ZF, so skip the flag
  save for the rotate (as the 8/16-bit path already does at 552, "no flag save").
- **Confidence:** high.

---

## 4. jit86-parity — `__builtin_parityll` wrong (PF not modeled for integers)

- **Symptom:** `par` output wrong; parity flag (PF) reads garbage.
- **Repro:** `cargo run -p dd-tests -- popcnt -e x86_64` (uses
  `__builtin_parityll`).
- **Lowering (gcc -O2, verified):** parity folds the value down and ends with
  `xorb %dh,%dl` (8-bit XOR, opcode `30 /r`) **then `setnp %r8b` (`0F 9B`)**,
  reading the x86 parity flag of the resulting low byte.
- **Translator location:** the x86 condition map `x86cc_to_arm`
  `translate.c:277–284` maps the parity conditions (low-nibble idx 10 = JP/SETP,
  idx 11 = JNP/SETNP) onto **ARM cond 6/7 = VS/VC**, i.e. the V flag:
  `static const int t[16] = {...,6,7,...}` (entries [10],[11]). setcc reads this
  at `translate.c:2174–2192` (`e_cset(16, cc, 0)` after `e_nzcv_load`); jcc at
  `translate.c:2128–2129`. The integer XOR sets flags via the logic finalizer
  (`alu_core` case 6 → `e_nzcv_save`/`e_nzcv_save_c1`, `emit.c:120,144`), which
  stores ARM NZCV; **V is never set to x86 parity** for integer ops.
- **Root cause:** there is no real PF substrate. PF is *aliased* to the ARM V
  flag, which is only meaningful right after an FP compare (comisd leaves V=1 on
  unordered — see the comment at `translate.c:279–281`). After an integer
  XOR/TEST, V is stale, so `setnp`/`jnp` read an unrelated bit → wrong parity.
- **Fix direction:** compute true x86 PF = even-parity of the low 8 bits of the
  result for flag-setting integer ops, and store it in a dedicated PF lane the
  parity consumers read. Practical approach: reserve a PF byte/bit in the cpu
  flags area (alongside `OFF_NZCV`), and in the integer flag finalizers compute
  parity (e.g. low byte → `EOR`-fold 8→1, or `FMOV`+`CNT`+`AND #1`, then invert
  for even-parity). Then route the parity branch of `x86cc_to_arm` /
  setcc(0x9A/0x9B) / jcc(0x7A/0x7B,0x8A/0x8B) to test that PF lane instead of V.
  Keep the existing V=PF alias for the FP-compare idiom, or fold FP-compare PF
  into the same lane. This is a substrate change, not a one-liner.
- **Confidence:** high (lowering and flag-aliasing both directly verified).

---

## 5. jit86-lrint-round — lrint ignores MXCSR rounding (CVTSD2SI)

- **Symptom:** `lrint()` always rounds to-nearest; `fesetround(FE_UPWARD/DOWNWARD/
  TOWARDZERO)` has no effect → wrong `acc`.
- **Repro:** `cargo run -p dd-tests -- fp-round -e x86_64`.
- **Encoding:** `lrint` → `cvtsd2si` = `F2 0F 2D /r` (round per MXCSR.RC).
  `fesetround` → `ldmxcsr` = `0F AE /2` (and `fldcw` for x87).
- **Translator location:** two independent gaps.
  1. CVTSD2SI handler `translate.c:1754–1765`: op 0x2D **hardcodes FCVTNS**
     (round-to-nearest-even): `fop = (op == 0x2C) ? FCVTZS : FCVTNS;` — there is
     no path that honors a dynamic rounding mode.
  2. `ldmxcsr` (`0F AE /2`) is a **no-op**: `translate.c:2001–2004`
     `if (sub == 2) { ... continue; } // ldmxcsr: ignore (no SSE rounding/excepts)`.
     So MXCSR.RC never reaches the ARM FPCR.
- **Root cause:** the guest rounding mode is never tracked nor threaded into
  FPCR, and CVTSD2SI uses a fixed rounding instruction.
- **Fix direction:** (a) on `ldmxcsr`, load the MXCSR word, extract RC (bits
  14:13: 00 nearest / 01 down(−∞) / 10 up(+∞) / 11 truncate), translate to ARM
  FPCR.RMode (bits 23:22: 00 RN / 01 RP / 10 RM / 11 RZ — note the encoding
  differs from x86) and `MSR FPCR, x`. (b) make CVTSD2SI (op 0x2D) honor the
  active mode — either select FCVTNS/FCVTPS/FCVTMS/FCVTZS from a tracked RC, or
  emit `FRINTI` (round-to-integral using FPCR mode) followed by `FCVTZS`. Also
  mirror RC into the x87 control word path (`fldcw`) since `fesetround` sets both
  — see `translate/x87.c`. (cvttsd2si / op 0x2C correctly stays FCVTZS = truncate
  regardless of mode.)
- **Confidence:** high.

---

## 6. jit86-fcmp-unordered — NaN compare predicate wrong (UCOMISD/COMISD)

- **Symptom:** float compares involving NaN give wrong booleans; le/ge/eq/seta
  counts off.
- **Repro:** `cargo run -p dd-tests -- fp-cmp -e x86_64` (matrix of compares
  with NAN/±INF; gcc emits `comisd`/`ucomisd` + `seta`/`setp`/`setnp`).
- **Encoding:** `0F 2E /r` (UCOMISD/UCOMISS) and `0F 2F /r` (COMISD/COMISS),
  `66` selects double.
- **Translator location:** `translate.c:1874–1885`:
  ```
  emit32((I.p66 ? FCMP.D : FCMP.S) | (s<<16) | (vd<<5)); // FCMP
  e_nzcv_save(); // raw store of ARM NZCV
  ```
- **Root cause:** ARM `FCMP` encodes **unordered** as `N=0,Z=0,C=1,V=1`, but x86
  COMISD must set `ZF=1,PF=1,CF=1` on unordered. The handler stores ARM NZCV
  verbatim. Downstream, x86 ZF maps to stored Z, x86 CF maps to NOT stored-C
  (borrow convention, `emit.c:128–130`), x86 PF maps to V. Result on unordered:
  PF=V=1 ✓ (the reason `setp`/`setnp` work), but **ZF = ARM Z = 0 ✗** (x86 wants
  1) and **x86 CF = NOT ARM C = 0 ✗** (x86 wants 1). So every NaN comparison that
  reads ZF/CF (`seta` = CF=0&ZF=0, `setbe`, `je`, `jbe`, …) is wrong. The ordered
  cases (less/equal/greater) map correctly; only the unordered column is broken.
- **Fix direction:** after `FCMP`, before storing, apply an unordered fixup so
  the stored flags satisfy the existing consumer conventions:
  `storedZ = Z | V`; `storedC(borrow) = C & ~V`; clear `N` (x86 SF is always 0
  for COMISD); keep `V` (=PF). Concretely read NZCV (`mrs`), derive a V-mask and
  OR it into Z / BIC it into C, then `str` to `OFF_NZCV` (and `msr` back if the
  live flags are reused). With that, ZF/CF/PF all match x86 in the ordered and
  unordered cases.
- **Confidence:** high.

---

## 7. jit86-opcode-1c — UNIMPL 1B 0x1C (SBB AL, imm8; byte ADC/SBB)

- **Symptom:** `[jit86] UNIMPL 1B opcode 0x1c` while running node:20-slim.
- **Repro:** observed under node:20-slim; minimal: any 8-bit `SBB AL,imm8`.
- **Encoding:** `1c 07` = `1C ib` = `SBB AL, imm8` (one-byte, not 0F). The
  message prefix "1B" is the engine's tag for *one-byte* opcodes (`I->two ?
  "0F" : "1B"`, `translate.c:2291`), so this is the single-byte 0x1C, **not** the
  two-byte `0F 1C` (which is a multi-byte-NOP and is handled at
  `translate.c:1930`).
- **Translator location:** byte-width ADC/SBB is explicitly unimplemented in
  every primary-ALU path:
  - acc-imm forms (0x14 ADC AL,ib / 0x1C SBB AL,ib): `translate.c:476–484` —
    `if (!((k==2||k==3) && w<4))` guards the body; for 0x1C k=3 (SBB, via
    `alu_kind_primary` at `translate.c:9–12`), w=1 → guard false → body skipped →
    falls through to `report_unimpl` at 2291.
  - reg/rm forms (0x10–0x13/0x18–0x1B): `translate.c:452–455`
    `if ((k==2||k==3) && w<4){ report_unimpl(...); break; } // ADC/SBB 8/16: TODO`.
  - group1 byte (`80 /2`,`80 /3`): `translate.c:486–488` same `w<4` guard.
- **Root cause:** narrow (8/16-bit) ADC/SBB is a deliberate TODO. `do_alu`
  kind 2/3 (the carry-value consumer path, `translate.c:180–254`) only supports
  width ≥ 4; byte/word carry-in + flag synthesis were never implemented.
- **Fix direction:** implement 8/16-bit ADC/SBB. Materialize the deferred/stored
  x86 CF into a register, do the add/sub-with-carry on the operands (the existing
  64-bit `ADCS/SBCS` with operands placed in the high bits via the `<<24`/`<<(64-
  8w)` trick used elsewhere for narrow flag synthesis), then store the low w
  bytes and synthesize ZF/SF/CF/OF/PF at width w. Touch `do_alu`'s kind 2/3
  branch and remove the three `w<4` bail guards at 452/478/488 once the narrow
  path exists. (Once bug #4 lands, PF must also be produced here.)
- **Confidence:** high.

---

## 8. jit86-movmskps — UNIMPL 0F 50 (MOVMSKPS / MOVMSKPD)

- **Symptom:** `[jit86] UNIMPL 0F opcode 0x50` abort. Surfaces inside glibc's
  hex-float printf (`%a`) and `strtold`/`strtof` paths.
- **Repro:** `cargo run -p dd-tests -- printf-hexfloat -e x86_64` and
  `cargo run -p dd-tests -- strto-float -e x86_64` (guests in
  `dd-tests/guests/ext_libc/`; the MOVMSKPS lives in glibc, not the guest .c).
- **Encoding:** `0f 50 c1` = `NP 0F 50 /r` MOVMSKPS (extract the 4 packed-single
  sign bits → low 4 bits of a GPR). `66 0f 50 c1` = `66 0F 50 /r` MOVMSKPD
  (2 packed-double sign bits → low 2 bits). Reg-form only (xmm source; ModRM
  mod=3); there is no memory form.
- **Translator location:** SSE dispatch `translate.c:1416–1899`. There is **no
  `op == 0x50` case** (the only 0x50 in the file is the one-byte PUSH at
  `translate.c:423`, in the `if (!I.two)` arm — unrelated). Two-byte 0x50 falls
  through `else handled = 0;` at `translate.c:1898` → `report_unimpl` at
  `translate.c:2291`.
- **Root cause:** missing opcode-table entry.
- **Fix direction:** add `op == 0x50` in the SSE block, structured like the
  existing PMOVMSKB fast path (`translate.c:1715–1733`) but on wider lanes.
  MOVMSKPS (NP): `USHR Vt.4S, Vm.4S, #31` to drop each lane to its sign bit,
  then gather the 4 lane-bit0s into bits 0..3 of `I.reg` (e.g. the sse2neon
  `_mm_movemask_ps`: USHR .4S #31, then a `USRA`-style horizontal pack, or
  `UMOV` 4 lanes + shift-OR). MOVMSKPD (66): same with `.2D, #63` → 2 bits.
  Write the result zero-extended into the GPR `I.reg` (W-form). No memory path
  needed.
- **Confidence:** high.

---

## 9. jit86-wcschr-bool — SILENT miscompile: byte `mov %al,%ch` clobbers RBP

- **Symptom:** exit 0, no UNIMPL, but `wchar_str` prints `d4=178659460`
  (garbage) instead of `d4=1`. d1/d2/d3/d5/d6/d7 are all correct.
- **Repro:** `cargo run -p dd-tests -- wchar-str -e x86_64` (oracle mismatch on
  x86_64 only; aarch64/darwin correct). Confirmed live: JIT stdout
  `...d4=178659460...`, qemu-x86_64 oracle `...d4=1...`.
- **Investigation (verified at runtime, not inferred):**
  - `wcschr` itself is translated **correctly** — a standalone
    `wcschr(L"hello", L'l')` returns the right pointer under the JIT.
  - In the guest, `d4 = wcschr(...) != NULL && wcschr(...) == NULL` is held in
    `%rbp`/`%bpl` (gcc -O2; `sete %bpl` at `main+0xc5`, later `mov %ebp,%r9d`).
    `%rbp` is callee-saved and spans the *later* `d6` call
    `wcscmp(w, L"12345")`.
  - A sentinel probe (put `0x11..11` in `%rbp`, call each wchar fn, re-read)
    shows **only `wcscmp` corrupts `%rbp`** under the JIT (rbx/r12/r13/r14/r15
    stay intact → not a stack imbalance; rbp-specific). So d6's `wcscmp`
    destroys d4, which is still living in rbp.
  - `__wcscmp_sse2`'s 4th instruction is `mov %al,%ch` (`88 c5`). `%ch` is the
    high byte of `%rcx`; with **no REX**, the byte r/m field value 5 denotes
    `CH`, *not* `RBP`.
- **Encoding:** `88 c5` = `MOV r/m8, r8` (opcode 0x88), ModRM `C5` = mod=3,
  reg=0 (`AL`), r/m=5 → `CH` (no-REX high-byte register).
- **Translator location:** the 88/89/8A/8B handler at `translate.c:382–413`.
  The memory paths (385–405) correctly use `byte_val`/`byte_wb` for the
  ah/bh/ch/dh high-byte semantics, but the **register-direct `else` branch
  ignores byte width entirely**:
  ```
  } else {
      if (to_reg) e_mov_rr(I.reg,    I.rm_reg, sf);   // 8A/8B
      else        e_mov_rr(I.rm_reg, I.reg,    sf);   // 88/89   <-- bug
  }
  ```
  (`translate.c:406–411`.) For `mov %al,%ch` this emits
  `e_mov_rr(I.rm_reg=5, I.reg=0, sf)` = a **full-width move of guest RAX into
  guest RBP** (reg index 5 = RBP), because it (a) never maps the no-REX byte
  index 4–7 to ah/ch/dh/bh and (b) uses `sf`/full width instead of a byte
  insert. At that point `RAX` = low32 of a string pointer (`mov %esi,%eax` two
  insns earlier), which is exactly the pointer-shaped garbage observed in the
  corrupted `%rbp` (e.g. `0x...6010`).
- **Root cause:** register-direct byte `MOV` (0x88/0x8A) is mistranslated. It
  does not honor 8-bit width (must preserve the upper 56 bits of the dest) and
  does not remap no-REX register indices 4–7 to the high-byte regs
  (AH/CH/DH/BH). So any reg-direct byte mov touching AH/CH/DH/BH writes the
  WRONG full register: `%ch`→`%rbp`, `%ah`→`%rsp`, `%dh`→`%rsi`, `%bh`→`%rdi`.
  Here it silently corrupts the guest's callee-saved RBP inside wcscmp.
- **Fix direction:** in the register-direct branch (`translate.c:406–411`),
  special-case `w == 1` exactly as the memory paths already do: read the source
  byte with `byte_val(&I, I.reg/I.rm_reg, scratch)` and write the destination
  byte with `byte_wb(&I, dest_regnum, scratch)` (which handles both the
  high-byte placement at bits 8–15 and preserving the surrounding bits). Only
  the `w >= 2` case may keep the current `e_mov_rr(..., sf)`. This also fixes
  the latent upper-bits-clobber for low-byte reg-direct moves (`mov %al,%bl`
  etc.).
- **Confidence:** high (runtime-verified end to end: corrupting fn isolated to
  wcscmp, instruction isolated to `mov %al,%ch`, and the mistranslated
  code path identified).

---

### Cross-cutting notes
- Bugs #4 (PF) and #6 (FP-compare flags) both stem from the same design
  shortcut: there is no dedicated PF lane and the FP-compare/parity idiom is
  squeezed onto the ARM V flag. A real PF lane fixes #4 and lets #6 stop relying
  on the V=PF coincidence.
- Bug #5 (MXCSR→FPCR) also benefits bug #1's `66 0F 5B` (CVTPS2DQ) and any other
  MXCSR-rounding convert; thread RC once.

---

## Syscall behavioral gaps

Behavioral (wrong-result, not unimplemented) divergences in the Linux
syscall-emulation layer on the macOS/BSD-hosted runtime. All paths live in
`dd-jit/src/runtime/os/linux/` (`service.c` big `switch`, sub-dispatchers in
`service/*.c`, caches in `fscache.c`). Every item below was **runtime-verified
against a freshly-rebuilt engine** (`cargo clean -p ddjit && cargo build`, then
`DDJIT_DIR=<out> cargo run -p dd-tests -- <name> -e x86_64 …`, observed via
`DD_DEBUG=1`) — so none of these are stale-bundle artifacts; they are genuine
source bugs.

Important harness gotcha confirmed while diagnosing: `Engine::jit()` resolves
the engine via `resolve_bundled` (dd-jit/src/lib.rs:80), whose order is
`$DDJIT_DIR` → exe dir → **`/Applications/dd.app/Contents/Resources`** → baked
build path. With dd.app installed, the harness silently runs the *bundled*
engine, NOT your source build. Pin `DDJIT_DIR` to the `build.rs` OUT_DIR
(`target/debug/build/ddjit-*/out/`) to test source changes.

### S1. nonblock → EAGAIN cluster (`pipe2(O_NONBLOCK)`, `eventfd(EFD_NONBLOCK)`)  — SHARED ROOT
- **Symptom:** `F_GETFL` shows `O_NONBLOCK` and the read does NOT hang, yet a
  read on an empty pipe / zero-counter eventfd reports `eagain=0`. Fresh-engine
  output: `pipe2 … eagain=0`, `eventfd_nonblock eagain=0 … eagain2=0`.
- **Repro:** `pipe2 -e x86_64`, `eventfd-nonblock -e x86_64`.
- **Handler:** create paths are correct — pipe2 sets host `O_NONBLOCK`
  (service/io.c:419-422), eventfd2 likewise (service.c:2315-2318); read paths
  return EAGAIN correctly: plain pipe `read()` → `-errno` (service/io.c:148-149),
  eventfd → `-EAGAIN` (service/io.c:132). The flag IS honored (read returns
  immediately instead of blocking).
- **Root cause:** **errno number not translated.** The boundary BSD→Linux errno
  map `m2l_errno` runs only at the very end of `service()`
  (service.c:2812-2818). But the I/O sub-dispatcher is invoked as
  `if (svc_io(...)) return;` (service.c:161) — and the other sub-dispatchers
  `svc_sysv/svc_mem/svc_signal/svc_time` at lines 157-160 — so **any syscall
  handled in a sub-module returns from `service()` BEFORE the boundary
  translation runs.** macOS `EAGAIN = 35`; the guest (Linux) receives `-35` and
  reads it as Linux errno 35 = `EDEADLK`, so `errno == EAGAIN (11)` is false.
  The empty-pipe read genuinely returns EAGAIN — with the wrong errno integer.
- **Fix direction:** apply `m2l_errno` on the error-return path of the
  sub-dispatchers too — either translate inside each `svc_*` before returning,
  or restructure so they fall through to the single boundary at service.c:2817
  (e.g. set a "handled" flag and `goto`/break to the tail instead of early
  `return`). This is the cleanest single fix.
- **Confidence:** high (mechanism isolated; flag-propagation theory ruled out —
  read does not block, only the errno integer is wrong).
- **Shared root:** affects *every* `svc_io/svc_mem/svc_signal/svc_time/svc_sysv`
  error return whose errno diverges between macOS and Linux (EAGAIN 35↔11,
  ENOSYS 78↔38, ELOOP 62↔40, ENOTEMPTY 66↔39, EDEADLK 11↔35, …). The nonblock
  cluster is the visible symptom; other divergent-errno paths are latently
  wrong for the same reason.

### S2. `prctl` PR_GET_* family (NO_NEW_PRIVS / DUMPABLE / PDEATHSIG)
- **Symptom:** `PR_GET_NO_NEW_PRIVS` returns -1; `PR_GET_DUMPABLE` always 0
  (doesn't reflect `PR_SET_DUMPABLE`); `PR_GET_PDEATHSIG` fails. Fresh output:
  `prctl_nnp … after=-1`, `prctl_dumpable off=0 on=0`, `prctl_pdeathsig set=1
  get=0 sig=0`.
- **Repro:** `prctl-nnp`, `prctl-dumpable`, `prctl-pdeathsig` `-e x86_64`.
- **Handler:** service.c:1513-1535 (`case 167`). Only PR_SET/GET_NAME (15/16)
  are real; an inner `switch` returns 0 for a hard-coded no-op set
  {1,3,4,8,15,35,36,38,53,55,59}, else `-EINVAL`.
- **Root cause:** no per-process state is stored for these options, and the
  GET subcodes aren't implemented to read it:
  - `PR_GET_PDEATHSIG`(2) and `PR_GET_NO_NEW_PRIVS`(39) fall to `default →
    -EINVAL` (returned to guest as -1). `PR_SET_PDEATHSIG`(1) /
    `PR_SET_NO_NEW_PRIVS`(38) are in the 0-list but persist nothing.
  - `PR_GET_DUMPABLE`(3) is in the 0-list, so it always returns **0** as the
    syscall value; `PR_SET_DUMPABLE`(4) stores nothing → GET never reflects SET.
  (Linux returns NNP/DUMPABLE as the syscall *return value*, and PDEATHSIG via
  the `*(int*)arg2` out-pointer.)
- **Fix direction:** add three static per-process variables (`g_no_new_privs`,
  `g_dumpable` default 1, `g_pdeathsig`); have SET subcodes store them and GET
  subcodes return them (NNP/DUMPABLE via `G_RET`, PDEATHSIG written to `*(int*)a1`
  then `G_RET=0`). `no_new_privs` is one-way (can only be set to 1).
- **Confidence:** high.

### S3. `openat2` — fd opens but reads yield `\0` (open_how ignored)
- **Symptom:** `openat2 ok=1 byte=\0` (expected `byte=Z`).
- **Repro:** `openat2 -e x86_64`.
- **Handler:** service.c:1303-1309 (`case 437`).
- **Root cause:** **mis-ordered `case` fall-through.** The handler unpacks
  `open_how` (`a2 = how[0]` flags, `a3 = how[1]` mode) then comments
  `/* fall through to openat */` — but the physically-next label is **not**
  openat. `case 56` (openat) is far above at service.c:737; the label
  immediately following `case 437` is `case 439:`/`case 48:` (faccessat,
  service.c:1311-1312). So openat2 actually executes **faccessat**: with
  `a2 = O_RDONLY = 0` it takes the `F_OK` existence branch and returns **0**
  on success. The guest reads `0` as an fd → treats it as a valid descriptor,
  then `read(0,…)` reads **stdin** (empty/`/dev/null` under the harness) → one
  `\0` byte. (`ok = r>=0` is true because 0≥0.)
- **Fix direction:** don't rely on physical fall-through across a 560-line gap.
  Either physically relocate `case 437` to sit immediately before `case 56`, or
  replace the fall-through with an explicit `goto`/shared helper invoking the
  openat logic with the unpacked flags/mode. (`how[2]` resolve flags can stay
  ignored — the VFS jail already confines.)
- **Confidence:** high (output `byte=\0`, `ok=1` exactly matches the stdin-fd-0
  theory; case ordering verified).

### S4. `fchmod(fd,…)` appears a no-op (chmod-by-path works)  — SHARED ROOT with S6
- **Symptom:** `chmodchown … m755=0` (chmod-by-path `m600=1` works; fchmod-by-fd
  then stat-by-path still reports the old mode).
- **Repro:** `chmodchown -e x86_64` (test: `chmod(path,0600)`; `stat`;
  `fchmod(open(path),0755)`; `stat`).
- **Handler:** `fchmod` = service.c:679 (`case 52`) — calls the real
  `fchmod((int)a0, mode)`, which DOES change the host file's mode.
- **Root cause:** **stale path-keyed metadata cache.** The first `stat(path)`
  caches the `struct stat` (mode 0600) in the `mc_` cache (fscache.c:124,
  keyed by path, **no TTL / no generation** — fscache.c:110-122). `fchmod`
  (case 52) mutates the host file but **never calls `mc_evict`** — it only has
  the fd, not the path, so it can't. The next `stat(path)` hits the stale
  cached 0600. By contrast `fchmodat`/path-chmod (service.c:691-705) **do** call
  `mc_evict(p)`, which is why chmod-by-path works.
- **Fix direction:** evict the fd's cached metadata in `fchmod`/`fchown`/
  `ftruncate`/`futimens` (any fd-based metadata mutation). Recover the path via
  `fcntl(fd, F_GETPATH, …)` and `mc_evict` it; or maintain an fd→path map
  (`g_fdpath[]` already exists and is populated on open) and evict by that; or,
  simplest, drop the whole `mc_` entry set on any fd-based metadata write.
- **Confidence:** high.
- **Shared root:** the un-invalidated `mc_` path-keyed stat cache (fscache.c) —
  same root as S6.

### S5. `rewinddir` / `lseek(dirfd,0)` doesn't reset getdents (2nd pass empty)
- **Symptom:** `readdir_dtype … rewind=0` (expected `rewind=3` — re-enumeration
  after `rewinddir`).
- **Repro:** `readdir-dtype -e x86_64`.
- **Handler:** getdents64 = service.c:1020 (`case 61`); lseek = service/io.c:6
  (`case 62`).
- **Root cause:** getdents caches a `DIR*` per guest fd via
  `fdopendir(dup(fd))` (service.c:1074-1085, table `g_dirs[]`). That `DIR*`
  owns its **own** cursor on a **duplicated** fd. glibc `rewinddir` issues
  `lseek(dirfd, 0, SEEK_SET)`, but the lseek handler (service/io.c:19) seeks
  only the *original* guest fd — it does **not** `rewinddir`/`seekdir` the
  cached `DIR*` nor drop the cache entry. So after the first pass exhausts the
  `DIR*`, the second getdents reuses the EOF-positioned `DIR*` and returns 0.
- **Fix direction:** in the lseek handler, when the fd has a cached `DIR*`
  (`g_dirs[]`), translate the seek into the dir stream: `lseek(fd,0,SEEK_SET)`
  → `rewinddir(d)` (or `seekdir(d, off)` for nonzero), or just `dirs_drop(fd)`
  (helper at service/helpers.c:97) so the next getdents re-`fdopendir`s at
  offset 0. (`dirs_drop` is already used on close at service.c:1012.)
- **Confidence:** high.

### S6. `st_nlink` stale after unlinking a hardlink  — SHARED ROOT with S4
- **Symptom:** `linksym … nlink1=0` (after `link(a,h)` → nlink 2 is seen
  correctly; after `unlink(h)`, `stat(a)` still reports nlink 2, not 1).
- **Repro:** `linksym -e x86_64`.
- **Handler:** stat population is correct (`fill_linux_stat`, copies host
  `st_nlink`); the fault is the cache. unlink = service.c:380 (`case 35`).
- **Root cause:** same stale `mc_` cache as S4. The first `stat(a)` caches
  nlink=2 under path `a`. `unlink(h)` evicts only path `h`
  (`mc_evict(p)` service.c:450) — it cannot know `a` is a sibling hardlink to
  the same inode — so path `a`'s cached `struct stat` (nlink=2) is never
  invalidated. The next `stat(a)` returns the stale nlink. `linkat`
  (service.c:481) likewise doesn't evict the source path.
- **Fix direction:** the path-keyed cache fundamentally can't track inode
  aliasing. Options: (a) don't cache `st_nlink` for files with nlink>1 (cache
  the rest, re-stat for the count); (b) key/secondary-index the cache by
  dev+ino so any inode-mutating op (unlink/link) can evict all aliases;
  (c) bypass/short-TTL the cache for `nlink`. Lowest-risk: drop nlink>1 entries
  from the cache (or never serve `st_nlink` from it).
- **Confidence:** high.
- **Shared root:** the un-invalidated `mc_` path-keyed stat cache (fscache.c) —
  same root as S4. A cache-coherence fix that handles fd-based and
  inode-aliased mutations resolves both S4 and S6.

### S7. `timerfd_gettime` reports 0 remaining for an armed one-shot
- **Symptom:** `timerfd_gettime … remaining=0` (expected positive remaining for
  a freshly-armed 10s one-shot; `disarmed=1` is correct).
- **Repro:** `timerfd-gettime -e x86_64`.
- **Handler:** service.c:2583-2588 (`case 87`) is a pure stub:
  `if (a1) memset(a1,0,32); G_RET=0;` — always reports zeros.
- **Root cause:** timerfd is emulated with a kqueue `EVFILT_TIMER`
  (`timerfd_settime` = service.c:2557, `EV_SET(&kv, 1, EVFILT_TIMER, …)`).
  kqueue has **no API to query an armed timer's remaining time**, and the
  engine stores no arming metadata, so gettime can only return 0. (Aside: the
  EVFILT_TIMER ident is hard-coded to `1`, fine since each timerfd has its own
  kqueue, but it means there's nowhere to look up remaining either.)
- **Fix direction:** record arm state per timerfd in a side table at
  settime — interval (ns) and the absolute deadline
  (`clock_gettime(MONOTONIC) + value`). In `timerfd_gettime`, compute
  `it_value = max(0, deadline - now)` and `it_interval` from the stored
  interval; return zeros only when disarmed.
- **Confidence:** high.

### S8. realtime `signalfd`: only the first siginfo has the right ssi_signo
- **Symptom:** `signalfd_rt reads=3 signo_ok=1` (3 reads succeed but only the
  first carries `ssi_signo == SIGRTMIN`; the rest are 0).
- **Repro:** `signalfd-rt -e x86_64` (blocks SIGRTMIN, `raise()`×3, reads 3).
- **Handler:** signalfd read = service/io.c:31-55; signalfd4 = service.c:2519;
  the pending set is a 64-bit bitmask `g_pending` poked from the signal path
  (service/time.c:111).
- **Root cause:** **pending signals are modeled as a bitmask, not a queue.**
  The read path scans `g_pending` for the first set bit in the signalfd mask,
  clears it, writes `ssi_signo = sig`, returns one 128-byte record
  (service/io.c:40-53). Three `raise(SIGRTMIN)` set the **same single bit** —
  the first read consumes it; subsequent reads find no bit → `sig = 0` →
  `ssi_signo = 0`. A bitmask cannot represent realtime-signal multiplicity/
  ordering (or queued `ssi_value`).
- **Fix direction:** back signalfd with a real FIFO queue of siginfo records
  (signo + value + code) instead of (or alongside) the bitmask, enqueued in the
  host signal handler and dequeued one-per-read. At minimum, track a per-signal
  pending **count** so repeated standard signals coalesce (Linux) but RT signals
  each deliver. Realtime signals (SIGRTMIN..SIGRTMAX) require true queuing.
- **Confidence:** high.

### S9. `inotify` IN_MOVED_FROM / IN_MOVED_TO never delivered
- **Symptom:** `inotify_moves create=1 from=0 to=0` (CREATE works; a rename
  within the watched dir yields no MOVED_FROM/MOVED_TO pair).
- **Repro:** `inotify-moves -e x86_64`.
- **Handler:** inotify_add_watch = service.c:2463 (`case 27`); read/diff =
  service/io.c:56-112.
- **Root cause:** directory events are synthesized by **snapshotting the
  directory listing and diffing** against the previous snapshot
  (service/io.c:68-95): pass 0 emits names that appeared → `IN_CREATE`(0x100),
  pass 1 emits names that vanished → `IN_DELETE`(0x200). A rename within the
  dir shows as "old name gone + new name present", so it is reported as
  DELETE+CREATE — **never** `IN_MOVED_FROM`(0x40)/`IN_MOVED_TO`(0x80), and the
  cookie that pairs them is always 0. A set-difference snapshot has no way to
  recognize that a disappearance and an appearance are the same inode moving;
  kqueue's `NOTE_RENAME` (registered at service.c:2474) fires on the *watched
  dir vnode itself* (→ IN_MOVE_SELF), not on per-entry renames.
- **Fix direction:** the snapshot must carry per-entry inode (dev/ino), not just
  names. On diff, pair a vanished name and an appeared name with the **same
  inode** into a MOVED_FROM/MOVED_TO event sharing a fresh cookie; emit
  CREATE/DELETE only for unpaired appears/vanishes. (Full fidelity — distinct
  watches, moves across watched dirs — ultimately needs FSEvents or a richer
  model, but intra-dir rename pairing via inode is enough for this case.)
- **Confidence:** high.

### Syscall cross-cutting notes
- **S1** is an architectural gap (errno translation bypassed for *all*
  sub-dispatched syscalls), not a per-call bug — fix it once at the
  `svc_*` return boundary and an entire class of divergent-errno failures
  clears.
- **S4 + S6** share the `mc_` path-keyed stat cache (fscache.c): it has no TTL
  and is only evicted by exact path, so fd-based mutations (S4) and inode
  aliasing (S6) leave stale `st_mode`/`st_nlink`. One cache-coherence fix
  covers both.
- **S3** is a textbook mis-ordered `switch` fall-through; cheap, isolated fix.
- **S2/S7/S8/S9** are missing-state / wrong-primitive emulation gaps: prctl
  needs 3 state vars; timerfd needs a deadline side-table; signalfd needs a
  queue not a bitmask; inotify needs inode-aware diff to synthesize move pairs.
