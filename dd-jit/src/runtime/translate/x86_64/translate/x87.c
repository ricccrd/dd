// ===== x87 translate-time stack-top (fptop) tracking =====================================
// The baseline x87 path keeps ST(0..7) in cpu->st[] with the live top in cpu->fptop, and every
// ST(i) touch recomputes the wrapped slot at runtime (e_st_addr: ldr fptop; add #i; and #7; add
// base; add idx,lsl#3 -- 5 insns) while each push/pop does a ldr/modify/str of cpu->fptop.
//
// When the absolute top is statically known at translate time we instead:
//   * resolve ST(i) to the concrete slot (g_fp_top+i)&7 and address it with ONE `add xa,x28,#off`,
//   * keep push/pop as pure translate-time bookkeeping (no cpu->fptop traffic), writing the shadow
//     back to cpu->fptop only when guest state may escape (a faulting guest memory access, a C-helper
//     exit, or any non-x87 instruction / block boundary) -- exactly as lazy flags spill to cpu->nzcv.
// Storage stays cpu->st[] at double precision, the ops/condition codes/fpsw paths are untouched, so
// results are bit-identical to the baseline; only the addressing of ST(i) and the timing of the
// cpu->fptop store change, and the escape-point materialize keeps cpu->fptop observably current.
//
// The top is "known" only after a `finit` anchors it (top=0) within an unbroken run of x87
// instructions; any non-x87 instruction ends the run (materialize + drop to the runtime model), and
// any x87 op we cannot statically track falls back to the baseline helpers. NOX87OPT forces the
// runtime-top path everywhere -> byte-identical to the pre-opt engine.
static int g_x87opt = -1;  // -1 uninit; 0 = NOX87OPT set (off); 1 = on
static int g_fp_known = 0; // 1 if the absolute top is statically known right now
static int g_fp_top = 0;   // the known top (0..7), valid iff g_fp_known
static int g_fp_dirty = 0; // 1 if the shadow top has not been written to cpu->fptop
static int x87opt_on(void) {
    if (g_x87opt < 0) g_x87opt = (getenv("NOX87OPT") == NULL);
    return g_x87opt;
}
// &cpu->st[(g_fp_top+i)&7] -> xdst, single add (OFF_ST + slot*8 fits the add imm12).
static void fp_slot_addr(int xdst, int i) {
    unsigned off = (unsigned)OFF_ST + (unsigned)(((g_fp_top + i) & 7) * 8);
    emit32(0x91000000u | (off << 10) | (28 << 5) | xdst); // add xdst, x28, #off
}
// Make cpu->fptop reflect the shadow (idempotent). Keeps g_fp_known.
static void fp_materialize(void) {
    if (g_fp_known && g_fp_dirty) {
        e_movconst(16, (uint64_t)g_fp_top);
        e_str(16, 28, OFF_FPTOP);
        g_fp_dirty = 0;
    }
}
// Leave the static-top model (run boundary / untrackable op): spill the shadow, go runtime-top.
static void fp_drop(void) {
    fp_materialize();
    g_fp_known = 0;
}
#define FP_STATIC (x87opt_on() && g_fp_known)
static void fp_ld(int vd, int i) { // vd = ST(i)
    if (FP_STATIC) {
        fp_slot_addr(17, i);
        e_ldr_d(vd, 17);
    } else
        e_fp_ld(vd, i);
}
static void fp_st(int vs, int i) { // ST(i) = vs
    if (FP_STATIC) {
        fp_slot_addr(17, i);
        e_str_d(vs, 17);
    } else
        e_fp_st(vs, i);
}
static void fp_push(int vs) { // push vs -> ST(0)  (top -= 1)
    if (FP_STATIC) {
        g_fp_top = (g_fp_top - 1) & 7;
        g_fp_dirty = 1;
        fp_slot_addr(17, 0);
        e_str_d(vs, 17);
    } else
        e_fp_push(vs);
}
static void fp_settop(int delta) { // top += delta  (pop = +1)
    if (FP_STATIC) {
        g_fp_top = (g_fp_top + delta) & 7;
        g_fp_dirty = 1;
    } else
        e_fp_settop(delta);
}
#undef FP_STATIC

// ===== x87 D9 Fx remainder / scale / extract group (computed on host doubles) ===========
// These ops have no SSE counterpart; emulate them on f64 with the same write-back-to-cpu->st[]
// and FPSW condition-code conventions as the inline D8-DF arithmetic. Scratch: GP x16/x17/x19/x21,
// FP v16/v17/v18 (v0..v15 are guest xmm; v16+ are free). The fp_ld/fp_st/fp_push/fp_settop calls
// keep the translate-time static-top shadow consistent exactly like the surrounding ops.

// FPREM (round-to-zero) / FPREM1 (round-to-nearest-even): ST0 = ST0 - ST1*Q, Q = round(ST0/ST1).
// The reduction is completed in one fused step, so C2<-0 ("reduction complete"). FPREM also publishes
// the low three bits of |Q| (C0<-Q2, C3<-Q1, C1<-Q0); FPREM1 leaves the quotient bits cleared -- both
// matching qemu's helper_fprem and the `do { fprem } while (C2)` loop libc wraps around fmod/remainder.
static void emit_fprem(int ieee) {
    fp_ld(18, 0);                                                  // d18 = ST0
    fp_ld(16, 1);                                                  // d16 = ST1
    emit32(0x1E601800u | (16 << 16) | (18 << 5) | 17);            // fdiv  d17, d18, d16
    emit32((ieee ? 0x1E644000u : 0x1E65C000u) | (17 << 5) | 17);  // frintn/frintz d17, d17  (= Q)
    emit32(0x1F408000u | (16 << 16) | (18 << 10) | (17 << 5) | 18); // fmsub d18, d17, d16, d18 (ST0-Q*ST1)
    fp_st(18, 0);
    e_ldr(16, 28, OFF_FPSW);
    e_movconst(19, ~(uint64_t)0x4700);
    e_rrr(A_AND, 16, 16, 19, 1, 0); // clear C0/C1/C2/C3 (C2 stays 0 -> reduction complete)
    if (!ieee) {                    // FPREM: quotient bits from the magnitude of Q
        emit32(0x1E60C000u | (17 << 5) | 17); // fabs   d17, d17  (|Q|)
        emit32(0x9E780000u | (17 << 5) | 17); // fcvtzs x17, d17  (|Q| as integer)
        e_bfi(16, 17, 9, 1, 1);               // C1 (bit 9)  <- Q bit0
        e_lsr_i(19, 17, 1, 1);
        e_bfi(16, 19, 14, 1, 1); // C3 (bit 14) <- Q bit1
        e_lsr_i(19, 17, 2, 1);
        e_bfi(16, 19, 8, 1, 1); // C0 (bit 8)  <- Q bit2
    }
    e_str(16, 28, OFF_FPSW);
}

// FSCALE: ST0 = ST0 * 2^trunc(ST1). Build 2^n straight into the double exponent field; clamping the
// biased exponent to [0,2047] gives +0.0 on underflow and +Inf on overflow, matching scalbn.
static void emit_fscale(void) {
    fp_ld(18, 0);                         // d18 = ST0
    fp_ld(16, 1);                         // d16 = ST1
    emit32(0x1E780000u | (16 << 5) | 16); // fcvtzs w16, d16  (n = trunc(ST1), int32-saturating)
    e_sxt(16, 16, 4);                     // sign-extend n to 64-bit
    e_addi(16, 16, 1023, 1);              // biased exponent e = n + 1023
    e_movconst(19, 2047);
    e_subi_s(31, 16, 2047, 1);        // cmp e, #2047
    e_csel(16, 19, 16, 12 /*GT*/, 1); // e = (e > 2047) ? 2047 : e
    e_movconst(19, 0);
    e_subi_s(31, 16, 0, 1);           // cmp e, #0
    e_csel(16, 19, 16, 11 /*LT*/, 1); // e = (e < 0)    ? 0    : e
    e_lsl_i(16, 16, 52, 1);           // place e into the exponent field
    e_fmov_to_d(17, 16);              // d17 = 2^n
    emit32(0x1E600800u | (17 << 16) | (18 << 5) | 18); // fmul d18, d18, d17
    fp_st(18, 0);
}

// FXTRACT: split ST0 into unbiased exponent and significand. ST0 <- significand (in [1,2) with ST0's
// sign), then the exponent is pushed so ST1 = exponent, ST0 = significand (normal operands).
static void emit_fxtract(void) {
    fp_ld(16, 0);          // d16 = ST0
    e_fmov_from_d(16, 16); // x16 = bit pattern
    e_lsr_i(17, 16, 52, 1);
    e_movconst(19, 0x7FF);
    e_rrr(A_AND, 17, 17, 19, 1, 0);       // exponent field
    e_subi(17, 17, 1023, 1);              // unbiased exponent (signed)
    emit32(0x9E620000u | (17 << 5) | 17); // scvtf d17, x17  (exponent -> double)
    e_movconst(19, ~(0x7FFULL << 52));
    e_rrr(A_AND, 16, 16, 19, 1, 0); // clear exponent field
    e_movconst(19, 1023ULL << 52);
    e_rrr(A_ORR, 16, 16, 19, 1, 0); // set exponent to bias -> significand in [1,2)
    e_fmov_to_d(18, 16);            // d18 = significand
    fp_st(17, 0);                   // ST0 = exponent
    fp_push(18);                    // push significand -> ST0 = significand, ST1 = exponent
}

// FRNDINT: round ST0 to an integral value using the current rounding mode.
static void emit_frndint(void) {
    fp_ld(16, 0);
    emit32(0x1E674000u | (16 << 5) | 16); // frintx d16, d16
    fp_st(16, 0);
}

// FTST: compare ST0 with 0.0 and set the FPSW condition codes (same path as fcom).
static void emit_ftst(void) {
    fp_ld(18, 0);
    e_movconst(16, 0);
    e_fmov_to_d(16, 16);    // d16 = 0.0
    e_fcom_setfpsw(18, 16); // ST0 : 0.0 -> C0/C2/C3
}

// FNSTSW / FSTSW: the x87 status word reports TOP-of-stack (cpu->fptop) in bits 11-13 ORed with the
// condition codes held in cpu->fpsw -- qemu does the same, and code that follows FNSTSW with SAHF
// relies on it. Result -> x16 (clobbers x17). The shadow top is materialized first so cpu->fptop is
// current under the static-top optimization.
static void emit_fpsw_with_top(void) {
    fp_materialize();
    e_ldr(16, 28, OFF_FPSW);
    e_ldr(17, 28, OFF_FPTOP);
    e_bfi(16, 17, 11, 3, 1); // status[13:11] = TOP
}

// FXAM: classify ST0 and set the FPSW condition codes (C1 = sign, {C3,C2,C0} = class), per the x87
// spec. We keep no tag bits, so "empty" cannot be reported and every stored slot is read as its value;
// cpu->st[] is double precision, so 80-bit unsupported/pseudo-denormal forms cannot arise. Class codes
// {C3,C2,C0}: zero=100, NaN=001, Inf=011, denormal=110, normal=010. From the IEEE-754 fields this is
// C0=(exp==max), C3=(exp==0), C2=!(zero|NaN). Scratch: x16/x17/x19/x21/x22, v18.
static void emit_fxam(void) {
    fp_ld(18, 0);
    e_fmov_from_d(16, 18);          // x16 = bit pattern of ST0
    e_lsr_i(21, 16, 63, 1);         // x21 = sign            -> C1
    e_lsr_i(17, 16, 52, 1);
    e_movconst(19, 0x7FF);
    e_rrr(A_AND, 17, 17, 19, 1, 0); // x17 = exponent field
    e_movconst(19, (1ull << 52) - 1);
    e_rrr(A_AND, 16, 16, 19, 1, 0); // x16 = mantissa field
    e_subi_s(31, 17, 0, 1);
    e_cset(19, 0, 1);               // x19 = (exp == 0)      -> C3
    e_subi_s(31, 17, 0x7FF, 1);
    e_cset(17, 0, 1);               // x17 = (exp == max)    -> C0
    e_subi_s(31, 16, 0, 1);
    e_cset(16, 0, 1);               // x16 = (mantissa == 0)
    e_rrr(A_AND, 22, 19, 16, 1, 0); // x22 = zero = exp0 & mant0
    e_rrr(A_BIC, 16, 17, 16, 1, 0); // x16 = NaN  = expMax & ~mant0
    e_rrr(A_ORR, 22, 22, 16, 1, 0); // x22 = zero | NaN
    e_movconst(16, 1);
    e_rrr(A_EOR, 22, 22, 16, 1, 0); // x22 = C2 = !(zero | NaN)
    e_movconst(16, 0);
    e_bfi(16, 17, 8, 1, 1);  // C0 (bit 8)
    e_bfi(16, 21, 9, 1, 1);  // C1 (bit 9)
    e_bfi(16, 22, 10, 1, 1); // C2 (bit 10)
    e_bfi(16, 19, 14, 1, 1); // C3 (bit 14)
    e_str(16, 28, OFF_FPSW);
}

// x87 transcendentals (the D9 F0-FF subset: F2XM1/FYL2X/FPTAN/FPATAN/FYL2XP1/FSINCOS/FSIN/FCOS) have
// no ARM/SSE counterpart and need host libm, so they exit the block to the C helper x87_func(), which
// computes the op on the double-precision ST stack. cpu->x87_ea carries the X87_* selector. The block
// ends here (like the m80 fld/fstp helpers); the caller breaks out of translation afterwards.
static void emit_x87func(int fn, uint64_t next) {
    fp_drop(); // the helper reads/writes cpu->st[] and cpu->fptop directly -> spill the shadow top
    e_movconst(16, (uint64_t)fn);
    e_str(16, 28, OFF_X87EA);
    emit_exit_const(next, R_X87FUNC);
}
