// dd/runtime/jit -- aarch64 HOST-code emitters (emit32 + e_*). Host is always arm64. Block
// prologue/spill, the indirect-branch IBTC + per-site monomorphic IC, exit trampolines.

// ---------------- instruction emitters ----------------
static void emit32(uint32_t in) {
    *(uint32_t *)g_cp = in;
    g_cp += 4;
}
static void e_str(int rt, int rn, int off) { emit32(0xF9000000u | (((unsigned)off / 8) << 10) | (rn << 5) | rt); }
static void e_ldr(int rt, int rn, int off) { emit32(0xF9400000u | (((unsigned)off / 8) << 10) | (rn << 5) | rt); }
static void e_mov_sp_from(int rn) { emit32(0x9100001Fu | (rn << 5)); } // mov sp, xrn
static void e_mov_from_sp(int rd) { emit32(0x910003E0u | rd); }        // mov xrd, sp
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
} // add xd,xn,#imm
static void e_addlsl4(int d, int n, int m) {
    emit32(0x8B000000u | (m << 16) | (4 << 10) | (n << 5) | d);
} // add xd,xn,xm,lsl #4
static void e_addlsl3(int d, int n, int m) {
    emit32(0x8B000000u | (m << 16) | (3 << 10) | (n << 5) | d);
}                                                                                       // add xd,xn,xm,lsl #3
static void e_and1023(int d, int n) { emit32(0x92400000u | (9 << 10) | (n << 5) | d); } // and xd,xn,#0x3FF
static void e_movr(int d, int m) { emit32(0xAA0003E0u | (m << 16) | d); }               // mov xd,xm
static void e_subi(int d, int n, unsigned imm) {
    emit32(0xD1000000u | ((imm & 0xFFF) << 10) | (n << 5) | d);
}                                                 // sub xd,xn,#imm
static void e_hret(void) { emit32(0xD65F03C0u); } // ret (host x30)
static void e_adrp_add(int rd, uint64_t target) {
    int64_t off = (int64_t)((target & ~0xFFFull) - ((uint64_t)g_cp & ~0xFFFull)) >> 12;
    emit32(0x90000000u | (((uint32_t)off & 3) << 29) | (((uint32_t)(off >> 2) & 0x7FFFF) << 5) | rd);
    emit32(0x91000000u | (((uint32_t)(target & 0xFFF)) << 10) | (rd << 5) | rd);
}
// Recover THIS thread's struct cpu* from host TLS into reg `r`. macOS keeps the
// pthread self pointer in TPIDRRO_EL0 with the low 3 bits = cpu id (mask them);
// pthread TSD[key] then holds our cpu pointer. Replaces a fixed global so every
// guest thread sees its own cpu.
static void e_load_cpu(int r) {
    emit32(0xD53BD060u | r);            // mrs r, TPIDRRO_EL0
    emit32(0x9243F000u | (r << 5) | r); // and r, r, #0xFFFFFFFFFFFFFFF8
    e_ldr(r, r, (int)(g_cpu_key * 8));  // ldr r, [r, #key*8]   (= TSD[key])
}

static void block_return(void);
// No host register is permanently reserved, so ALL 31 guest GPRs (incl. x28) live
// in the real registers during a block. The cpu pointer is recovered from this
// global only at block boundaries (set by the dispatcher before each run_block).
static int g_trace;
static const char *g_exe_path = "";

// STUR/LDUR (unscaled) — for [sp,#-16] red-zone scratch at block exit.
static void e_stur(int rt, int rn, int imm9) {
    emit32(0xF8000000u | (((unsigned)imm9 & 0x1FF) << 12) | (rn << 5) | rt);
}
static void e_ldur(int rt, int rn, int imm9) {
    emit32(0xF8400000u | (((unsigned)imm9 & 0x1FF) << 12) | (rn << 5) | rt);
}
static void e_str_q(int t, int rn, int off) {
    emit32(0x3D800000u | (((unsigned)off / 16) << 10) | (rn << 5) | t);
} // str q,[xn,#off]
static void e_ldr_q(int t, int rn, int off) {
    emit32(0x3DC00000u | (((unsigned)off / 16) << 10) | (rn << 5) | t);
} // ldr q,[xn,#off]
static void e_stp_q(int t1, int t2, int rn, int off) {
    emit32(0xAD000000u | (((unsigned)(off / 16) & 0x7F) << 15) | (t2 << 10) | (rn << 5) | t1);
}
static void e_ldp_q(int t1, int t2, int rn, int off) {
    emit32(0xAD400000u | (((unsigned)(off / 16) & 0x7F) << 15) | (t2 << 10) | (rn << 5) | t1);
}

// Prologue: entered with x0 = &cpu. Restore flags, load guest sp + ALL GPRs, x0 last.
static void emit_prologue(void) {
    e_ldr(9, 0, OFF_SP);
    e_mov_sp_from(9);
    e_ldr(9, 0, OFF_NZCV);
    emit32(0xD51B4200u | 9); // msr nzcv, x9 (restore flags)
    for (int t = 0; t < 32; t += 2)
        e_ldp_q(t, t + 1, 0, OFF_V + t * 16); // guest V0..V31 (paired)
    for (int r = 1; r <= 30; r++)
        if (!is_stolen(r)) e_ldr(r, 0, r * 8); // x18,x28 stolen: live only in cpu->x[]
    emit32(0xAA0003FCu);                       // mov x28, x0  -- reserve real x28 = cpu (x0 still = cpu here)
    e_ldr(0, 0, 0);
    // IBTC indirect-entry stub: an inline-cache hit jumps to (body-8) with the guest
    // x16/x17 stashed in the red zone; restore them, then fall into body. Fresh/direct
    // entries jump straight to body via this `b #12`, skipping the restores.
    emit32(0x14000003u); // b #12 -> body
    e_ldur(16, 31, -16); // body_ind: ldr x16, [sp,#-16]
    e_ldur(17, 31, -24); //           ldr x17, [sp,#-24]
}
// Spill: recover &cpu into x0 (guest x0 stashed in the red zone), store all GPRs+sp+flags.
static void emit_spill(void) {
    e_stur(0, 31, -16); // save guest x0
    emit32(0xD53B4200u | 0);
    e_stur(0, 31, -24); // mrs x0, nzcv; stash flags (none of this clobbers flags)
    e_load_cpu(0);      // x0 = cpu (thread-local)
    for (int t = 0; t < 32; t += 2)
        e_stp_q(t, t + 1, 0, OFF_V + t * 16); // guest V0..V31 (paired)
    for (int r = 1; r <= 30; r++)
        if (!is_stolen(r)) e_str(r, 0, r * 8); // skip x18 (volatile) + x28 (= cpu)
    e_ldur(9, 31, -16);
    e_str(9, 0, 0); // cpu->x[0] = saved guest x0
    e_ldur(9, 31, -24);
    e_str(9, 0, OFF_NZCV); // cpu->nzcv
    e_mov_from_sp(9);
    e_str(9, 0, OFF_SP); // cpu->sp
}
static void emit_exit_const(uint64_t pc, uint64_t reason) {
    emit_spill(); // x0 = cpu
    e_movconst(9, pc);
    e_str(9, 0, OFF_PC);
    e_movconst(9, reason);
    e_str(9, 0, OFF_RSN);
    e_movconst(9, (uint64_t)block_return);
    e_br(9); // x0=cpu -> block_return
}
static void emit_exit_reg(int rn, uint64_t reason) {
    emit_spill(); // x0 = cpu
    e_ldr(9, 0, rn * 8);
    e_str(9, 0, OFF_PC); // cpu->pc = cpu->x[rn]
    e_movconst(9, reason);
    e_str(9, 0, OFF_RSN);
    e_movconst(9, (uint64_t)block_return);
    e_br(9);
}
// IBTC: inline-cache an indirect branch (br/blr/ret). If the guest target equals this
// site's cached target, jump straight into the cached block's indirect-entry (body-8) --
// no spill, no V-register save, no dispatcher round-trip. On a miss, take the full exit
// and hand the cache-site address to the dispatcher, which fills it once the target is
// resolved. x16/x17 are the scratch pair; the indirect-entry stub restores them.
static void emit_ibtc_miss(int rn) { // shared IBTC miss tail (x16/x17 restored)
    emit_spill();                    // slow path: x0 = cpu
    e_ldr(9, 0, rn * 8);
    e_str(9, 0, OFF_PC); // cpu->pc = guest target
    e_movconst(9, R_BRANCH);
    e_str(9, 0, OFF_RSN);
    e_movconst(9, 1);
    e_str(9, 0, OFF_ICSITE); // flag: indirect miss -> insert into IBTC
    e_movconst(9, (uint64_t)block_return);
    e_br(9);
}
static void emit_ibranch(int rn) {
    if (rn == 18 || rn == 28) {
        emit_exit_reg(rn, R_BRANCH);
        return;
    } // host x18 volatile / x28=cpu: can't hold target
    if (rn == 30)
        e_ldr(30, CPUREG, 30 * 8); // ret/br/blr x30: load guest x30 into the FREE host link reg, then
                                   // run the normal IBTC (per-site + shared hash) -- fast, lock-free, correct
    if (rn == 16 || rn == 17) {
        // Hot case: a function pointer called via x16/x17 (qsort comparator, vtable). The
        // target IS a scratch reg, read from its red-zone copy. A per-site monomorphic
        // cache turns the common "same target every time" case into a direct compare+jump;
        // a miss falls to the shared hash (recompute since x16/x17 are scratch).
        int other = (rn == 16) ? 17 : 16, tslot = (rn == 16) ? -16 : -24;
        e_stur(16, 31, -16);
        e_stur(17, 31, -24);
        // --- per-site monomorphic fast path: xRn (target) still live here ---
        uint32_t *p_ldrt = (uint32_t *)g_cp;
        emit32(0);                                               // ldr xOTHER, Lsite_tgt
        emit32(0xCB000000u | (rn << 16) | (other << 5) | other); // sub xOTHER, xOTHER, xRn
        uint32_t *p_cbslow = (uint32_t *)g_cp;
        emit32(0); // cbnz xOTHER, Lslow
        uint32_t *p_ldrb = (uint32_t *)g_cp;
        emit32(0); // ldr x16, Lsite_body
        e_br(16);  // HIT -> body_ind (restores x16/x17)
        uint32_t *Lslow = (uint32_t *)g_cp;
        // --- shared hash IBTC (recompute slot) ---
        e_ldur(16, 31, tslot);                // x16 = target
        e_stur(16, 31, -32);                  // stash target at [sp,-32]
        emit32(0xD342FC00u | (16 << 5) | 17); // lsr x17, x16, #2
        emit32(0x92403000u | (17 << 5) | 17); // and x17, x17, #0x1FFF
        e_adrp_add(16, (uint64_t)g_ibtc);
        emit32(0x8B000000u | (17 << 16) | (4 << 10) | (16 << 5) | 16);
        e_ldr(17, 16, 0); // x17 = slot.target
        e_ldur(16, 31, -32);
        emit32(0xCB000000u | (16 << 16) | (17 << 5) | 17); // sub x17, x17, x16
        uint32_t *p_cbnz = (uint32_t *)g_cp;
        emit32(0); // cbnz x17, miss
        emit32(0xD342FC00u | (16 << 5) | 17);
        emit32(0x92403000u | (17 << 5) | 17);
        e_adrp_add(16, (uint64_t)g_ibtc);
        emit32(0x8B000000u | (17 << 16) | (4 << 10) | (16 << 5) | 16);
        e_ldr(16, 16, 8);
        e_br(16);
        uint32_t *miss = (uint32_t *)g_cp;
        e_ldur(16, 31, -16);
        e_ldur(17, 31, -24);
        emit_spill();
        e_ldr(9, 0, rn * 8);
        e_str(9, 0, OFF_PC);
        e_movconst(9, R_BRANCH);
        e_str(9, 0, OFF_RSN);
        uint32_t *p_adr = (uint32_t *)g_cp;
        emit32(0); // adr x9, Lsite_tgt -> dispatcher fills both caches
        e_str(9, 0, OFF_ICSITE);
        e_movconst(9, (uint64_t)block_return);
        e_br(9);
        if ((uint64_t)g_cp & 7) emit32(0);
        uint8_t *Lt = g_cp;
        *(uint64_t *)g_cp = 0;
        g_cp += 8;
        uint8_t *Lb = g_cp;
        *(uint64_t *)g_cp = 0;
        g_cp += 8;
        *p_ldrt = 0x58000000u | (((uint32_t)((Lt - (uint8_t *)p_ldrt) / 4) & 0x7FFFF) << 5) | other;
        *p_cbslow = 0xB5000000u | (((uint32_t)(((uint8_t *)Lslow - (uint8_t *)p_cbslow) / 4) & 0x7FFFF) << 5) | other;
        *p_ldrb = 0x58000000u | (((uint32_t)((Lb - (uint8_t *)p_ldrb) / 4) & 0x7FFFF) << 5) | 16;
        *p_cbnz = 0xB5000000u | (((uint32_t)(((uint8_t *)miss - (uint8_t *)p_cbnz) / 4) & 0x7FFFF) << 5) | 17;
        int64_t ao = Lt - (uint8_t *)p_adr;
        *p_adr = 0x10000000u | ((uint32_t)(ao & 3) << 29) | (((uint32_t)(ao >> 2) & 0x7FFFF) << 5) | 9;
        return;
    }
    e_stur(16, 31, -16);
    e_stur(17, 31, -24); // stash x16/x17 scratch
    // --- per-site monomorphic cache (ahead of the shared hash; ret/br/blr are usually monomorphic) ---
    uint32_t *p_ldrt = (uint32_t *)g_cp;
    emit32(0);                                         // ldr x16, Lsite_tgt
    emit32(0xCB000000u | (rn << 16) | (16 << 5) | 16); // sub x16, x16, xRn   (xRn live; rn != 16/17 here)
    uint32_t *p_cbslow = (uint32_t *)g_cp;
    emit32(0); // cbnz x16, Lhash
    uint32_t *p_ldrb = (uint32_t *)g_cp;
    emit32(0); // ldr x16, Lsite_body
    e_br(16);  // HIT -> body_ind (restores x16/x17)
    uint32_t *Lhash = (uint32_t *)g_cp;
    emit32(0xD3423800u | (rn << 5) | 16);                          // ubfx x16, xRn, #2, #13  ((xRn>>2)&0x1FFF)
    e_adrp_add(17, (uint64_t)g_ibtc);                              // x17 = &g_ibtc  (2 instr)
    emit32(0x8B000000u | (16 << 16) | (4 << 10) | (17 << 5) | 16); // add x16, x17, x16, lsl #4  (slot)
    e_ldr(17, 16, 0);                                              // x17 = slot.target
    emit32(0xCB000000u | (rn << 16) | (17 << 5) | 17);             // sub x17, x17, xRn
    uint32_t *p_cbnz = (uint32_t *)g_cp;
    emit32(0);        // cbnz x17, Lmiss
    e_ldr(16, 16, 8); // x16 = slot.body (body_ind)
    e_br(16);         // HIT -> jump
    uint32_t *miss = (uint32_t *)g_cp;
    e_ldur(16, 31, -16);
    e_ldur(17, 31, -24); // restore scratch
    emit_spill();        // slow path: x0 = cpu
    e_ldr(9, 0, rn * 8);
    e_str(9, 0, OFF_PC); // cpu->pc = guest target
    e_movconst(9, R_BRANCH);
    e_str(9, 0, OFF_RSN);
    uint32_t *p_adr = (uint32_t *)g_cp;
    emit32(0); // adr x9, Lsite_tgt -> dispatcher fills the per-site cache
    e_str(9, 0, OFF_ICSITE);
    e_movconst(9, (uint64_t)block_return);
    e_br(9);
    if ((uint64_t)g_cp & 7) emit32(0);
    uint8_t *Lt = g_cp;
    *(uint64_t *)g_cp = 0;
    g_cp += 8; // Lsite_tgt
    uint8_t *Lb = g_cp;
    *(uint64_t *)g_cp = 0;
    g_cp += 8; // Lsite_body
    *p_ldrt = 0x58000000u | (((uint32_t)((Lt - (uint8_t *)p_ldrt) / 4) & 0x7FFFF) << 5) | 16;
    *p_cbslow = 0xB5000000u | (((uint32_t)(((uint8_t *)Lhash - (uint8_t *)p_cbslow) / 4) & 0x7FFFF) << 5) | 16;
    *p_ldrb = 0x58000000u | (((uint32_t)((Lb - (uint8_t *)p_ldrb) / 4) & 0x7FFFF) << 5) | 16;
    *p_cbnz = 0xB5000000u | (((uint32_t)(((uint8_t *)miss - (uint8_t *)p_cbnz) / 4) & 0x7FFFF) << 5) | 17;
    int64_t ao = Lt - (uint8_t *)p_adr;
    *p_adr = 0x10000000u | ((uint32_t)(ao & 3) << 29) | (((uint32_t)(ao >> 2) & 0x7FFFF) << 5) | 9;
}

// A direct-branch exit to a CONSTANT target. If the target is already translated,
// emit a single `b target.body` (regs stay live, no dispatcher round-trip). Otherwise
// emit a full spill-exit whose first instruction is remembered so it can later be
// back-patched into that `b` once the target gets translated.
static void emit_chain_exit(uint64_t target) {
    void *body = map_body(target);
    uint32_t *slot = (uint32_t *)g_cp;
    if (body) {
        int64_t d = ((uint8_t *)body - (uint8_t *)slot) / 4;
        emit32(0x14000000u | ((uint32_t)d & 0x3FFFFFFu)); // b target.body
        return;
    }
    add_pend(slot, target);
    emit_exit_const(target, R_BRANCH); // slot (= first insn) is patched to `b body` later
}

static int64_t sext(uint64_t v, int bits) {
    uint64_t m = 1ull << (bits - 1);
    return (int64_t)((v ^ m) - m);
}
