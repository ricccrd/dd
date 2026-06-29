//! libc — basics breadth expansion (in-process JIT matrix). Owner: libc agent. Edit ONLY this file.
//! Builders: src(name,file).oracle()/.exit()/.out()/.has(); port(name,file) for cross-engine golden.
//! Keep this module compiling at all times (`cargo build -p dd-tests`).
//!
//! Strategy: verdict-style guests (every field must be `1`) are `port(...)` so they run on all three
//! engines (Linux x2 + macOS) with a deterministic golden line. Raw-format dumps whose exact rendering
//! is glibc-specific (printf %a/%e/%g, strerror text, the PRNG sequence, transcendental ULPs, broad
//! strftime locale names) are `src(...).oracle()` so they only run on the two Linux engines and are
//! diffed byte-for-byte against a native glibc run — that auto-catches any JIT divergence.
#![allow(unused_imports)]
use crate::{group, src, port, fixture, in_rootfs, Case, Engine, Group};

pub fn groups() -> Vec<Group> {
    vec![ext_string(), ext_stdio(), ext_stdlib(), ext_math(), ext_ctype_wchar(), ext_time(), ext_misc()]
}

/// string.h breadth — copy/concat/move, comparison (incl case-insensitive + C collation), search,
/// tokenisation (strtok/strtok_r/strsep), dup/stp* and overlapping memmove. Portable golden verdicts.
fn ext_string() -> Group {
    group("ext-string", vec![
        port("str-copy", "ext_libc/str_copy.c")
            .out("str_copy len=1 cpy=1 ncpy=1 cat=1 ncat=1 mcpy=1 mmove=1 mset=1\n"),
        port("str-cmp", "ext_libc/str_cmp.c")
            .out("str_cmp eq=1 lt=1 gt=1 n=1 m=1 ci=1 nci=1 coll=1\n"),
        port("str-search", "ext_libc/str_search.c")
            .out("str_search chr=1 rchr=1 str=1 nul=1 pbrk=1 spn=1 cspn=1 mchr=1 nlen=1\n"),
        port("str-tok", "ext_libc/str_tok.c").out("str_tok tok=1 tokr=1 sep=1\n"),
        port("str-dup", "ext_libc/str_dup.c").out("str_dup dup=1 ndup=1 stpcpy=1 stpncpy=1\n"),
        port("str-xfrm", "ext_libc/str_xfrm.c").out("str_xfrm len=1 ord=1\n"),
        port("mem-move", "ext_libc/mem_move.c").out("mem_move fwd=1 bwd=1 set=1 cmp=1\n"),
    ])
}

/// stdio breadth — printf format coverage (int/float/%a/str/length-mods/flags/positional/limits) via
/// the native oracle, plus snprintf return semantics, the *scanf family, getline/getdelim, fread/fwrite,
/// fseek/ftell/fgetpos, ungetc, fgets/fputs and setvbuf as portable golden verdicts.
fn ext_stdio() -> Group {
    group("ext-stdio", vec![
        // printf rendering is glibc-specific in places -> oracle vs native (both Linux engines).
        src("printf-int", "ext_libc/printf_int.c").oracle(),
        src("printf-float", "ext_libc/printf_float.c").oracle(),
        // %a/%A renders through glibc's hex-float path which uses MOVMSKPD (SSE sign extract); the
        // x86_64 JIT lacks opcode 0F 50 -> UNIMPL abort (exit 70). See GAPS "jit86-movmskps". aarch64 ok.
        src("printf-hexfloat", "ext_libc/printf_hexfloat.c").oracle(),
        src("printf-str", "ext_libc/printf_str.c").oracle(),
        src("printf-len", "ext_libc/printf_len.c").oracle(),           // hh h l ll z j t
        src("printf-flags", "ext_libc/printf_flags.c").oracle(),
        src("printf-pos", "ext_libc/printf_pos.c").oracle(),           // %n$ positional
        src("printf-limits", "ext_libc/printf_neg.c").oracle(),        // INT_MIN..ULLONG_MAX
        src("getline", "ext_libc/getline.c").oracle(),                 // getline + getdelim
        // portable golden verdicts — behaviour is standard across libc.
        port("snprintf-ret", "ext_libc/snprintf_ret.c").out("snprintf r1=1 r2=1 r3=1 r4=1\n"),
        port("sscanf-num", "ext_libc/sscanf_num.c").out("sscanf_num d1=1 d2=1 d3=1 d4=1 d5=1 d6=1\n"),
        port("sscanf-str", "ext_libc/sscanf_str.c").out("sscanf_str d1=1 d2=1 d3=1 d4=1 d5=1\n"),
        port("sscanf-mix", "ext_libc/sscanf_mix.c").out("sscanf_mix d1=1 d2=1 d3=1 d4=1 d5=1\n"),
        port("fread-fwrite", "ext_libc/fread_fwrite.c").out("fread_fwrite d1=1 d2=1 d3=1 d4=1\n"),
        port("fseek-ftell", "ext_libc/fseek_ftell.c").out("fseek_ftell d1=1 d2=1 d3=1 d4=1 d5=1\n"),
        port("ungetc", "ext_libc/ungetc.c").out("ungetc d1=1 d2=1 d3=1 d4=1 d5=1 d6=1\n"),
        port("fgets-fputs", "ext_libc/fgets_fputs.c").out("fgets_fputs d1=1 d2=1 d3=1 d4=1 d5=1\n"),
        port("fscanf", "ext_libc/fscanf.c").out("fscanf d1=1 d2=1 d3=1\n"),
        port("setvbuf", "ext_libc/setvbuf.c").out("setvbuf d1=1 d2=1 d3=1\n"),
    ])
}

/// stdlib breadth — strtol/strtoul/strtoll/strtoull (bases, sign, base-0, ERANGE), strtod/strtof/strtold,
/// atoi/atol/atoll/atof, qsort (int/struct/string), bsearch, abs/div families; rand() via oracle.
fn ext_stdlib() -> Group {
    group("ext-stdlib", vec![
        port("strtol", "ext_libc/strtol.c")
            .out("strtol d1=1 d2=1 d3=1 d4=1 d5=1 d6=1 d7=1 d8=1\n"),
        port("strtoul", "ext_libc/strtoul.c").out("strtoul d1=1 d2=1 d3=1 d4=1\n"),
        port("strtoll", "ext_libc/strtoll.c").out("strtoll d1=1 d2=1 d3=1 d4=1 d5=1\n"),
        // strtold/hex-float parsing pulls in glibc's MOVMSKPD sign extract -> x86_64 JIT UNIMPL 0F 50
        // abort (exit 70, empty stdout). Same gap as printf-hexfloat. See GAPS "jit86-movmskps". aarch64/mac ok.
        port("strto-float", "ext_libc/strto_float.c")
            .out("strto_float d1=1 d2=1 d3=1 d4=1 d5=1 d6=1 d7=1\n"),
        port("atox", "ext_libc/atox.c").out("atox d1=1 d2=1 d3=1 d4=1 d5=1\n"),
        port("qsort-int", "ext_libc/qsort_int.c").out("qsort_int sorted=1 d2=1\n"),
        port("qsort-struct", "ext_libc/qsort_struct.c").out("qsort_struct d1=1 d2=1\n"),
        port("qsort-str", "ext_libc/qsort_str.c").out("qsort_str d1=1 d2=1\n"),
        port("bsearch", "ext_libc/bsearch.c").out("bsearch d1=1 d2=1 d3=1 d4=1\n"),
        port("absdiv", "ext_libc/absdiv.c").out("absdiv d1=1 d2=1 d3=1 d4=1 d5=1 d6=1 d7=1\n"),
        // rand() PRNG sequence is glibc-specific -> oracle vs native (both Linux engines).
        src("rand-seq", "ext_libc/rand_seq.c").oracle(),
    ])
}

/// math.h breadth — transcendental functions diffed against native glibc (ULP-sensitive), plus exact
/// integral/structural results (round/mod/frexp/classify/misc) as portable golden verdicts.
fn ext_math() -> Group {
    group("ext-math", vec![
        src("math-explog", "ext_libc/math_explog.c").oracle(),
        src("math-trig", "ext_libc/math_trig.c").oracle(),
        src("math-hyper", "ext_libc/math_hyper.c").oracle(),
        src("math-gamma", "ext_libc/math_gamma.c").oracle(),
        port("math-round", "ext_libc/math_round.c")
            .out("math_round d1=1 d2=1 d3=1 d4=1 d5=1 d6=1 d7=1 d8=1\n"),
        port("math-mod", "ext_libc/math_mod.c").out("math_mod d1=1 d2=1 d3=1 d4=1 d5=1 d6=1 d7=1\n"),
        port("math-frexp", "ext_libc/math_frexp.c")
            .out("math_frexp d1=1 d2=1 d3=1 d4=1 d5=1 d6=1 d7=1\n"),
        port("math-misc", "ext_libc/math_misc.c").out("math_misc d1=1 d2=1 d3=1 d4=1 d5=1 d6=1 d7=1\n"),
        port("math-class", "ext_libc/math_class.c")
            .out("math_class d1=1 d2=1 d3=1 d4=1 d5=1 d6=1 d7=1\n"),
    ])
}

/// ctype + wchar/wctype breadth — narrow classification/conversion over ASCII, wide string ops and wide
/// classification, and mbstowcs/wcstombs roundtrip in the C locale. Portable golden verdicts.
fn ext_ctype_wchar() -> Group {
    group("ext-ctype-wchar", vec![
        port("ctype-class", "ext_libc/ctype_class.c")
            .out("ctype_class d1=1 d2=1 d3=1 d4=1 d5=1 d6=1 d7=1 d8=1 d9=1 d10=1 d11=1 d12=1\n"),
        port("ctype-conv", "ext_libc/ctype_conv.c").out("ctype_conv up=1 lo=1 d3=1 d4=1\n"),
        // d4 (the wcschr NULL/non-NULL comparison) materialises a garbage pointer-like value instead of
        // a 0/1 boolean on the x86_64 JIT (exit 0, no UNIMPL) — silent miscompile of glibc wcschr's SSE
        // wide-char scan + boolean fold. aarch64 + mac correct. See GAPS "jit86-wcschr-bool".
        port("wchar-str", "ext_libc/wchar_str.c")
            .out("wchar_str d1=1 d2=1 d3=1 d4=1 d5=1 d6=1 d7=1\n"),
        port("wctype-class", "ext_libc/wctype_class.c")
            .out("wctype_class d1=1 d2=1 d3=1 d4=1 d5=1 d6=1 d7=1\n"),
        port("mbs-wcs", "ext_libc/mbs_wcs.c").out("mbs_wcs d1=1 d2=1 d3=1 d4=1\n"),
    ])
}

/// time.h breadth — FIXED-epoch gmtime/mktime/timegm/difftime/asctime/ctime and strftime (numeric
/// specifiers portable; broad locale-name specifiers via oracle). No clocks — fully deterministic.
fn ext_time() -> Group {
    group("ext-time", vec![
        port("time-gmtime", "ext_libc/time_gmtime.c")
            .out("time_gmtime d1=1 d2=1 d3=1 d4=1 d5=1 d6=1\n"),
        port("time-mktime", "ext_libc/time_mktime.c").out("time_mktime d1=1 d2=1 d3=1\n"),
        port("time-diff", "ext_libc/time_diff.c").out("time_diff d1=1 d2=1 d3=1\n"),
        port("time-asctime", "ext_libc/time_asctime.c").out("time_asctime d1=1 d2=1\n"),
        port("time-strftime-num", "ext_libc/time_strftime_num.c")
            .out("time_strftime_num d1=1 d2=1 d3=1 d4=1 d5=1\n"),
        // %A/%a/%B/%b month+weekday names rendered by the locale -> oracle vs native (both Linux).
        src("time-strftime", "ext_libc/time_strftime.c").oracle(),
    ])
}

/// Misc C-runtime corners — C-locale localeconv/collation, errno (symbolic, since numeric codes differ
/// across libc) + strerror text via oracle, setjmp/longjmp value passing, and assert() off under NDEBUG.
fn ext_misc() -> Group {
    group("ext-misc", vec![
        port("locale-c", "ext_libc/locale_c.c").out("locale_c d1=1 d2=1 d3=1 d4=1 d5=1\n"),
        port("errno-sym", "ext_libc/errno_sym.c").out("errno_sym d1=1 d2=1 d3=1 d4=1 d5=1\n"),
        src("strerror", "ext_libc/strerror.c").oracle(), // message text is glibc-specific
        port("setjmp-val", "ext_libc/setjmp_val.c").out("setjmp_val d1=1 d2=1\n"),
        port("assert-off", "ext_libc/assert_off.c").out("assert_off reached=1\n"),
    ])
}
