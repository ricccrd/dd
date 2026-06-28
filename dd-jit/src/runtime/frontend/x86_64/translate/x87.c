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
