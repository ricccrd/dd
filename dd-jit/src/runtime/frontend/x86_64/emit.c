// dd/runtime/frontend/x86_64 -- arm64 host emitters + NEON/SSE encoders (xmm->v0..15) + x87 FPU stack
// (ST(i) at double precision) + prologue/spill/exits.

// ---------------- ARM64 instruction emitters ----------------
// (the same-ISA-independent half: these emit HOST code, copied from jit.c +
//  a few width-typed loads/stores the x86 front-end needs.)
static void emit32(uint32_t in) {
    *(uint32_t *)g_cp = in;
    g_cp += 4;
}
static void e_str(int rt, int rn, int off) {
    emit32(0xF9000000u | (((unsigned)off / 8) << 10) | (rn << 5) | rt);
} // str x
static void e_ldr(int rt, int rn, int off) {
    emit32(0xF9400000u | (((unsigned)off / 8) << 10) | (rn << 5) | rt);
} // ldr x
static void e_movz(int rd, uint32_t imm16, int sh) { emit32(0xD2800000u | (sh << 21) | (imm16 << 5) | rd); }
static void e_movk(int rd, uint32_t imm16, int sh) { emit32(0xF2800000u | (sh << 21) | (imm16 << 5) | rd); }
static void e_br(int rn) { emit32(0xD61F0000u | (rn << 5)); }
static void e_movconst(int rd, uint64_t v) {
    e_movz(rd, v & 0xffff, 0);
    if ((v >> 16) & 0xffff) e_movk(rd, (v >> 16) & 0xffff, 1);
    if ((v >> 32) & 0xffff) e_movk(rd, (v >> 32) & 0xffff, 2);
    if ((v >> 48) & 0xffff) e_movk(rd, (v >> 48) & 0xffff, 3);
}
// width-typed load/store at [rn, #0]. w = 1/2/4/8 bytes. (zero-extends on load)
static void e_load(int w, int rt, int rn) {
    uint32_t b = w == 1 ? 0x39400000u : w == 2 ? 0x79400000u : w == 4 ? 0xB9400000u : 0xF9400000u;
    emit32(b | (rn << 5) | rt);
}
static void e_store(int w, int rt, int rn) {
    uint32_t b = w == 1 ? 0x39000000u : w == 2 ? 0x79000000u : w == 4 ? 0xB9000000u : 0xF9000000u;
    emit32(b | (rn << 5) | rt);
}
static void e_ldrs(int w, int rt, int rn) {                                 // sign-extending load into X
    uint32_t b = w == 1 ? 0x39800000u : w == 2 ? 0x79800000u : 0xB9800000u; // ldrsb/ldrsh/ldrsw
    emit32(b | (rn << 5) | rt);
}
// Address-mode-folded load/store: fold a [base+disp] memory operand into ONE ldr/str.
// Scaled unsigned-offset form (disp a multiple of w, disp/w in [0,4095]):
static void e_load_uoff(int w, int rt, int rn, unsigned disp) { // ldr{b,h,,} rt,[rn,#disp]
    uint32_t b = w == 1 ? 0x39400000u : w == 2 ? 0x79400000u : w == 4 ? 0xB9400000u : 0xF9400000u;
    emit32(b | (((disp / (unsigned)w) & 0xFFF) << 10) | (rn << 5) | rt);
}
static void e_store_uoff(int w, int rt, int rn, unsigned disp) { // str{b,h,,} rt,[rn,#disp]
    uint32_t b = w == 1 ? 0x39000000u : w == 2 ? 0x79000000u : w == 4 ? 0xB9000000u : 0xF9000000u;
    emit32(b | (((disp / (unsigned)w) & 0xFFF) << 10) | (rn << 5) | rt);
}
// Unscaled signed-offset form (simm9 in [-256,255]) -- covers small negative disps:
static void e_ldur(int w, int rt, int rn, int simm9) { // ldur{b,h,,} rt,[rn,#simm9]
    uint32_t b = w == 1 ? 0x38400000u : w == 2 ? 0x78400000u : w == 4 ? 0xB8400000u : 0xF8400000u;
    emit32(b | (((uint32_t)simm9 & 0x1FF) << 12) | (rn << 5) | rt);
}
static void e_stur(int w, int rt, int rn, int simm9) { // stur{b,h,,} rt,[rn,#simm9]
    uint32_t b = w == 1 ? 0x38000000u : w == 2 ? 0x78000000u : w == 4 ? 0xB8000000u : 0xF8000000u;
    emit32(b | (((uint32_t)simm9 & 0x1FF) << 12) | (rn << 5) | rt);
}
static void e_mov_rr(int rd, int rm, int sf) { // mov rd, rm  (orr rd, xzr, rm)
    emit32((sf ? 0xAA0003E0u : 0x2A0003E0u) | (rm << 16) | rd);
}
static void e_addi(int rd, int rn, unsigned imm12, int sf) { // add rd, rn, #imm
    emit32((sf ? 0x91000000u : 0x11000000u) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
static void e_subi(int rd, int rn, unsigned imm12, int sf) { // sub rd, rn, #imm
    emit32((sf ? 0xD1000000u : 0x51000000u) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
// add/sub immediate with optional LSL #12 (sh=1 shifts imm12 left by 12) -- lets a 24-bit
// displacement fold into one or two ALU ops instead of materializing a 64-bit constant.
static void e_addi_sh(int rd, int rn, unsigned imm12, int sf, int sh) {
    emit32((sf ? 0x91000000u : 0x11000000u) | ((unsigned)(sh & 1) << 22) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
static void e_subi_sh(int rd, int rn, unsigned imm12, int sf, int sh) {
    emit32((sf ? 0xD1000000u : 0x51000000u) | ((unsigned)(sh & 1) << 22) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
static void e_addi_s(int rd, int rn, unsigned imm12, int sf) { // adds rd, rn, #imm (sets flags)
    emit32((sf ? 0xB1000000u : 0x31000000u) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
static void e_subi_s(int rd, int rn, unsigned imm12, int sf) { // subs rd, rn, #imm (sets flags)
    emit32((sf ? 0xF1000000u : 0x71000000u) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
// shifted-register 3-operand (LSL #amt) for add/sub/and/orr/eor and their S-forms.
static void e_rrr(uint32_t base, int rd, int rn, int rm, int sf, int lsl) {
    emit32(base | (sf ? 0x80000000u : 0) | (rm << 16) | ((lsl & 0x3F) << 10) | (rn << 5) | rd);
}
#define A_ADD 0x0B000000u
#define A_ADDS 0x2B000000u
#define A_SUB 0x4B000000u
#define A_SUBS 0x6B000000u
#define A_AND 0x0A000000u
#define A_ANDS 0x6A000000u
#define A_ORR 0x2A000000u
#define A_EOR 0x4A000000u
#define A_ORN 0x2A200000u // orn (for mvn)
#define A_BIC 0x0A200000u // bic (and-not)
// nzcv scratch is x20 (a free callee-saved host reg, saved/restored by the trampoline),
// NOT x16/x17 -- so x16(value)/x17(EA) stay usable across flag-setting mem-dest ops.
static void e_nzcv_save(void) {
    emit32(0xD53B4200u | 20);
    e_str(20, 28, OFF_NZCV);
} // mrs x20,nzcv; str
static void e_nzcv_load(void) {
    e_ldr(20, 28, OFF_NZCV);
    emit32(0xD51B4200u | 20);
} // ldr x20; msr nzcv,x20
// Carry convention: cpu->nzcv stores the ARM *borrow* C (= NOT x86 CF), which ARM SUBS/
// SBCS produce naturally and the jcc table assumes. ARM ADDS/ADCS produce C = x86 CF
// (the opposite), so flags coming from an x86 add/adc must have C flipped to match.
static void e_nzcv_save_ci(void) {  // save flags, inverting C (scratch x22: x21 may hold a result)
    emit32(0xD53B4200u | 20);       // mrs x20, nzcv
    e_movconst(22, 1u << 29);       // C is bit 29 of nzcv
    e_rrr(A_EOR, 20, 20, 22, 1, 0); // eor x20, x20, #(1<<29)
    e_str(20, 28, OFF_NZCV);
    emit32(0xD51B4200u | 20); // also sync live ARM nzcv (msr) so spill persists the corrected value
}
static void e_nzcv_load_ci(void) { // load flags into live nzcv, inverting C
    e_ldr(20, 28, OFF_NZCV);
    e_movconst(22, 1u << 29);
    e_rrr(A_EOR, 20, 20, 22, 1, 0);
    emit32(0xD51B4200u | 20); // msr nzcv, x20
}
static void e_nzcv_save_c1(void) { // logical ops: x86 CF=0,OF=0; ARM ANDS/TST leave C,V stale
    emit32(0xD53B4200u | 20);      // mrs x20, nzcv
    e_movconst(22, 1u << 28);
    e_rrr(A_BIC, 20, 20, 22, 1, 0); // clear V (bit 28) -> OF=0  (jg/jle test SF==OF)
    e_movconst(22, 1u << 29);
    e_rrr(A_ORR, 20, 20, 22, 1, 0); // set C (bit 29) -> stored borrow for CF=0
    e_str(20, 28, OFF_NZCV);
    emit32(0xD51B4200u | 20); // sync live ARM nzcv
}
static void e_nzcv_save_setcf(int cfreg) { // save N/Z (from ARM nzcv), set stored C = NOT x86CF (cfreg holds 0/1)
    emit32(0xD53B4200u | 20);              // mrs x20, nzcv  (N,Z valid)
    e_movconst(22, 1u << 29);
    e_rrr(A_BIC, 20, 20, 22, 1, 0); // clear C
    e_movconst(23, 1);
    e_rrr(A_EOR, 23, cfreg, 23, 0, 0); // x23 = NOT cf (0/1)
    e_rrr(A_ORR, 20, 20, 23, 1, 29);   // stored C = (NOT cf) << 29
    e_str(20, 28, OFF_NZCV);
    emit32(0xD51B4200u | 20); // sync live ARM nzcv
}
static void e_nzcv_save_keepC(void) { // inc/dec: take new N/Z/V, KEEP stored C (x86 inc/dec don't touch CF)
    emit32(0xD53B4200u | 20);         // mrs x20, nzcv (new N,Z,V; C junk) -- scratch x24/x25 (x21 may hold a result)
    e_ldr(24, 28, OFF_NZCV);          // x24 = old stored flags (has the C to keep)
    e_movconst(25, 1u << 29);
    e_rrr(A_BIC, 20, 20, 25, 1, 0); // clear C in new
    e_rrr(A_AND, 24, 24, 25, 1, 0); // isolate old C
    e_rrr(A_ORR, 20, 20, 24, 1, 0); // new N/Z/V | old C
    e_str(20, 28, OFF_NZCV);
    emit32(0xD51B4200u | 20); // sync live ARM nzcv
}
static void e_bcond(int cond, int32_t off19) {
    emit32(0x54000000u | (((uint32_t)off19 & 0x7FFFF) << 5) | (cond & 0xF));
}
static void e_cset(int rd, int cond, int sf) { // cset rd, cond
    emit32((sf ? 0x9A9F07E0u : 0x1A9F07E0u) | (((cond ^ 1) & 0xF) << 12) | rd);
}
static void e_csel(int rd, int rn_t, int rm_f, int cond, int sf) {
    emit32((sf ? 0x9A800000u : 0x1A800000u) | (rm_f << 16) | ((cond & 0xF) << 12) | (rn_t << 5) | rd);
}
static void e_uxt(int rd, int rn, int w) { // uxtb/uxth (zero-extend reg)
    emit32((w == 1 ? 0x12001C00u : 0x12003C00u) | (rn << 5) | rd);
}
static void e_sxt(int rd, int rn, int w) { // sxtb/sxth/sxtw into X
    uint32_t b = w == 1 ? 0x93401C00u : w == 2 ? 0x93403C00u : 0x93407C00u;
    emit32(b | (rn << 5) | rd);
}
// shift by immediate (UBFM/SBFM). sh in [0,31] (32-bit) or [0,63] (64-bit).
static void e_lsl_i(int rd, int rn, int sh, int sf) {
    int w = sf ? 64 : 32, immr = (w - sh) & (w - 1), imms = w - 1 - sh;
    emit32((sf ? 0xD3400000u : 0x53000000u) | (immr << 16) | (imms << 10) | (rn << 5) | rd);
}
static void e_lsr_i(int rd, int rn, int sh, int sf) {
    int imms = sf ? 63 : 31;
    emit32((sf ? 0xD3400000u : 0x53000000u) | (sh << 16) | (imms << 10) | (rn << 5) | rd);
}
static void e_asr_i(int rd, int rn, int sh, int sf) { // asr = SBFM rd,rn,#sh,#(w-1)
    int imms = sf ? 63 : 31;
    emit32((sf ? 0x93400000u : 0x13000000u) | (sh << 16) | (imms << 10) | (rn << 5) | rd);
}
static void e_bfi(int rd, int rn, int lsb, int width, int sf) { // bfi rd,rn,#lsb,#width
    int w = sf ? 64 : 32, immr = (w - lsb) & (w - 1), imms = width - 1;
    emit32((sf ? 0xB3400000u : 0x33000000u) | (immr << 16) | (imms << 10) | (rn << 5) | rd);
}
// variable shift (LSLV/LSRV/ASRV/RORV), count in rm (low bits used)
static void e_shv(uint32_t base32, int rd, int rn, int rm, int sf) {
    emit32(base32 | (sf ? 0x80000000u : 0) | (rm << 16) | (rn << 5) | rd);
}
#define S_LSLV 0x1AC02000u
#define S_LSRV 0x1AC02400u
#define S_ASRV 0x1AC02800u
#define S_RORV 0x1AC02C00u
static void e_mul(int rd, int rn, int rm, int sf) {
    emit32((sf ? 0x9B007C00u : 0x1B007C00u) | (rm << 16) | (rn << 5) | rd);
}
static void e_umulh(int rd, int rn, int rm) { emit32(0x9BC07C00u | (rm << 16) | (rn << 5) | rd); }
static void e_smulh(int rd, int rn, int rm) { emit32(0x9B407C00u | (rm << 16) | (rn << 5) | rd); }
static void e_udiv(int rd, int rn, int rm, int sf) {
    emit32((sf ? 0x9AC00800u : 0x1AC00800u) | (rm << 16) | (rn << 5) | rd);
}
static void e_sdiv(int rd, int rn, int rm, int sf) {
    emit32((sf ? 0x9AC00C00u : 0x1AC00C00u) | (rm << 16) | (rn << 5) | rd);
}
static void e_msub(int rd, int rn, int rm, int ra, int sf) {
    emit32((sf ? 0x9B008000u : 0x1B008000u) | (rm << 16) | (ra << 10) | (rn << 5) | rd);
}
static void e_ror_i(int rd, int rn, int sh, int sf) { // ror rd,rn,#sh  (EXTR rd,rn,rn,#sh)
    emit32((sf ? 0x93C00000u : 0x13800000u) | (rn << 16) | ((sh & (sf ? 63 : 31)) << 10) | (rn << 5) | rd);
}
static void e_extr(int rd, int rn, int rm, int lsb, int sf) { // EXTR rd, rn, rm, #lsb  = (rn:rm) >> lsb
    emit32((sf ? 0x93C00000u : 0x13800000u) | (rm << 16) | ((lsb & (sf ? 63 : 31)) << 10) | (rn << 5) | rd);
}
static void e_tst(int rn, int sf) {
    emit32((sf ? 0xEA00001Fu : 0x6A00001Fu) | (rn << 16) | (rn << 5));
} // ands xzr,rn,rn
static void e_rbit(int rd, int rn, int sf) { emit32((sf ? 0xDAC00000u : 0x5AC00000u) | (rn << 5) | rd); }
static void e_clz(int rd, int rn, int sf) { emit32((sf ? 0xDAC01000u : 0x5AC01000u) | (rn << 5) | rd); }

// ---- NEON / SSE encoders (guest xmm0..15 live in host v0..v15) ----
static void e_str_q(int t, int rn, int off) { emit32(0x3D800000u | (((unsigned)off / 16) << 10) | (rn << 5) | t); }
static void e_ldr_q(int t, int rn, int off) { emit32(0x3DC00000u | (((unsigned)off / 16) << 10) | (rn << 5) | t); }
static void e_stp_q(int t1, int t2, int rn, int off) {
    emit32(0xAD000000u | (((unsigned)(off / 16) & 0x7F) << 15) | (t2 << 10) | (rn << 5) | t1);
}
static void e_ldp_q(int t1, int t2, int rn, int off) {
    emit32(0xAD400000u | (((unsigned)(off / 16) & 0x7F) << 15) | (t2 << 10) | (rn << 5) | t1);
}
static void e_ldr_d(int t, int rn) { emit32(0xFD400000u | (rn << 5) | t); }         // ldr d,[xn]
static void e_str_d(int t, int rn) { emit32(0xFD000000u | (rn << 5) | t); }         // str d,[xn]
static void e_ldr_s(int t, int rn) { emit32(0xBD400000u | (rn << 5) | t); }         // ldr s,[xn]
static void e_str_s(int t, int rn) { emit32(0xBD000000u | (rn << 5) | t); }         // str s,[xn]
static void e_fmov_to_d(int vd, int xn) { emit32(0x9E670000u | (xn << 5) | vd); }   // fmov d[vd], x[xn] (zeroes hi)
static void e_fmov_to_s(int vd, int wn) { emit32(0x1E270000u | (wn << 5) | vd); }   // fmov s[vd], w[wn]
static void e_fmov_from_d(int xd, int vn) { emit32(0x9E660000u | (vn << 5) | xd); } // fmov x[xd], d[vn]
static void e_fmov_from_s(int wd, int vn) { emit32(0x1E260000u | (vn << 5) | wd); } // fmov w[wd], s[vn]
static void e_vmov(int vd, int vn) { emit32(0x4EA01C00u | (vn << 16) | (vn << 5) | vd); } // mov vd.16b, vn.16b (orr)
static void e_vmov8(int vd, int vn) {
    emit32(0x0EA01C00u | (vn << 16) | (vn << 5) | vd);
}                                                     // mov vd.8b, vn.8b (low 64, zero upper)
static void e_ins_d(int vd, int ld, int vn, int ls) { // ins vd.d[ld], vn.d[ls]
    emit32(0x6E000400u | ((unsigned)((ld << 4) | 8) << 16) | ((unsigned)(ls << 3) << 11) | (vn << 5) | vd);
}
static void e_ins_s(int vd, int ls_lane, int vn, int sl) { // ins vd.s[ls_lane], vn.s[sl]
    emit32(0x6E000400u | ((unsigned)((ls_lane << 3) | 4) << 16) | ((unsigned)(sl << 2) << 11) | (vn << 5) | vd);
}
// ---- x87 FPU stack helpers (ST(i) emulated at double precision in cpu->st[]) ----
// ST(0) = cpu->st[fptop & 7]; the stack grows downward (push: --top). Scratch: x16/x17, v16+.
static void e_st_addr(int xa, int i) { // xa = &cpu->st[(fptop+i)&7]   (clobbers x16)
    e_ldr(16, 28, OFF_FPTOP);
    if (i) emit32(0x11000000u | ((unsigned)(i & 7) << 10) | (16 << 5) | 16); // add w16,w16,#i
    emit32(0x12000800u | (16 << 5) | 16);                                    // and w16,w16,#7
    emit32(0x91000000u | ((unsigned)OFF_ST << 10) | (28 << 5) | xa);         // add xa,x28,#OFF_ST
    emit32(0x8B000000u | (16 << 16) | (3 << 10) | (xa << 5) | xa);           // add xa,xa,x16,lsl#3
}
static void e_fp_settop(int delta) { // fptop = (fptop+delta) & 7   (clobbers x16)
    e_ldr(16, 28, OFF_FPTOP);
    if (delta < 0)
        emit32(0x51000000u | ((unsigned)(-delta) << 10) | (16 << 5) | 16); // sub w16,w16,#n
    else if (delta)
        emit32(0x11000000u | ((unsigned)delta << 10) | (16 << 5) | 16); // add w16,w16,#n
    emit32(0x12000800u | (16 << 5) | 16);                               // and w16,w16,#7
    e_str(16, 28, OFF_FPTOP);
}
static void e_fp_ld(int vd, int i) {
    e_st_addr(17, i);
    e_ldr_d(vd, 17);
} // vd = ST(i)
static void e_fp_st(int vs, int i) {
    e_st_addr(17, i);
    e_str_d(vs, 17);
} // ST(i) = vs
static void e_fp_push(int vs) {
    e_fp_settop(-1);
    e_st_addr(17, 0);
    e_str_d(vs, 17);
} // push vs -> ST(0)
// fcom-family compare: FCMP dn,dm then set cpu->fpsw bits C0(8)/C2(10)/C3(14) so a
// following fnstsw ax + sahf reproduces x86 ZF/PF/CF. (clobbers x16/x17/x20)
static void e_fcom_setfpsw(int n, int m) {
    emit32(0x1E602000u | (m << 16) | (n << 5)); // fcmp dn, dm
    e_cset(16, 3, 0);                           // less       (LO: C clear)
    e_cset(20, 6, 0);                           // unordered  (VS)
    e_rrr(A_ORR, 16, 16, 20, 0, 0);             // C0 = less | unordered
    e_cset(17, 0, 0);                           // equal      (EQ)
    e_rrr(A_ORR, 17, 17, 20, 0, 0);             // C3 = equal | unordered
    e_lsl_i(16, 16, 8, 0);
    e_lsl_i(20, 20, 10, 0);
    e_rrr(A_ORR, 16, 16, 20, 0, 0); // | C2<<10
    e_lsl_i(17, 17, 14, 0);
    e_rrr(A_ORR, 16, 16, 17, 0, 0); // | C3<<14
    e_str(16, 28, OFF_FPSW);
}
// SSE shift-by-immediate -> NEON USHR/SSHR/SHL (esize in bits: 16/32/64)
static void e_vshr_imm(int vd, int vn, int esize, int sh, int sgn) {
    if (sh <= 0) {
        e_vmov(vd, vn);
        return;
    }
    if (sh > esize) sh = esize;
    unsigned immhb = 2 * esize - sh;
    emit32((sgn ? 0x4F000400u : 0x6F000400u) | (immhb << 16) | (vn << 5) | vd);
}
static void e_vshl_imm(int vd, int vn, int esize, int sh) {
    if (sh <= 0) {
        e_vmov(vd, vn);
        return;
    }
    if (sh >= esize) {
        emit32(0x6E201C00u | (vd << 16) | (vd << 5) | vd);
        return;
    } // >=width -> 0 (eor vd,vd,vd)
    unsigned immhb = esize + sh;
    emit32(0x4F005400u | (immhb << 16) | (vn << 5) | vd);
}
static void e_ext(int vd, int vn, int vm, int idx) {
    emit32(0x6E000000u | (vm << 16) | ((idx & 0xF) << 11) | (vn << 5) | vd);
}
static void e_v3(uint32_t base, int vd, int vn, int vm) {
    emit32(base | (vm << 16) | (vn << 5) | vd);
} // NEON 3-same .16b/.Ns
// LSE atomics (AL ordering). sz: 1/2/4/8 bytes.
static void e_lse(uint32_t base, int sz, int rs, int rt, int rn) {
    uint32_t szb = sz == 8 ? 0xC0000000u : sz == 4 ? 0x80000000u : sz == 2 ? 0x40000000u : 0;
    emit32((base & 0x3FFFFFFFu) | szb | (rs << 16) | (rn << 5) | rt);
}
static void e_cas(int sz, int rs, int rt, int rn) { // casal Rs(old/cmp), Rt(new), [Rn]
    uint32_t b = sz == 8 ? 0xC8E0FC00u : sz == 4 ? 0x88E0FC00u : sz == 2 ? 0x48E0FC00u : 0x08E0FC00u;
    emit32(b | (rs << 16) | (rn << 5) | rt);
}
#define LSE_LDADD 0xB8E00000u // ldaddal  ([m] += rs)
#define LSE_LDCLR 0xB8E01000u // ldclral  ([m] &= ~rs)
#define LSE_LDEOR 0xB8E02000u // ldeoral  ([m] ^= rs)
#define LSE_LDSET 0xB8E03000u // ldsetal  ([m] |= rs)
#define LSE_SWP 0xB8E08000u   // swpal    (x = [m]; [m] = rs)

static int64_t sext(uint64_t v, int bits) {
    uint64_t m = 1ull << (bits - 1);
    return (int64_t)((v ^ m) - m);
}
static void block_return(void);

// ---------------- prologue / spill / exits ----------------
// Prologue: entered x0 = &cpu. Pin cpu in x28, restore flags + 16 guest GPRs (x0 last).
static void emit_prologue(void) {
    emit32(0xAA0003FCu); // mov x28, x0   (cpu)
    e_nzcv_load();       // restore flags
    for (int t = 0; t < 16; t += 2)
        e_ldp_q(t, t + 1, 28, OFF_V + t * 16); // guest xmm0..15 -> v0..v15
    for (int r = 1; r <= 15; r++)
        e_ldr(r, 28, R_OFF(r));
    e_ldr(0, 28, 0); // rax last
}
// Spill: x28 is live (== cpu), so store the 16 GPRs + flags + xmm. The flag save reads the
// LIVE ARM nzcv -- which is kept == cpu->nzcv by every flag producer (the borrow-convention
// helpers below `msr` their corrected value back into ARM nzcv so this stays consistent).
static void emit_spill(void) {
    for (int r = 0; r <= 15; r++)
        e_str(r, 28, R_OFF(r));
    for (int t = 0; t < 16; t += 2)
        e_stp_q(t, t + 1, 28, OFF_V + t * 16); // v0..v15 -> guest xmm
    e_nzcv_save();
}
static void emit_exit_const(uint64_t rip, uint64_t reason) {
    emit_spill();
    e_movconst(16, rip);
    e_str(16, 28, OFF_RIP);
    e_movconst(16, reason);
    e_str(16, 28, OFF_RSN);
    e_movconst(16, (uint64_t)block_return);
    e_br(16); // block_return uses x28 (still cpu)
}
// ---------------- S1: inline vDSO-style time fast path (cntvct-based) ----------------
// Apple Silicon exposes the EL0-readable generic-timer count (CNTVCT_EL0) + its frequency
// (CNTFRQ_EL0). We calibrate once at startup against the host's REALTIME/MONOTONIC clocks,
// then emit inline ARM64 at the guest `syscall` site that reads CNTVCT and converts to a Linux
// timespec/timeval WITHOUT entering service() -- the guest's clock_gettime/gettimeofday never
// trap. ns = base_ns + ((ticks-base_ticks)*mult)>>FAST_SHIFT (Q30, overflow-safe 128-bit).
static int g_fastsys = 1;         // master switch (JIT86_NOFASTSYS=1 -> 0 = byte-identical old path)
static uint64_t g_fast_count;     // # of guest time syscalls satisfied inline (written by emitted code)
static uint64_t g_cal_base_ticks; // CNTVCT at calibration
static uint64_t g_cal_mono_ns;    // CLOCK_MONOTONIC ns at calibration
static uint64_t g_cal_real_ns;    // CLOCK_REALTIME  ns at calibration
static uint64_t g_cal_mult;       // tick->ns multiplier (Q30): round(1e9 * 2^FAST_SHIFT / cntfrq)
#define FAST_SHIFT 30
static void s1_calibrate(void) {
    const char *off = getenv("JIT86_NOFASTSYS");
    if (off && off[0] && off[0] != '0') {
        g_fastsys = 0;
        return;
    }
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (!freq) {
        g_fastsys = 0; // no readable counter frequency -> safe fallback
        return;
    }
    g_cal_mult = (uint64_t)(((unsigned __int128)1000000000ull << FAST_SHIFT) / freq);
    struct timespec tm, tr;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(g_cal_base_ticks));
    clock_gettime(CLOCK_MONOTONIC, &tm);
    clock_gettime(CLOCK_REALTIME, &tr);
    g_cal_mono_ns = (uint64_t)tm.tv_sec * 1000000000ull + (uint64_t)tm.tv_nsec;
    g_cal_real_ns = (uint64_t)tr.tv_sec * 1000000000ull + (uint64_t)tr.tv_nsec;
}
// Emitted at the guest `syscall` site. Guest GPRs are live in x0..x15 (rax=x0, rsi=x6, rdi=x7),
// guest flags live in ARM nzcv, cpu pinned in x28. x16/x17 and the host callee-saved x19..x27
// (saved by run_block, restored by block_return) are free scratch in-block, NOT guest state.
// For clock_gettime(228)/gettimeofday(96) with clockid in {REALTIME=0,MONOTONIC=1} we read
// CNTVCT_EL0, convert with the startup calibration, write the guest result buffer, set rax=0, and
// branch to L_after. All other syscall numbers (and unhandled clockids) restore flags and take the
// unchanged full R_SYSCALL exit. Guest nzcv is saved (x17) and restored on EVERY path so the slow
// exit is semantically identical to the baseline. The CALLER ends the block right after this (a
// chained branch to `next`); we never continue decoding past the syscall (would run off the end).
static void emit_fast_syscall(uint64_t next) {
    emit32(0xD53B4211u); // mrs x17, nzcv   (preserve guest flags across our compares)
    uint32_t *after[2];
    int na = 0; // slots: b L_after
    uint32_t *to_slow[2];
    int nsl = 0; // slots: b.ne slow_restore

    // ---- clock_gettime: rax == 228 ----
    e_subi_s(16, 0, 228, 1); // subs x16, x0, #228
    uint32_t *m1 = (uint32_t *)g_cp;
    e_bcond(1, 0);           // b.ne -> gettimeofday
    e_subi_s(16, 7, 1, 1);   // cmp clockid(rdi=x7), #1 (MONOTONIC)
    uint32_t *cs = (uint32_t *)g_cp;
    e_bcond(1, 0);                  // b.ne -> check REALTIME
    e_movconst(19, g_cal_mono_ns);  // base_ns = mono
    uint32_t *jc = (uint32_t *)g_cp;
    emit32(0x14000000u);            // b conv
    // check REALTIME:
    *cs = (*cs & 0xFF00001Fu) | ((uint32_t)(((uint32_t *)g_cp - cs) & 0x7FFFF) << 5);
    e_subi_s(16, 7, 0, 1); // cmp clockid, #0 (REALTIME)
    to_slow[nsl++] = (uint32_t *)g_cp;
    e_bcond(1, 0);                 // b.ne -> slow (unhandled clockid)
    e_movconst(19, g_cal_real_ns); // base_ns = real
    // conv:
    *jc = 0x14000000u | (uint32_t)(((uint32_t *)g_cp - jc) & 0x3FFFFFF);
    emit32(0xD53BE050u);                  // mrs x16, cntvct_el0
    e_movconst(20, g_cal_base_ticks);
    e_rrr(A_SUB, 21, 16, 20, 1, 0);       // x21 = delta = ticks - base_ticks
    e_movconst(20, g_cal_mult);
    e_mul(22, 21, 20, 1);                 // lo = delta*mult
    e_umulh(23, 21, 20);                  // hi
    e_extr(24, 23, 22, FAST_SHIFT, 1);    // x24 = (hi:lo) >> 30 = ns_delta (128-bit safe)
    e_rrr(A_ADD, 24, 19, 24, 1, 0);       // x24 = base_ns + ns_delta = total_ns
    e_movconst(25, 1000000000ull);
    e_udiv(26, 24, 25, 1);                // sec
    e_msub(27, 26, 25, 24, 1);            // nsec = total - sec*1e9
    e_str(26, 6, 0);                      // ts->tv_sec   (rsi=x6)
    e_str(27, 6, 8);                      // ts->tv_nsec
    e_movconst(20, (uint64_t)&g_fast_count);
    e_ldr(21, 20, 0);
    e_addi(21, 21, 1, 1);
    e_str(21, 20, 0);    // g_fast_count++
    e_movconst(0, 0);    // rax = 0
    emit32(0xD51B4211u); // msr nzcv, x17  (restore guest flags)
    after[na++] = (uint32_t *)g_cp;
    emit32(0x14000000u); // b L_after

    // ---- gettimeofday: rax == 96 ----
    *m1 = (*m1 & 0xFF00001Fu) | ((uint32_t)(((uint32_t *)g_cp - m1) & 0x7FFFF) << 5);
    e_subi_s(16, 0, 96, 1); // subs x16, x0, #96
    to_slow[nsl++] = (uint32_t *)g_cp;
    e_bcond(1, 0);                 // b.ne -> slow
    e_movconst(19, g_cal_real_ns); // gettimeofday is REALTIME
    emit32(0xD53BE050u);           // mrs x16, cntvct_el0
    e_movconst(20, g_cal_base_ticks);
    e_rrr(A_SUB, 21, 16, 20, 1, 0);
    e_movconst(20, g_cal_mult);
    e_mul(22, 21, 20, 1);
    e_umulh(23, 21, 20);
    e_extr(24, 23, 22, FAST_SHIFT, 1);
    e_rrr(A_ADD, 24, 19, 24, 1, 0); // total_ns
    e_movconst(25, 1000000000ull);
    e_udiv(26, 24, 25, 1);     // sec
    e_msub(27, 26, 25, 24, 1); // nsec
    e_movconst(20, 1000);
    e_udiv(27, 27, 20, 1); // usec = nsec/1000
    e_str(26, 7, 0);       // tv->tv_sec   (rdi=x7)
    e_str(27, 7, 8);       // tv->tv_usec
    e_movconst(20, (uint64_t)&g_fast_count);
    e_ldr(21, 20, 0);
    e_addi(21, 21, 1, 1);
    e_str(21, 20, 0);
    e_movconst(0, 0);    // rax = 0
    emit32(0xD51B4211u); // msr nzcv, x17
    after[na++] = (uint32_t *)g_cp;
    emit32(0x14000000u); // b L_after

    // ---- slow path: restore guest flags, take the unchanged full R_SYSCALL exit ----
    for (int i = 0; i < nsl; i++) {
        uint32_t *s = to_slow[i];
        *s = (*s & 0xFF00001Fu) | ((uint32_t)(((uint32_t *)g_cp - s) & 0x7FFFF) << 5);
    }
    emit32(0xD51B4211u);              // msr nzcv, x17
    emit_exit_const(next, R_SYSCALL); // -> block_return (slow path ends the block)

    // ---- L_after: fall through to the caller's block terminator (chain to `next`) ----
    for (int i = 0; i < na; i++) {
        uint32_t *s = after[i];
        *s = 0x14000000u | (uint32_t)(((uint32_t *)g_cp - s) & 0x3FFFFFF);
    }
}
// Direct-branch chaining: if target already translated, single `b body`; else a full
// exit whose first insn is remembered and back-patched to `b body` later. (from jit.c)
static void emit_chain_exit(uint64_t target) {
    if (g_trace || g_nochain || g_threaded) {
        emit_exit_const(target, R_BRANCH);
        return;
    } // debug: no chaining -> exact rip per block
    void *body = map_body(target);
    uint32_t *slot = (uint32_t *)g_cp;
    if (body) {
        int64_t d = ((uint8_t *)body - (uint8_t *)slot) / 4;
        emit32(0x14000000u | ((uint32_t)d & 0x3FFFFFFu));
        return;
    }
    add_pend(slot, target);
    emit_exit_const(target, R_BRANCH);
}
// Indirect branch (ret / jmp reg / call reg) with the guest target already in x16.
// Probe the IBTC inline: HIT -> jump straight into the cached body (guest regs stay
// live, no spill/dispatch); MISS -> spill and flag the dispatcher to fill the cache.
// Scratch x16/x17/x19/x20/x21 are not guest registers here, and `sub` (not `subs`)
// keeps nzcv live, so the cached body is entered exactly like a chained block.
static void emit_ibranch(void) {
    if (g_trace || g_nochain || g_noibtc) { // debug: always dispatch (exact rip)
        e_str(16, 28, OFF_RIP);
        emit_spill();
        e_movconst(16, R_BRANCH);
        e_str(16, 28, OFF_RSN);
        e_movconst(16, (uint64_t)block_return);
        e_br(16);
        return;
    }
    emit32(0xD3423800u | (16 << 5) | 17);                          // ubfx x17, x16, #2, #13  ((tgt>>2)&0x1FFF)
    e_movconst(19, (uint64_t)g_ibtc);                              // x19 = &g_ibtc
    emit32(0x8B000000u | (17 << 16) | (4 << 10) | (19 << 5) | 19); // add x19, x19, x17, lsl #4   (slot)
    e_ldr(20, 19, 0);                                              // x20 = slot.target
    emit32(0xCB000000u | (16 << 16) | (20 << 5) | 20);             // sub x20, x20, x16  (NOT subs: keep nzcv)
    uint32_t *p_cbnz = (uint32_t *)g_cp;
    emit32(0); // cbnz x20, Lmiss
    e_ldr(21, 19, 8);
    e_br(21); // HIT: x21 = slot.body -> jump (regs live)
    uint32_t *miss = (uint32_t *)g_cp;
    e_str(16, 28, OFF_RIP);
    emit_spill(); // MISS: slow path
    e_movconst(16, R_BRANCH);
    e_str(16, 28, OFF_RSN);
    e_movconst(16, 1);
    e_str(16, 28, OFF_ICMISS); // dispatcher fills g_ibtc for cpu->rip
    e_movconst(16, (uint64_t)block_return);
    e_br(16);
    *p_cbnz = 0xB5000000u | (((uint32_t)(((uint8_t *)miss - (uint8_t *)p_cbnz) / 4) & 0x7FFFF) << 5) | 20;
}
