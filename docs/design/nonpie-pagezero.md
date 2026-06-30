# Non-PIE low-address hosting — the `__PAGEZERO` platform limitation

**Read before touching the non-PIE path in `frontend/{x86_64,aarch64}/translate.c` or `os/linux/elf.c`.**
This is the highest future-conflict area.

## The limitation (macOS, immovable)

A GNU-toolchain non-PIE `ET_EXEC` is linked at a *fixed* low vaddr (`0x400000`) with un-relocated
absolute references. On arm64 macOS the engine's main Mach-O carries a **mandatory ~4 GiB
`__PAGEZERO`** reserving `0x0 … 0x1_0000_0000` unmapped. So the guest's baked low addresses are
**unmappable in-process**, and nothing recovers them:

- `-pagezero_size` below ~4 GiB on an arm64 *main* executable → "Malformed Mach-O" / SIGKILL.
  (Shrinking it is a **proven dead-end** — see `fix-nonpie-crash.md`.)
- A `posix_spawn`'d helper is itself a main exe → its own 4 GiB pagezero re-covers the low region.
- `mmap(MAP_FIXED)`, `mach_vm_map/allocate`, `MAP_32BIT` cannot return `0x4xxxxx`. macOS has no
  `mmap_min_addr` knob to lower.

Conclusion: the guest must stay mapped **high**; correctness comes from address folding, never from
acquiring low memory. (QEMU `guest_base` and blink's linear skew solve the identical problem the same
way; blink proves it works on Apple Silicon.) Native low-vaddr load is **wontfix**.

## The invariant future devs must not break

Guest register and pointer **values stay LOW** (the guest vaddr the binary expects); the real bytes
live at `low + bias` in the high mapping. Therefore:

- `g2h(p) = p + bias` at **every** guest memory access and **every** syscall that dereferences a
  guest pointer; `h2g(p) = p − bias` on every host address returned to the guest (`mmap`/`brk`/
  `mremap` return path). `bias` (`g_nonpie_bias`) is nonzero **only** for a non-PIE `ET_EXEC`; PIE
  keeps `bias == 0` → identity → zero overhead, unchanged path. **Never regress the PIE matrix** when
  touching this: gate every fold/extra-stolen-register on `g_nonpie_lo != 0`.
- `adr/adrp` and baked absolute pointers already produce low values via `pcrel_base()` — keep it.

## Shipped state

Today the low mismatch is reconciled lazily: code PCs are redirected through the dispatcher
(`dispatch.c`); absolute data loads/stores fault and are served `+bias` by a `SIGSEGV` fixup
(`nonpie_fixup`). Correct but slow (one trap per low access; `cc1` ~400 s) and fragile post-`fork`
(dense child layout). The forward fix is an **eager translate-time bias-fold** (QEMU-style
register-held `guest_base`, mode-gated on non-PIE), keeping `nonpie_fixup` only as the
un-foldable-form safety net. Signals must report the fault addr in **guest (low)** space (`h2g`).
