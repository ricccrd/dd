# Rejected: shrink `__PAGEZERO` + `MAP_FIXED`-pin the non-PIE

**Do not retry.** Shrinking the engine's main-exe `__PAGEZERO` (e.g. `-Wl,-pagezero_size,0x400000`)
to pin a non-PIE `ET_EXEC` low cannot even load on arm64 macOS (Malformed Mach-O / SIGKILL), and the
`0x1000`-alone variant regressed the PIE matrix. The low 4 GiB is unrecoverable in-process. See
[`nonpie-pagezero.md`](nonpie-pagezero.md) for the limitation and the bias-fold that replaces it.
