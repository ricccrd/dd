# FIX — the x86_64 (jit86) `busybox sort` SIGSEGV

Status: **root-caused with a minimal differential reproducer; fix scoped (one handler).**
Date: 2026-06-27. Read-only investigation against the already-built engine
(`target/release/build/ddjit-d209c70febc03a6a/out/ddjit-linux_x86_64`) driven through the `mac`
bridge; differential oracle = `qemu-x86_64` (host `/usr/bin/qemu-x86_64`).

## TL;DR

`busybox sort` does **not** depend on input size — it SIGSEGVs on a **1-line** sort just as it does
on 100 000 lines. The fault is a single mistranslated instruction:

```
49 90    xchg %rax,%r8
```

`0x90` is `XCHG eAX, r` ; with **REX.B** the encoded register is `r8` (`(0x90-0x90)|(rexB<<3) = 8`),
so `49 90` is a real **`xchg r8, rax`** swap — not a NOP. The x86 frontend decodes `0x90` as a NOP
**without checking `REX.B`** (`translate.c:749`), so the swap is dropped. In busybox's array
allocator (a `malloc` whose result is moved into `r8` via `xchg`), dropping the swap leaves `r8`
holding a stale value; the subsequent `add %r8,%rbp ; mov %rbp,%rdi ; rep stosb` then memsets a
wild/low address → SIGSEGV.

## (a) Exact repro

The x86 busybox + rootfs the matrix's `containersw`/`busybox` cases use for the x86 arch:
`/Users/x/dd/poc/fixtures-x86/alpine` (`/bin/busybox` = ELF x86-64 PIE, musl).

The jit86 crash diagnostics are gated by **trigger files** (the `mac` bridge drops env), not
`CRASHDBG`/`JT`: `touch /Users/x/dd/poc/runtime/jit86/FAULT_ON` installs `jit86_faulth`
(`frontend/x86_64/elf.c:196`) and redirects stderr to `…/runtime/jit86/trace.log`.

```
JIT=/Users/x/dd/dd/target/release/build/ddjit-d209c70febc03a6a/out/ddjit-linux_x86_64
RF=/Users/x/dd/poc/fixtures-x86/alpine
touch /Users/x/dd/poc/runtime/jit86/FAULT_ON
mac bash -lc "$JIT --rootfs $RF /bin/busybox sort /tmp/in3.txt"   # /tmp/in3.txt = 'c\nb\na\n' in the rootfs
```

Notes on reproduction:
* A piped stdin (`printf '…' | … sort`) through the `mac` bridge yields **empty** stdin, so sort
  exits 0 without allocating — this is why the bug hides. Use a **file argument** (or real stdin)
  so sort actually processes lines.
* `busybox echo` / `cat` / `wc` on the same rootfs all succeed — only `sort`'s allocator path hits
  the `xchg`.

**Size threshold = none.** Every size faults at the *same* instruction (ASLR slides only the high
bits; the low bits are always `…5cd`):

| input            | rc  | curpc (low) |
|------------------|-----|-------------|
| 1 line           | 133 | `…5cd`      |
| 2 lines          | 133 | `…5cd`      |
| 3 lines (`c b a`)| 133 | `…5cd`      |
| 50 lines         | 133 | `…5cd`      |
| 100 000 lines    | 133 | `…5cd`      |

`rc=133` is `jit86_faulth`'s `_exit(133)`. Without `FAULT_ON` the process dies with host `rc=139`
(SIGSEGV).

Canonical `[FAULT]` capture (3-line input):

```
[LOADED] …/bin/busybox base=101bf4000 span=d0000 end=101cc4000 entry=101bfaed1
[FAULT] sig=11 addr=0x1  guest rip(last blk)=101c915cd  curpc=101c915cd prevblk=101ceeb28 ibranch_src=101bfc406
  rax=1035cb0c0  rcx=ffffffa0  rdx=84  rbx=40
  rsp=11c77fcc0  rbp=0  rsi=1035cb34c  rdi=9      <- rdi (rep-stos dest) ≈ wild; rcx (count) huge
  bytes@rip: 8d 4b 01 49 90 41 0f af cc 31 c0 4c 01 c5 48 89 ef f3 aa 5b …
```

`curpc 0x101c915cd − base 0x101bf4000 = 0x9d5cd`. The `bytes@rip` are the guest x86 bytes of the
faulting block and contain `49 90` (xchg) at `+3` and `f3 aa` (rep stosb) at `+17`.

Disassembly of the block (`x86_64-linux-gnu-objdump -d busybox`, file offset == vaddr for this PIE):

```
9d59f: push %r12
9d5a3: 49 89 f8   mov  %rdi,%r8          ; r8 = arg
9d5a8: 6a 01;5b   mov  $1,%rbx
9d5ab: d3 e3      shl  %cl,%ebx
9d5c8: e8 ..      call <malloc@plt>       ; rax = malloc(...)
9d5cd: 8d 4b 01   lea  0x1(%rbx),%ecx     ; <- block entry the fault reports
9d5d0: 49 90      xchg %rax,%r8           ; *** r8 = malloc result, rax = old r8 ***
9d5d2: 41 0f af cc imul %r12d,%ecx        ; rcx = count
9d5d6: 31 c0      xor  %eax,%eax          ; fill byte 0
9d5d8: 4c 01 c5   add  %r8,%rbp           ; rbp = base + r8(malloc)
9d5db: 48 89 ef   mov  %rbp,%rdi          ; dest
9d5de: f3 aa      rep stos %al,(%rdi)     ; memset -> faults when r8 is stale
9d5e1: 49 90      xchg %rax,%r8           ; symmetric swap-back (also dropped)
```

This is the classic compiler idiom for *preserving `r8` across a call*: `xchg %rax,%r8` moves the
return value into `r8` (and stashes the saved value in `rax`); a second `xchg` restores it. Drop the
swaps and `r8` (hence `rbp`/`rdi`) is garbage → wild `rep stosb`.

## (b) Differential divergence (oracle vs jit)

A 9-byte freestanding reproducer isolates the single instruction (no libc, exit code = the result):

```asm
_start:
    mov $0x42, %rax
    mov $0x99, %r8
    .byte 0x49,0x90          /* xchg %rax,%r8  -> expect rax=0x99, r8=0x42 */
    mov %r8, %rdi            /* exit(r8 & 0xff) */
    mov $60, %rax
    syscall
```
`x86_64-linux-gnu-gcc -nostdlib -static -o xchg_free xchg_free.S`

| run                         | exit code | meaning                          |
|-----------------------------|-----------|----------------------------------|
| **oracle** `qemu-x86_64`    | **66** = 0x42 | swap happened (r8 = old rax)  |
| **jit86** x86 engine        | **153** = 0x99 | swap **dropped** (r8 unchanged) |

That is the exact divergence: the very first (and only) instruction under test, `49 90`, leaves
`r8` unchanged in the JIT while the oracle swaps it. (In the real busybox block this manifests two
instructions later as the wild `rep stosb`.)

## (c) Culprit instruction + handler (why it's wrong)

`dd-jit/src/runtime/frontend/x86_64/translate.c:748-752`, inside `translate_block`:

```c
// ---- nop (90) / xchg rAX, rN (91-97) ----
if (op == 0x90 && !I.rep) {
    gpc = next;
    continue;
} // (F3 90 = pause -> also nop)
```

It returns NOP for **any** `0x90` (modulo the `F3`/pause prefix) and never consults `I.rexB`. But
`0x90+rd` is `XCHG rAX, r`; the comment on the very next handler
(`translate.c:1107-1111`) gets the register right for `0x91-0x97`:

```c
if (op >= 0x91 && op <= 0x97) {
    int r = (op - 0x90) | (I.rexB << 3);   // <-- 0x90 is wrongly excluded from this
    e_mov_rr(19, RAX, sf);
    e_mov_rr(RAX, r, sf);
    e_mov_rr(r, 19, sf);
    ...
```

So `0x90` is the **one** member of the `XCHG rAX,r` family that ignores `REX.B`. With `REX.B`,
`r = 0 | (1<<3) = 8 = r8`, i.e. `xchg r8, rax` — dropped. (The decoder *does* populate `I.rexB`,
`decode.c:159`; only this handler ignores it.) The second `if (op == 0x90)` NOP at `translate.c:1103`
is unreachable for this case today because `:749` `continue`s first; it should be hardened too (see
fix).

`66 90` (`xchg ax,ax`) and a bare `90` stay NOPs because `rexB == 0`. `F3 90` (PAUSE) is already
excluded by `!I.rep`.

## (d) The fix

Make `0x90` honor `REX.B` — emit the `xchg` when `rexB` is set, NOP otherwise. Self-contained edit
at `translate.c:749` (uses the same `sf` operand-size flag and scratch `x19` as the `0x91-0x97`
handler; `sf = I.opsize == 8`, so `49 90` → 64-bit swap, `41 90` → 32-bit swap that zero-extends —
correct x86 semantics):

```c
// ---- nop (90) / xchg rAX, rN (90-97) ----
if (op == 0x90 && !I.rep) {
    if (I.rexB) {                      // 41/49 90 = xchg r8(d), rAX  (NOT a nop)
        e_mov_rr(19, RAX, sf);
        e_mov_rr(RAX, 8, sf);          // r = (0x90-0x90)|(rexB<<3) = 8 = r8
        e_mov_rr(8, 19, sf);
    }
    gpc = next;                        // plain 90 / 66 90 / F3 90(pause): still NOP
    continue;
}
```

(Equivalently: change the range handler at `:1107` to `op >= 0x90 && op <= 0x97`, and at `:749`
add `&& !I.rexB` so `49 90` falls through to it — but that also requires removing the second
unconditional `0x90` NOP at `:1103`. The inline form above is the smaller, lower-risk change and
needs no other edits.)

## (e) Test gate

Two complementary gates:

1. **Opcode-level (deterministic, no rootfs):** add the freestanding `xchg_r8` fixture to the `x86`
   group in `dd-tests/src/cases/mod.rs`, asserting the oracle exit code:

   ```rust
   fixture("xchg-r8", &[(Engine::LinuxX86_64, "guests/x86/xchg_r8")]).exit(66),
   ```
   built from the 9-byte `.S` above (`gcc -nostdlib -static`). Pre-fix → exit 153; post-fix → 66.
   This is the canonical regression for "`0x90` ignores REX.B".

2. **End-to-end differential (`busybox/sort-large`):** run `/bin/busybox sort <file>` from the
   x86 alpine rootfs (`poc/fixtures-x86/alpine`) under the x86 engine and diff stdout+rc against
   `qemu-x86_64 /bin/busybox sort <file>` on a large shuffled input (e.g. `seq 1 100000 | shuf`).
   This requires the harness to (i) allow `Bin::InRootfs` on `Engine::LinuxX86_64` and (ii) resolve
   an x86 rootfs (today `rootfs_path` is aarch64-only, `dd-tests/src/lib.rs:173`) — small plumbing,
   but it locks in the real-world path, not just the isolated opcode.

## Key references

* Bug: `dd-jit/src/runtime/frontend/x86_64/translate.c:749` (`0x90` NOP ignores `REX.B`); correct
  sibling handler `:1107-1111`; redundant second NOP `:1103`; `REX.B` decode `decode.c:159`.
* Fault handler / trigger files: `frontend/x86_64/elf.c:196` `jit86_faulth`,
  `targets/linux_x86_64.c:162` (`FAULT_ON`/`TRACE_ON`/`ITRACE_ON` under
  `/Users/x/dd/poc/runtime/jit86/`).
* Guest: `poc/fixtures-x86/alpine/bin/busybox`; faulting function `+0x9d59f` (array allocator),
  faulting block `+0x9d5cd`.
* Reproducers (this investigation): `xchg_free.S` (differential, exit 66 vs 153);
  inputs in `poc/fixtures-x86/alpine/tmp/{in1,in2,in3,in50,big}.txt`.
