//! abi — basics expansion (in-process JIT matrix). Owner: abi agent. Edit ONLY this file.
//! Builders: src(name,file).oracle()/.exit()/.out()/.has(); port(name,file) for cross-engine golden.
//! Keep this module compiling at all times (`cargo build -p dd-tests`).
//!
//! These groups stress the JIT's codegen/ABI corners — the instruction-translation surface that must be
//! byte-exact: integer overflow/wrapping, 64-bit & 128-bit mul/div/mod, all float/double ops + rounding
//! modes + NaN/inf + fma + long double, bit ops, varargs, recursion/tail-calls, jump tables, computed
//! goto, function-pointer/vtable indirect dispatch (IBTC), struct-by-value ABI (small/large/mixed/HFA/
//! returns), alloca/VLA, setjmp/longjmp, single-thread atomics, endian, sign-extension edges, and the
//! arg/return register ABI. Every case is `.oracle()`-checked: the JIT run's stdout+exit must equal the
//! same guest run natively (aarch64 direct, x86_64 via qemu) — so any per-arch divergence is caught
//! automatically. long double / HFA / struct-by-value legitimately differ BETWEEN arches, which is fine:
//! the oracle compares JIT-vs-native within one arch, never across.
#![allow(unused_imports)]
use crate::{group, src, port, fixture, in_rootfs, Case, Engine, Group};

fn s(name: &'static str, file: &'static str) -> Case { src(name, file).oracle() }

pub fn groups() -> Vec<Group> {
    vec![abi_int(), abi_float(), abi_vararg(), abi_control(), abi_indirect(), abi_struct(), abi_mem(), abi_args()]
}

/// Integer arithmetic: overflow/wrapping at every width, 64- & 128-bit mul/div/mod, shifts, rotates,
/// sign-extension, bit-count intrinsics, byte-swap, and carry/overflow-flag detection.
fn abi_int() -> Group {
    group("abi-int", vec![
        s("intwrap",   "ext_abi/intwrap.c"),            // signed/unsigned wrap at int/short/char/long edges
        s("mul64",     "ext_abi/mul64.c"),              // 64x64->128 product + mul-overflow flag
        s("divmod",    "ext_abi/divmod.c"),             // signed/unsigned 64-bit div & rem, negative trunc
        s("shifts",    "ext_abi/shifts.c"),             // logical vs arithmetic, variable shift amounts
        s("int128",    "ext_abi/int128.c"),             // __int128 mul/div/mod/shift/neg
        s("clmul",     "ext_abi/clmul.c"),              // widening mul sweep + add-with-carry
        s("signext",   "ext_abi/signext.c"),            // width-boundary sign/zero extension + mixed cmp
        s("rotate",    "ext_abi/rotate.c"),  // jit86: UNIMPL `D3 /0` ROL r/m32,CL (variable rotate)
        s("popcnt",    "ext_abi/popcnt.c"),  // jit86: __builtin_parityll wrong (par=1 vs 0)
        s("bswap",     "ext_abi/bswap.c"),              // bswap16/32/64
        s("overflow",  "ext_abi/overflow_builtin.c"),   // __builtin_*_overflow across signed/unsigned
        s("cmpchain",  "ext_abi/cmpchain.c"),           // compare -> conditional-select (csel/setcc)
        s("charmath",  "ext_abi/charmath.c"),// jit86: UNIMPL `0F F6` PSADBW (auto-vectorized loop)
    ])
}

/// Floating point: rounding modes, NaN/inf/-0, fma single-rounding, int<->fp conversion, long double,
/// classification/decomposition, min/max NaN-quieting, the full comparison-predicate matrix, and float32.
fn abi_float() -> Group {
    group("abi-float", vec![
        s("fp-round",    "ext_abi/fp_round.c"), // jit86: lrint ignores MXCSR rounding mode (acc 50 vs 42)
        s("fp-special",  "ext_abi/fp_special.c"),       // NaN/inf/-0 detect & propagate
        s("fp-fma",      "ext_abi/fp_fma.c"),           // fma/fmaf fused single-rounding
        s("fp-conv",     "ext_abi/fp_conv.c"),          // int<->float conversion + truncation edges
        s("longdouble",  "ext_abi/longdouble.c"),       // long double (80-bit x87 vs 128-bit; per-arch)
        s("fp-classify", "ext_abi/fp_classify.c"),      // fpclassify + frexp/ldexp/modf
        s("fp-minmax",   "ext_abi/fp_minmax.c"),        // fmin/fmax/fdim/copysign NaN semantics
        s("fp-cmp",      "ext_abi/fp_cmp.c"), // jit86: NaN unordered-compare predicate wrong (lt 36 vs 21)
        s("float32",     "ext_abi/float32.c"),          // single-precision transcendental + rounding
    ])
}

/// Varargs (the trickiest register-class ABI) and printf formatting breadth.
fn abi_vararg() -> Group {
    group("abi-vararg", vec![
        s("varargs-mixed", "ext_abi/varargs_mixed.c"),  // int/long/double/pointer interleaved
        s("varargs-float", "ext_abi/varargs_float.c"),  // many doubles -> va-area stack spill
        s("printf-fmt",    "ext_abi/printf_formats.c"), // flag/width/precision/length conversion matrix
    ])
}

/// Control flow: tail/mutual recursion, deep non-tail recursion, large jump tables, computed goto
/// (threaded code), deeply nested loops with break/continue, and boolean materialization/short-circuit.
fn abi_control() -> Group {
    group("abi-control", vec![
        s("tailcall",      "ext_abi/tailcall.c"),       // self + mutual tail recursion at depth
        s("deep-recursion","ext_abi/deep_recursion.c"), // 50k-deep frames + Ackermann
        s("bigswitch",     "ext_abi/bigswitch.c"),      // 128-case dense switch -> jump table
        s("computed-goto", "ext_abi/computed_goto.c"),  // labels-as-values dispatch
        s("nested-loops",  "ext_abi/nested_loops.c"),   // nested loops w/ break/continue (block-chaining)
        s("boolconv",      "ext_abi/boolconv.c"),       // !!x, &&/||, _Bool, short-circuit side effects
    ])
}

/// Indirect dispatch (IBTC / inline-cache stress): C vtables, a 16-target call site, and a libc qsort
/// comparator callback (indirect call from libc back into guest code).
fn abi_indirect() -> Group {
    group("abi-indirect", vec![
        s("vtable",   "ext_abi/vtable.c"),              // struct-of-fn-ptrs virtual dispatch
        s("fnptr-many","ext_abi/fnptr_many.c"),         // 16-way rotating indirect call
        s("qsort-cb", "ext_abi/qsort_cb.c"),            // qsort/bsearch comparator callback
    ])
}

/// Struct-by-value ABI: small (register-pair), large (memory/sret), mixed int+float (class splitting),
/// homogeneous float aggregates (HFA), struct returns of varying size, and union/bitfield punning.
fn abi_struct() -> Group {
    group("abi-struct", vec![
        s("struct-small", "ext_abi/struct_small.c"),    // <=16B structs in registers
        s("struct-large", "ext_abi/struct_large.c"),    // >16B struct via memory/hidden ptr
        s("struct-mixed", "ext_abi/struct_mixed.c"),    // int+float fields split across reg classes
        s("struct-hfa",   "ext_abi/struct_hfa.c"), // jit86: UNIMPL `0F 5B` CVTDQ2PS (int->float vec)
        s("struct-ret",   "ext_abi/struct_ret.c"),      // register-pair vs sret-pointer returns
        s("union-pun",    "ext_abi/union_pun.c"), // jit86: UNIMPL `0F 5B` CVTDQ2PS (int->float vec)
    ])
}

/// Memory & non-local control: alloca, C99 VLAs (incl 2-D), setjmp/longjmp across frames, mem* builtins,
/// pointer arithmetic / multi-dim indexing, endian (de)serialization, and aggregate global initializers.
fn abi_mem() -> Group {
    group("abi-mem", vec![
        s("alloca",     "ext_abi/alloca.c"),            // dynamic stack allocation in a loop
        s("vla",        "ext_abi/vla.c"),               // 1-D & 2-D variable-length arrays
        s("setjmp",     "ext_abi/setjmp_longjmp.c"),    // setjmp/longjmp across several frames
        s("memops",     "ext_abi/memops.c"),            // memcpy/memmove/memset/memcmp sizes & overlap
        s("ptr-arith",  "ext_abi/ptr_arith.c"),         // pointer arithmetic + multi-dim indexing
        s("endian",     "ext_abi/endian.c"),            // little/big-endian byte (de)serialization
        s("globalinit", "ext_abi/globalinit.c"),        // static aggregate / designated initializers
    ])
}

/// Calling-convention edges: more args than registers (stack-passed), interleaved int/fp args, the full
/// scalar return-type matrix, single-thread atomics (fetch/CAS/exchange), and a mul/xor/shift hash kernel.
fn abi_args() -> Group {
    group("abi-args", vec![
        s("manyargs",  "ext_abi/manyargs.c"),           // >8 int / >8 fp args -> stack ABI
        s("mixedargs", "ext_abi/mixedargs.c"),          // interleaved int/fp register sequencing
        s("rettypes",  "ext_abi/rettypes.c"),           // char/short/int/long/uint/float/double/ptr returns
        s("atomics-st","ext_abi/atomics_st.c"),         // __atomic fetch_*/CAS/exchange (deterministic)
        s("hash",      "ext_abi/hash.c"),               // FNV-1a + splitmix64 (mul/xor/shift codegen)
    ])
}
