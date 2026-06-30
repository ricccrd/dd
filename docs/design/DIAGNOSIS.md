# jit86 (x86_64→ARM64) codegen invariants

Load-bearing facts about the x86_64 frontend (`frontend/x86_64/`) that a future change must respect —
each is the root of a fixed miscompile, so breaking it silently corrupts the guest. (The individual
opcode-gap bugs that surfaced these have shipped; these are the structural rules left behind.)

## Flags substrate

- **There is no real x86 PF.** Parity is *aliased* onto the ARM **V flag** (`x86cc_to_arm` maps the
  parity conditions to VS/VC). V is only meaningful right after an FP compare (`comisd` leaves V=1 on
  unordered). After an integer op V is **stale** — so any new integer flag-setting path that must feed
  `setp/setnp/jp/jnp` has to compute true even-parity of the low 8 bits itself, not lean on V.
- **Unordered FP compare must be fixed up.** ARM `FCMP` encodes unordered as `N=0,Z=0,C=1,V=1`, but
  x86 `COMISD`/`UCOMISD` need `ZF=1,PF=1,CF=1`. With dd's conventions (x86 ZF←ARM Z, x86 CF←NOT ARM C,
  x86 PF←V) a raw NZCV store gives wrong ZF/CF on unordered. After `FCMP`, before storing: `Z |= V`,
  `C &= ~V`, clear N (x86 SF is 0 for COMISD), keep V. Any new FP-compare lowering must apply this.
- **Narrow ADC/SBB synthesize their own CF/OF/ZF/SF/PF** at width `w` (the carry-value path only ever
  supported `w≥4`; 8/16-bit needs the explicit synthesis).

## Rounding

x86 MXCSR.RC and ARM FPCR.RMode use **different bit encodings** (x86 bits 14:13 nearest/down/up/trunc;
ARM bits 23:22 RN/RP/RM/RZ). `ldmxcsr`/`fldcw` must translate RC→RMode into FPCR; rounding-mode
converts (`cvtsd2si` op 0x2D) must honor the active mode, while `cvttsd2si` (0x2C) stays FCVTZS
(truncate) regardless. `fesetround` sets both MXCSR and the x87 control word — thread RC to both.

## Register-direct byte MOV — the no-REX high-byte trap

`MOV r/m8,r8` / `MOV r8,r/m8` (0x88/0x8A) in **register-direct** form (ModRM mod=3) must honor 8-bit
width *and* remap no-REX register indices 4–7 to the high-byte regs **AH/CH/DH/BH** — exactly as the
memory paths already do via `byte_val`/`byte_wb`. A full-width `e_mov_rr` here silently writes the
WRONG 64-bit register (`%ch`→RBP, `%ah`→RSP, `%dh`→RSI, `%bh`→RDI) and clobbers the upper 56 bits of
low-byte dests. This silently corrupted a callee-saved register inside `__wcscmp_sse2`.

## Syscall layer — the boundary errno-translation invariant

The BSD→Linux errno map `m2l_errno` runs only at the *end* of `service()`. Any syscall handled in a
sub-dispatcher (`svc_io/mem/signal/time/sysv`) that `return`s early **bypasses it**, so the guest sees
a macOS errno number (e.g. EAGAIN 35 read as Linux EDEADLK). Every sub-dispatcher error return must be
errno-translated (translate before returning, or fall through to the single boundary). This is one
architectural fix, not per-call — it latently affects every divergent-errno path (ENOSYS 78↔38,
ELOOP 62↔40, ENOTEMPTY 66↔39, …).

## Path-keyed stat cache (`fscache.c` `mc_`) cannot track aliasing

The metadata cache is keyed by exact path with no TTL/generation. fd-based mutations (`fchmod`/`fchown`/
`ftruncate`) and inode aliasing (`unlink` of a hardlink, `link`) leave stale `st_mode`/`st_nlink`
because eviction is by exact path only. Any fd-based metadata write must recover+evict the path (or
drop the entry); `st_nlink` for nlink>1 inodes must not be served from this cache.
