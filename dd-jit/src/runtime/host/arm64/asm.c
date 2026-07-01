// dd/runtime/host/arm64 -- the HOST ARM64 ASSEMBLER: emit32 + the e_* instruction encoders. Host is
// always arm64. Pure encoders (no engine/guest knowledge) -- the lowest layer, below engine/ and used by
// the engine block-ABI stubs (engine/stubs.c) and the aarch64 translator. Extracted from the former
// engine/emit_arm64.c (C7: home the assembler in its own host layer). Emits into the engine cursor g_cp.

// ---------------- instruction emitters ----------------
static void emit32(uint32_t in) {
    *(uint32_t *)g_cp = in;
    g_cp += 4;
}
static void e_str(int rt, int rn, int off) { emit32(0xF9000000u | (((unsigned)off / 8) << 10) | (rn << 5) | rt); }
static void e_ldr(int rt, int rn, int off) { emit32(0xF9400000u | (((unsigned)off / 8) << 10) | (rn << 5) | rt); }
// mov sp, xrn
static void e_mov_sp_from(int rn) { emit32(0x9100001Fu | (rn << 5)); }
// mov xrd, sp
static void e_mov_from_sp(int rd) { emit32(0x910003E0u | rd); }
static void e_movz(int rd, uint32_t imm16, int sh) { emit32(0xD2800000u | (sh << 21) | (imm16 << 5) | rd); }
static void e_movk(int rd, uint32_t imm16, int sh) { emit32(0xF2800000u | (sh << 21) | (imm16 << 5) | rd); }
static void e_br(int rn) { emit32(0xD61F0000u | (rn << 5)); }
static void e_movconst(int rd, uint64_t v) {
    e_movz(rd, v & 0xffff, 0);
    if ((v >> 16) & 0xffff) e_movk(rd, (v >> 16) & 0xffff, 1);
    if ((v >> 32) & 0xffff) e_movk(rd, (v >> 32) & 0xffff, 2);
    if ((v >> 48) & 0xffff) e_movk(rd, (v >> 48) & 0xffff, 3);
}
// §B host bl/ret RAS: GP-register encoding helpers.
static void e_stp(int t, int t2, int n, int off) {
    emit32(0xA9000000u | (((unsigned)(off / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t);
}
static void e_ldp(int t, int t2, int n, int off) {
    emit32(0xA9400000u | (((unsigned)(off / 8) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t);
}
static void e_addi(int d, int n, unsigned imm) {
    emit32(0x91000000u | ((imm & 0xFFF) << 10) | (n << 5) | d);
// add xd,xn,#imm
}
static void e_addlsl4(int d, int n, int m) {
    emit32(0x8B000000u | (m << 16) | (4 << 10) | (n << 5) | d);
// add xd,xn,xm,lsl #4
}
static void e_addlsl3(int d, int n, int m) {
    emit32(0x8B000000u | (m << 16) | (3 << 10) | (n << 5) | d);
// add xd,xn,xm,lsl #3
}
// and xd,xn,#0x3FF
static void e_and1023(int d, int n) { emit32(0x92400000u | (9 << 10) | (n << 5) | d); }
// mov xd,xm
static void e_movr(int d, int m) { emit32(0xAA0003E0u | (m << 16) | d); }
static void e_subi(int d, int n, unsigned imm) {
    emit32(0xD1000000u | ((imm & 0xFFF) << 10) | (n << 5) | d);
// sub xd,xn,#imm
}
// ret (host x30)
static void e_hret(void) { emit32(0xD65F03C0u); }
static void e_adrp_add(int rd, uint64_t target) {
    // adrp's page immediate is PC-relative; this instruction EXECUTES from the RX alias, so it
    // must be computed against the RX-alias address of the emit cursor (g_cp is the RW alias).
    // `target` is always an absolute external address (e.g. &g_ibtc), so no J_RX on it.
    int64_t off = (int64_t)((target & ~0xFFFull) - ((uint64_t)J_RX(g_cp) & ~0xFFFull)) >> 12;
    emit32(0x90000000u | (((uint32_t)off & 3) << 29) | (((uint32_t)(off >> 2) & 0x7FFFF) << 5) | rd);
    emit32(0x91000000u | (((uint32_t)(target & 0xFFF)) << 10) | (rd << 5) | rd);
}
// Recover THIS thread's struct cpu* from host TLS into reg `r`. macOS keeps the
// pthread self pointer in TPIDRRO_EL0 with the low 3 bits = cpu id (mask them);
// pthread TSD[key] then holds our cpu pointer. Replaces a fixed global so every
// guest thread sees its own cpu.
static void e_load_cpu(int r) {
    // mrs r, TPIDRRO_EL0
    emit32(0xD53BD060u | r);
    // and r, r, #0xFFFFFFFFFFFFFFF8
    emit32(0x9243F000u | (r << 5) | r);
    // ldr r, [r, #key*8]   (= TSD[key])
    e_ldr(r, r, (int)(g_cpu_key * 8));
}

// STUR/LDUR (unscaled) — for [sp,#-16] red-zone scratch at block exit.
static void e_stur(int rt, int rn, int imm9) {
    emit32(0xF8000000u | (((unsigned)imm9 & 0x1FF) << 12) | (rn << 5) | rt);
}
static void e_ldur(int rt, int rn, int imm9) {
    emit32(0xF8400000u | (((unsigned)imm9 & 0x1FF) << 12) | (rn << 5) | rt);
}
static void e_str_q(int t, int rn, int off) {
    emit32(0x3D800000u | (((unsigned)off / 16) << 10) | (rn << 5) | t);
// str q,[xn,#off]
}
static void e_ldr_q(int t, int rn, int off) {
    emit32(0x3DC00000u | (((unsigned)off / 16) << 10) | (rn << 5) | t);
// ldr q,[xn,#off]
}
static void e_stp_q(int t1, int t2, int rn, int off) {
    emit32(0xAD000000u | (((unsigned)(off / 16) & 0x7F) << 15) | (t2 << 10) | (rn << 5) | t1);
}
static void e_ldp_q(int t1, int t2, int rn, int off) {
    emit32(0xAD400000u | (((unsigned)(off / 16) & 0x7F) << 15) | (t2 << 10) | (rn << 5) | t1);
}
