# Fix: non-PIE `fork()`+`execve()` crash (the GCC toolchain segfault)

**Status:** design, ready to implement. **Owner:** JIT runtime. **Gate:** cross-engine matrix 240
green (PIE must not regress) **and** the `compile` group XPASSes on `LinuxAarch64`.

This is the #1 reliability bug: running the gcc-14 driver inside a container (`gcc -> cc1 -> as ->
ld -> run`) segfaults under the aarch64 JIT. It is fully root-caused; what follows is the concrete,
file:line-level plan and the reasoning that makes it safe for the PIE common case.

---

## (a) Mechanism — re-confirmed

The guest gcc driver `fork()`s and `execve()`s its sub-tools (`cc1`, `as`, `ld`, and the freshly
linked `a.out`). The sub-tools the GNU toolchain produces are **non-PIE `ET_EXEC`** images: they are
linked at a *fixed* low virtual address (the GNU `ld` aarch64/x86_64 default text base **`0x400000`**)
and contain **un-relocated absolute references** (absolute code targets *and* absolute data refs) baked
at that link vaddr. A non-PIE binary expects to be loaded *exactly* there.

On macOS that address is unusable: the host Mach-O's **`__PAGEZERO`** segment reserves the entire low
4 GiB (`0x0 .. 0x1_0000_0000`) as an unmapped guard, so `mmap(NULL, …)` can never return `0x400000`.

The loader copes by **biasing** the image high:

- `load_elf` (`dd-jit/src/runtime/os/linux/elf.c:73`) maps the image with `mmap(NULL, span, …)`, then
  computes `bias = base - basepage` (`elf.c:79`) and copies each `PT_LOAD` to `v + bias` (`elf.c:85`).
- For a non-PIE (`etype == 2`) it records the un-biased link span + bias so the dispatcher can
  patch *code* jumps (`elf.c:80`): `g_nonpie_lo/hi/bias`.
- The dispatcher (`dd-jit/src/runtime/jit/dispatch.c:51`) redirects any guest PC that lands in the
  un-biased low link span into the biased copy: `if (g_nonpie_lo && pc >= lo && pc < hi) pc += bias;`.

This patches **absolute code jumps** but does **nothing for absolute data loads/stores**: a `ldr`/`str`
to the baked low link address `0x4xxxxx` is a real memory access, not a dispatch event, so it hits the
unmapped low region and faults. On a *fresh* exec the inherited address space is sparse and the bias
happens to keep things working often enough; **post-`fork`** the child inherits the parent's **dense**
layout (heap at `mmap(NULL)`, stack, the 256 MiB brk arena, libc maps), so the new image's biased base
and its absolute refs collide / land on unmapped low pages and **SIGSEGV**.

Captured signature (`CRASHDBG=1`; the fork-child clears the Mach port at
`dd-jit/src/runtime/os/linux/service.c:1844-1846` so the inherited POSIX `diag_crash` handler at
`dd-jit/src/runtime/targets/linux_aarch64.c:80-96` reports it):

```
[CRASH] sig=11 fault=0x00000000004xxxxx pc=0x00000000004xxxxx tid=0x0
```

The tell is **`pc == fault` and both in `0x4xxxxx`** — the guest is executing/dereferencing at its
*un-biased* non-PIE link vaddr, i.e. an absolute ref the bias never relocated. (Contrast the two
*unrelated* fork crashes already documented: the busybox W^X/`MAP_JIT` fault has
`fault≈0x10xxxxxxx` *in the code cache* — `docs/design/research-busybox-crash.md:18,64`; the redis
`mprotect` fault is on the main thread with no fork — `docs/design/research-realsw-crashes.md:38-44`.
Neither is this bug.)

### Why the landed mitigations are insufficient

Already in tree: the execve **address-space teardown** (`gmap_reset_all()` at `service.c:1903`, backed
by the `g_gmap` registry in `dd-jit/src/runtime/os/linux/container/vfs.c:66-81`) and the **dispatcher
PC-redirect** (`dispatch.c:51`). Teardown removes the *parent's* maps so the child re-loads into a
clean layout; the redirect advances a code-jump fault into a data-ref fault. Together they convert an
early crash into the residual `pc==fault==0x4xxxxx` data-ref crash above. They cannot fix it because
the image is still **biased off** its link vaddr — the only true fix is to **pin the non-PIE at its
link vaddr so `bias == 0`** and every baked absolute ref resolves natively.

### Why the naive one-liner failed

`-pagezero_size 0x1000` alone (PLAN "fully fixes it but broke the PIE common case") shrinks `__PAGEZERO`
so the non-PIE *can* be pinned low — but it also makes the **low region available to `mmap(NULL,…)`**.
Suddenly the PIE image/interp, heap, and stack (all `mmap(NULL)`) start coming back at *low* addresses,
perturbing the layout the PIE world implicitly depends on and regressing the matrix (PLAN: PIE 43/195
broke). The fix below frees the low region for the non-PIE **and** keeps every other mapping high.

---

## (b) The plan

Three coordinated changes: shrink `__PAGEZERO`, pin the non-PIE `ET_EXEC` at its link vaddr, and force
**every other** guest mapping to a HIGH hint via a monotonic cursor.

### B.1 — `-pagezero_size 0x400000` in the clang link line

`dd-jit/build.rs:35`, the JIT link command:

```rust
"clang -O2 -o {bin} {tu} && codesign -s - --entitlements {ent} -f {bin}"
//        ^ add: -Wl,-pagezero_size,0x400000
"clang -O2 -Wl,-pagezero_size,0x400000 -o {bin} {tu} && codesign …"
```

**Value choice — `0x400000` (4 MiB), not `0x1000`:**

| `pagezero_size` | NULL-deref guard covers | Frees for non-PIE | Verdict |
|---|---|---|---|
| `0x100000000` (default 4 GiB) | all of low 4 GiB | nothing — bug unfixable | status quo |
| `0x400000` (4 MiB) | `0x0 .. 0x400000` | `0x400000` upward | **recommended** |
| `0x4000` (16 KiB, arm64 min) | `0x0 .. 0x4000` | `0x4000` upward | weakest guard |

`0x400000` is the sweet spot: it leaves a **4 MiB NULL-guard** (catches every realistic NULL deref —
a `NULL->field` with offset < 4 MiB still faults, for host *and* guest code) while freeing **exactly**
the canonical GNU-`ld` non-PIE base `0x400000` and everything above it. Note arm64 macOS pages are
16 KiB, so the value must be 16 KiB-aligned (`0x400000` is); `-pagezero_size 0x1000` from the PLAN
experiment is below page granularity and is rounded by the linker — prefer the explicit `0x400000`.

Apply the same flag to **only** the `linux_aarch64` (and later `linux_x86_64`) JIT target; `jitdarwin`
and `darwinjail` are unaffected (they don't bias guests).

> Caveat: this assumes the non-PIE links at `>= 0x400000` (the GNU default). The loader **guards**
> against a lower base at runtime (B.2): if `basepage < PAGEZERO_END`, it falls back to the existing
> bias+dispatch-redirect path instead of `MAP_FIXED`, so a weird binary degrades to today's behavior
> rather than failing the `MAP_FIXED`.

### B.2 — `MAP_FIXED` at the link vaddr for `ET_EXEC` in `load_elf`

`dd-jit/src/runtime/os/linux/elf.c:73`, replace the unconditional `mmap(NULL, …)` base for the
non-PIE case. Sketch:

```c
#define PAGEZERO_END 0x400000ull            // must match build.rs -pagezero_size
uint8_t *base;
if (etype == 2 && basepage >= PAGEZERO_END) {
    // Non-PIE: pin at the link vaddr so bias==0 and baked absolute refs resolve natively.
    base = mmap((void *)basepage, span, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (base == MAP_FAILED) {               // low region still occupied -> fall back to bias path
        base = hi_mmap(span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        // (g_nonpie_* set below, dispatcher redirect stays active)
    }
} else {
    // PIE image + the dynamic interp + everything else: HIGH hint (see B.3).
    base = hi_mmap(span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
}
```

Then `bias = (uint64_t)base - basepage` (`elf.c:79`) is **0** for the pinned non-PIE, so:

- the `PT_LOAD` copies (`elf.c:85`) write to the true link vaddr,
- `out->entry = e_entry + 0` is the native entry,
- the `g_nonpie_*` redirect (`elf.c:80`, `dispatch.c:51`) becomes a **no-op** (`pc += 0`). Keep
  setting it only on the `MAP_FAILED` fallback path; on the pinned path leave `g_nonpie_lo == 0` so
  the dispatcher skips the check entirely.

`gmap_add((uint64_t)base, span)` (`elf.c:78`) is unchanged — the pinned image is still tracked for the
next execve teardown.

### B.3 — a high-hint allocator wired into EVERY non-fixed guest mmap site

Add a monotonic cursor + helper next to the gmap registry in
`dd-jit/src/runtime/os/linux/container/vfs.c` (it is `#include`d at `linux_aarch64.c:55`, **before**
`service.c:61`, `dispatch.c:63`, and `elf.c:65`, so it is visible to every site below, and `jit_run`
in `linux_aarch64.c` compiles after all includes):

```c
// Hand every NON-fixed guest mapping a HIGH hint so the freed low region (< PAGEZERO_END after the
// __PAGEZERO shrink) is reserved exclusively for a pinned non-PIE ET_EXEC. Monotonic, never reused;
// macOS honors the hint when free, else picks nearby-high (still != low). Single global; guest mmaps
// are serialized under the dispatcher path. Start well clear of the non-PIE link span.
static _Atomic uint64_t g_hi_cursor = 0x1_0000_0000ull;   // 4 GiB
static void *hi_mmap(size_t len, int prot, int flags, int fd, off_t off) {
    size_t step = (len + 0xFFFFull) & ~0xFFFFull;          // 16 KiB granule
    uint64_t hint = atomic_fetch_add(&g_hi_cursor, step + 0x1_0000_0000ull); // +4 GiB gap, avoid overlap
    void *r = mmap((void *)hint, len, prot, flags, fd, off);
    if (r == MAP_FAILED && !(flags & MAP_FIXED))           // hint rejected -> let the kernel choose
        r = mmap(NULL, len, prot, flags, fd, off);
    return r;
}
```

> The hint is advisory (no `MAP_FIXED`): if it collides, macOS ignores it and returns *some* address;
> the only address we must avoid is the low non-PIE region, and a hint at >= 4 GiB never lands there.
> The fallback to `mmap(NULL)` is a safety net — with the `__PAGEZERO` *guard reservation* (§d) even
> that fallback can't return low.

Route **every non-`MAP_FIXED` guest mapping** through `hi_mmap`:

| # | Site | File:line | What | Change |
|---|------|-----------|------|--------|
| 1 | PIE image / interp base | `elf.c:73` | the `mmap(NULL, span, …)` ELF base | `hi_mmap(…)` (B.2; non-PIE pinned via `MAP_FIXED`) |
| 2 | guest stack | `elf.c:118` | `build_stack` 8 MiB stack | `hi_mmap(SZ, …)` |
| 3 | initial heap/brk | `linux_aarch64.c:349` | `jit_run` 256 MiB brk arena | `hi_mmap(256u<<20, …)` |
| 4 | post-execve heap/brk | `service.c:1928` | new image's 256 MiB brk arena | `hi_mmap(256u<<20, …)` |
| 5 | guest `mmap(addr==NULL)` | `service.c:1610` | syscall 222 anon/file, **only when `a0==0` and not `MAP_FIXED`** | `a0 ? mmap(a0,…) : hi_mmap(len,…)` |
| 6 | guest `mremap` | `service.c:1578` | syscall 216 copy-grow `mmap(0,…)` | `hi_mmap((size_t)a2, …)` |

Notes per site:

- **Site 5 is the load-bearing one.** Case 222 currently calls `mmap((void *)a0, …)` (`service.c:1610`)
  with the guest's `a0` hint — which is `NULL` for the common `mmap(NULL,…)` and previously relied on
  `__PAGEZERO` to keep the kernel from ever choosing low. With `__PAGEZERO` shrunk, an `a0==0` request
  *must* be redirected high. Keep `MAP_FIXED` (`a3 & 0x10`) requests and non-NULL hints exactly as-is
  (the guest may legitimately ask for a specific address); only `a0==0 && !(a3&0x10)` takes `hi_mmap`.
  The existing `guard` tail, charge accounting, and `prot` upgrade logic are unchanged — `hi_mmap`
  only replaces the address selection.
- **Sites 3 & 4 (brk arenas)** are the densest maps and the ones that, post-fork, used to crowd the
  biased non-PIE; high-hinting them is what keeps the non-PIE's low region clear after teardown+reload.
- **Transient loader maps stay `mmap(NULL)`:** the read-only file maps in `elf_interp` (`elf.c:22`)
  and `load_elf` (`elf.c:52`) are `munmap`ped before the guest runs and never overlap the guest's
  address space — leave them as plain `mmap(NULL, …)`. (Harmless to high-hint, but unnecessary.)
- The `mremap` at site 6 is a partial implementation (copy into a fresh map); high-hinting it keeps
  the post-mremap buffer out of the low region for consistency.

`PAGEZERO_END` should be a single shared constant (define once in `vfs.c` near `hi_mmap`, used by
`elf.c` B.2). Keep it in sync with the `build.rs` flag — add a one-line comment in both pointing at the
other, or (nicer) a `_Static_assert`-style runtime check at startup that `sysctl`/`getsegmentbyname`
reports the expected `__PAGEZERO` vmsize.

---

## (c) Why the PIE world is unperturbed

In a PIE-only run (the matrix's common case — `compat`, `libc`, `threads`, `realsw`, every container
that isn't a non-PIE `ET_EXEC`):

- The image, interp, stack, both brk arenas, and every `mmap(NULL)` go through `hi_mmap`, landing at
  **>= 4 GiB** — exactly where `mmap(NULL)` put them under the old 4 GiB `__PAGEZERO` (the kernel
  always allocated above `__PAGEZERO`, i.e. >= 4 GiB). So the PIE layout is **the same shape as today**.
- `g_nonpie_lo == 0` for a PIE main image, so the dispatcher redirect (`dispatch.c:51`) is inert —
  unchanged from today.
- The freed low region `0x400000 .. 0x1_0000_0000` is touched by **nothing** in a PIE run (and is
  PROT_NONE-reserved per §d), so shrinking `__PAGEZERO` is invisible to it.

Only a **non-PIE `ET_EXEC`** uses the low region, and it uses it *exactly* (pinned `MAP_FIXED` at its
link span). This is precisely the "small `__PAGEZERO` + force every other mapping high" fix the PLAN
calls achievable, and it dodges the `-pagezero_size 0x1000`-alone regression because `mmap(NULL)` is no
longer how any guest mapping is placed — `hi_mmap` is.

---

## (d) NULL-page deref + guard-page implications of a smaller `__PAGEZERO`

Shrinking `__PAGEZERO` from 4 GiB to 4 MiB has two consequences:

1. **Host NULL-deref detection.** The JIT host binary loses its 4 GiB→4 MiB guard. A host C bug that
   derefs `NULL + k` now faults only for `k < 4 MiB` (still essentially all real NULL bugs) and for
   larger `k` could read/write into the low region instead of trapping. **Mitigation:** keep
   `pagezero_size = 0x400000` (not `0x4000`) so the guard stays 4 MiB — comfortably larger than any
   plausible struct/array offset off NULL.

2. **The freed `0x400000 .. 0x1_0000_0000` must not silently absorb stray accesses** (host or guest)
   when no non-PIE is loaded. **Mitigation — a PROT_NONE low-guard reservation.** At `jit_run`
   startup (before any guest map; `linux_aarch64.c` after the env block ~line 201) reserve the whole
   freed low band:

   ```c
   // Re-create the NULL/low guard the shrunk __PAGEZERO no longer provides: reserve the freed low band
   // PROT_NONE so stray low accesses fault and mmap(NULL) can never wander into it. A pinned non-PIE
   // (load_elf B.2) punches its exact span out with MAP_FIXED; the rest stays trapping.
   mmap((void *)PAGEZERO_END, 0x1_0000_0000ull - PAGEZERO_END,
        PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
   ```

   - For a **PIE run** this restores a full `0 .. 4 GiB` trap (4 MiB hardware `__PAGEZERO` + the
     PROT_NONE reservation) — NULL-deref detection is *as strong as before*.
   - For a **non-PIE run**, `load_elf`'s `MAP_FIXED` (B.2) overwrites exactly the image span within the
     reservation (MAP_FIXED replaces the PROT_NONE pages); the surrounding low band stays trapping.
   - On execve teardown (`gmap_reset_all`, `service.c:1903`) the *image* span is unmapped; re-establish
     the PROT_NONE reservation over it (or simply re-`mmap(MAP_FIXED, PROT_NONE)` the whole band before
     the next `load_elf`) so a subsequent PIE exec is guarded again.

   This makes the `hi_mmap` `mmap(NULL)` fallback provably safe (the low band is occupied → the kernel
   can't return it) and preserves NULL-deref ergonomics for development.

---

## (e) Risks + the test gate

**Risks**

- **R1 — PIE layout drift.** If any PIE `mmap(NULL)` site is *missed* and `__PAGEZERO` is shrunk, that
  site could now return low and corrupt the layout. *Mitigation:* the §d PROT_NONE reservation makes a
  missed site fault loudly (MAP_FAILED → `hi_mmap` fallback also blocked low) rather than silently
  regress; the site table in B.3 is exhaustive (audited against `grep -n 'mmap(' os/linux elf.c
  targets`).
- **R2 — non-PIE base below `0x400000`.** A non-standard non-PIE could link below the guard.
  *Mitigation:* the `basepage >= PAGEZERO_END` check in B.2 falls back to the bias+redirect path
  (today's behavior) — no regression, just not improved.
- **R3 — guest asks for a specific low address** via `mmap(addr<4MiB, MAP_FIXED)`. Extremely rare;
  it would hit the PROT_NONE reservation / `__PAGEZERO`. Same as a real Linux process mapping over its
  own low pages — acceptable; could special-case later.
- **R4 — host NULL+large-offset bug** writing into `[4MiB,4GiB)` when a non-PIE occupies part of it.
  Bounded by R1's loud-fault property for the unoccupied band; the occupied band is the guest's, and a
  host write there is already-a-bug territory. Net guard is strictly stronger than `-pagezero_size
  0x1000` alone.
- **R5 — `mremap`/large maps exceeding the 4 GiB step.** `hi_mmap` adds `step + 4 GiB`; a single map
  > 4 GiB still fits (step grows with len). Cursor is 64-bit, monotonic; exhaustion is not reachable in
  a container lifetime.

**Test gate (must all hold):**

1. `make test` — cross-engine matrix **240 green**, no regressions. Specifically the **PIE common case
   must not drop** (PLAN's regression was PIE 43/195 broken; that must read 0 broken).
2. The **`compile` group XPASSes** on `Engine::LinuxAarch64`: `compile/hello`, `compile/c-primes`,
   `compile/cpp-stl` (`dd-tests/src/cases/mod.rs:187-196`) are `.xfail(&[Engine::LinuxAarch64])` today;
   the fix should flip them to XPASS. Once stable, **drop the `.xfail`** (`mod.rs:186`) so a future
   regression fails the build.
3. `CRASHDBG=1` repro: run gcc-14 in the `gcc-bundle` rootfs under load (8–16 procs × several rounds,
   as in `research-busybox-crash.md:157`); the `[CRASH] sig=11 fault=0x4xxxxx pc=0x4xxxxx` line must be
   **gone**.
4. NULL-guard sanity: a deliberate host/guest `*(volatile int*)0 = 1` and `*(int*)0x100000` still
   SIGSEGV (4 MiB guard intact).
5. `make test-realsw` / `make test-docker` unchanged (PIE workloads — redis/postgres/perl/sqlite are
   all PIE per `research-realsw-crashes.md:6`, so they exercise the high-hint path).

---

## (f) Phased rollout

**Phase 0 — flag-gated, no `__PAGEZERO` change (de-risk the allocator).**
Land `hi_mmap` + the §d reservation logic behind `getenv("DD_HI_MMAP")`, keeping the default 4 GiB
`__PAGEZERO`. With the big `__PAGEZERO` still present, `mmap(NULL)` already returns high, so the cursor
should produce a layout *identical in shape* to today. Run the full matrix with the flag **on** and
**off** — both must be 240 green. This proves the high-hint plumbing is correct in isolation, before
touching the pagezero guard. (Sites 1–6 wired; B.2 `MAP_FIXED` and the build.rs flag NOT yet.)

**Phase 1 — shrink `__PAGEZERO` + pin the non-PIE, still flag-gated.**
Add `-Wl,-pagezero_size,0x400000` (B.1), the B.2 `MAP_FIXED` pin, and the §d PROT_NONE reservation —
all under `DD_HI_MMAP`/`DD_LOW_NONPIE`. (Note: the linker flag is build-time, so "gated" here means a
second build variant or a cfg, not a pure runtime env; gate the *behavioral* pieces — reservation,
`MAP_FIXED` pin, hi_mmap on — at runtime and ship the shrunk-pagezero binary only in the variant under
test.) Matrix + `CRASHDBG` repro + the `compile` group with the flag on. Expect `compile` XPASS and PIE
unchanged.

**Phase 2 — default on, remove the gate.**
Once Phase 1 is green across `make test`, `test-realsw`, `test-docker`, and the soak fork/exec cases
(`soak/forkchurn`), make the shrunk `__PAGEZERO` + `hi_mmap` the default, delete the env gate, and
**remove the `compile` `.xfail`** (`dd-tests/src/cases/mod.rs:186`). Update `docs/PLAN.md`: move the
"fork()+execve() non-PIE crash" bullet from *Deep bugs* (line 34) and drop the *Platform limitations*
caveat (line 82) — it is now solved, not a platform limit.

**Phase 3 — extend to `linux_x86_64`.**
The same `__PAGEZERO`/bias mechanics apply to the jit86 target (non-PIE x86 `ET_EXEC` at `0x400000`).
Add the build.rs flag + `hi_mmap` to the x86 mmap sites and re-run the jit86 matrix.

---

## File/line change summary

| File:line | Change |
|---|---|
| `dd-jit/build.rs:35` | add `-Wl,-pagezero_size,0x400000` to the `linux_*` clang link line (Phase 1) |
| `dd-jit/src/runtime/os/linux/container/vfs.c` (near `:66-86`) | add `PAGEZERO_END`, `g_hi_cursor`, `hi_mmap()` |
| `dd-jit/src/runtime/os/linux/elf.c:73` | `MAP_FIXED` pin for `ET_EXEC` (`basepage>=PAGEZERO_END`), else `hi_mmap` (B.2) |
| `dd-jit/src/runtime/os/linux/elf.c:80` | set `g_nonpie_*` **only** on the bias-fallback path |
| `dd-jit/src/runtime/os/linux/elf.c:118` | `build_stack` stack via `hi_mmap` |
| `dd-jit/src/runtime/targets/linux_aarch64.c:~201` | install the §d PROT_NONE low-guard reservation at startup |
| `dd-jit/src/runtime/targets/linux_aarch64.c:349` | initial brk arena via `hi_mmap` |
| `dd-jit/src/runtime/os/linux/service.c:1578` | `mremap` copy via `hi_mmap` |
| `dd-jit/src/runtime/os/linux/service.c:1610` | case 222: `a0==0 && !MAP_FIXED` → `hi_mmap` |
| `dd-jit/src/runtime/os/linux/service.c:1903-1904` | re-establish §d reservation over the torn-down image span before reload |
| `dd-jit/src/runtime/os/linux/service.c:1928` | post-execve brk arena via `hi_mmap` |
| `dd-tests/src/cases/mod.rs:186` | drop `.xfail(&[Engine::LinuxAarch64])` from `compile` (Phase 2) |
| `docs/PLAN.md:34,82` | move the bug out of *Deep bugs* / *Platform limitations* (Phase 2) |
