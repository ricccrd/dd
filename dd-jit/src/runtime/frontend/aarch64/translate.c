// dd/runtime/frontend/aarch64 -- the aarch64-Linux -> arm64-host transliterator. Same-ISA: copy
// most instructions verbatim; MANGLE only stolen-register (x18/x28/x30) users. Optimizations: LSE
// atomic upgrade, §B shadow-return prediction (depth-gated), tier-2 purity gate. See OPTIMIZATIONS.md.

// ---- x18 stealing ----
// macOS asynchronously zeroes the real x18 (it is reserved on Apple platforms), but a
// Linux guest uses x18 as a normal GP register. So guest x18 must NEVER live in the real
// x18: it lives in cpu->x[18], and any guest instruction that names x18 is rewritten to
// use a scratch loaded from / stored back to cpu->x[18].
//
// gpr_field_mask: which of the 4 register fields are GP registers for this instruction.
//   bit0 = [4:0] (Rd/Rt)   bit1 = [9:5] (Rn)   bit2 = [20:16] (Rm/Rs)   bit3 = [14:10] (Rt2/Ra)
static int gpr_field_mask(uint32_t in) {
    uint32_t op = (in >> 25) & 0xF;
    if (op == 8 || op == 9) {                                   // Data-processing immediate
        if ((in & 0x1F000000u) == 0x10000000u) return 1;        //   adr/adrp: Rd
        if ((in & 0x1F800000u) == 0x12800000u) return 1;        //   move wide: Rd (imm in [20:5])
        if ((in & 0x1F800000u) == 0x13800000u) return 1|2|4;    //   extr: Rd,Rn,Rm
        return 1 | 2;                                           //   add/sub-imm, logical-imm, bitfield: Rd,Rn
    }
    if (op == 0xA || op == 0xB) {                               // Branches/Exception/System
        if ((in & 0xFFD00000u) == 0xD5100000u) return 1;        //   mrs/msr <-> Rt
        return 0;                                               //   branches: handled as block enders
    }
    if ((in & 0x0A000000u) == 0x08000000u) {                    // Loads and Stores
        int v = (in >> 26) & 1, m = 2;                          //   Rn[9:5] base is GP
        if (!v) m |= 1;                                         //   Rt[4:0] GP unless SIMD/FP
        if ((in & 0x3B200C00u) == 0x38200800u) m |= 4;          //   register offset: Rm[20:16]
        if ((in & 0x3A000000u) == 0x28000000u && !v) m |= 8;    //   load/store pair: Rt2[14:10] (GP only)
        if ((in & 0x3F000000u) == 0x08000000u) m |= 4 | 8;      //   exclusive: Rs[20:16], Rt2[14:10]
        return m;
    }
    if ((in & 0x0E000000u) == 0x0A000000u) {                    // Data-processing register
        if ((in & 0x1F000000u) == 0x1B000000u) return 1|2|4|8;  //   3-source: Rd,Rn,Rm,Ra
        if ((in & 0x5FE00000u) == 0x5AC00000u) return 1|2;      //   1-source: Rd,Rn (Rm field is opcode)
        if ((in & 0x1FE00000u) == 0x1A400000u) return (in & 0x800u) ? 2 : (2|4);  // ccmp/ccmn: [4:0]=nzcv; imm -> Rn only
        return 1 | 2 | 4;                                      //   logical/addsub-reg/cond-sel/2-source
    }
    return 0;                                                   // SIMD/FP data: V registers only
}
static int field_is(uint32_t in, int bit, int shift) { return is_stolen((in >> shift) & 0x1F) && bit; }
static int uses_x18(uint32_t in, int mask) {   // "uses a STOLEN reg" (x18 / x28 [/ x30 in Stage B])
    return field_is(in, mask & 1, 0) || field_is(in, mask & 2, 5)
        || field_is(in, mask & 4, 16) || field_is(in, mask & 8, 10);
}
// Emit a guest insn that references stolen reg(s): for each, a scratch S = cpu->x[stolen]; run the
// insn with the stolen field(s) replaced by scratch(es); store back. Real x28 = cpu is the base;
// scratch originals are spilled to cpu->mscratch (NOT the stack -- that would collide with the
// guest's own stp/ldp frame stores + writeback). At most two distinct stolen regs in one insn.
static void emit_mangled_x18(uint32_t in, int mask) {
    static const int shifts[4] = {0, 5, 16, 10}, mbits[4] = {1, 2, 4, 8};
    int stolen[2], ns = 0, used = 0;
    for (int k = 0; k < 4; k++) if (mask & mbits[k]) { int rf = (in >> shifts[k]) & 0x1F; used |= 1 << rf;
        if (is_stolen(rf)) { int seen = 0; for (int j = 0; j < ns; j++) if (stolen[j] == rf) seen = 1; if (!seen) stolen[ns++] = rf; } }
    int sc[2], nsc = 0;
    for (int r = 0; r <= 27 && nsc < ns; r++) if (!(used & (1 << r)) && !is_stolen(r)) sc[nsc++] = r;
    for (int i = 0; i < ns; i++) e_str(sc[i], CPUREG, (int)OFF_MSCRATCH + 8 * i);   // spill scratch -> cpu->mscratch
    for (int i = 0; i < ns; i++) e_ldr(sc[i], CPUREG, stolen[i] * 8);               // scratch = cpu->x[stolen]
    uint32_t m = in;
    for (int k = 0; k < 4; k++) if (mask & mbits[k]) { int rf = (m >> shifts[k]) & 0x1F;
        if (is_stolen(rf)) { int s = sc[0]; for (int i = 0; i < ns; i++) if (stolen[i] == rf) s = sc[i];
            m = (m & ~(0x1Fu << shifts[k])) | ((unsigned)s << shifts[k]); } }
    emit32(m);
    for (int i = 0; i < ns; i++) e_str(sc[i], CPUREG, stolen[i] * 8);               // cpu->x[stolen] = scratch
    for (int i = 0; i < ns; i++) e_ldr(sc[i], CPUREG, (int)OFF_MSCRATCH + 8 * i);   // restore scratch
}
// For instructions that WRITE x18 via a special path (adr/ldr-literal/mrs): save x0,x1 to
// the red zone, x1 := cpu. The case then computes a value into x0 and stores it to
// cpu->x[18]; x18_epilog restores x0,x1.
static void x18_prolog(void) { e_stur(0, 31, -16); e_stur(1, 31, -24); e_load_cpu(1); }
static void x18_epilog(void) { e_ldur(1, 31, -24); e_ldur(0, 31, -16); }
// §B: store a constant to cpu->x[30] (the stolen guest link reg). x28=cpu; x0 scratched via mscratch.
static void emit_set_x30(uint64_t val) {
    e_str(0, CPUREG, (int)OFF_MSCRATCH); e_movconst(0, val); e_str(0, CPUREG, 30 * 8); e_ldr(0, CPUREG, (int)OFF_MSCRATCH);
}
// §B shadow push: cpu->x[30] = gpc+4; sstk[ssp&1023] = (gpc+4, &Lcont); ssp++. x0..x2 spilled to
// cpu->mscratch (all guest regs are live across the call). Returns the `adr x1,Lcont` to backpatch.
static uint32_t *emit_shadow_push(uint64_t gpc) {
    int M = (int)OFF_MSCRATCH;
    e_stp(0, 1, CPUREG, M); e_stp(2, 3, CPUREG, M + 16);             // spill x0..x3 -> mscratch (paired: 2 stp not 4 str)
    e_movconst(0, gpc + 4); e_str(0, CPUREG, 30 * 8);                 // x0 = guest_ret; cpu->x[30] = guest_ret (ALWAYS)
    e_ldr(1, CPUREG, (int)OFF_SSP);                                   // x1 = ssp (capped at 1024)
    uint32_t *p_full = (uint32_t *)g_cp; emit32(0);                   // tbnz x1, #10, Lskip (ssp==1024 -> overflow; no flags)
    e_addlsl4(2, CPUREG, 1); e_addi(2, 2, (unsigned)OFF_SSTK);        // x2 = C + idx*16 + OFF_SSTK = &sstk[2*ssp]
    uint32_t *p_adr = (uint32_t *)g_cp; emit32(0);                    // adr x3, Lcont (host_ret; backpatched)
    e_stp(0, 3, 2, 0);                                               // sstk[2*ssp] = (guest_ret, host_ret=&Lcont)
    e_mov_from_sp(3); e_addlsl3(2, CPUREG, 1); e_str(3, 2, (int)OFF_GSP);  // gsp[ssp] = current guest SP (frame disambiguator)
    e_addi(1, 1, 1); e_str(1, CPUREG, (int)OFF_SSP);                  // ssp++
    uint8_t *Lskip = g_cp;
    *p_full = 0x37000000u | (10u << 19) | (((uint32_t)(((uint8_t *)Lskip - (uint8_t *)p_full) / 4) & 0x3FFF) << 5) | 1;  // tbnz x1,#10
    e_ldp(0, 1, CPUREG, M); e_ldp(2, 3, CPUREG, M + 16);             // restore x0..x3 (paired: 2 ldp not 4 ldr)
    return p_adr;
}
// §B profile gate: scan the target's entry block. A LEAF function (reaches `ret` with no bl/blr
// first) gains nothing from the shadow-RAS -- its monomorphic return is predicted by the per-site IC
// -- so paying the per-bl shadow push is pure overhead (floatk: sqrt/sin/pow). Only non-leaf targets
// (depth -> the hardware RAS predicts nested returns: stringk/recursion) get §B. Static, no profiling
// overhead; the ret auto-adapts (no frame pushed -> classify falls to the IC return).
// Scan target's straight-line extent (bounded by forward-branch reach). Returns -1 if a blr (unknown
// callee), else the count of direct-call (bl) targets, writing up to `max` of them to calls[].
static int scan_calls(uint64_t target, uint64_t calls[], int max) {
    int64_t reach = 0; int n = 0;
    for (int i = 0; i < 64; i++) {
        uint32_t in = *(uint32_t *)(target + (uint64_t)i * 4);
        if ((in & 0xFFFFFC1Fu) == 0xD63F0000u) return -1;                                       // blr -> unknown callee
        if ((in & 0xFC000000u) == 0x94000000u) { if (n < max) calls[n] = target + (uint64_t)i * 4 + ((uint64_t)sext(in & 0x3FFFFFF, 26) << 2); n++; }  // bl
        int64_t off = 0; int isb = 0;
        if ((in & 0xFF000010u) == 0x54000000u) { off = sext((in >> 5) & 0x7FFFF, 19); isb = 1; }          // b.cond
        else if ((in & 0x7E000000u) == 0x34000000u) { off = sext((in >> 5) & 0x7FFFF, 19); isb = 1; }     // cbz/cbnz
        else if ((in & 0x7E000000u) == 0x36000000u) { off = sext((in >> 5) & 0x3FFF, 14); isb = 1; }      // tbz/tbnz
        else if ((in & 0xFC000000u) == 0x14000000u) { off = sext(in & 0x3FFFFFF, 26); isb = 1; }          // b
        if (isb && off > 0 && i + off < 64 && i + off > reach) reach = i + off;
        if ((in & 0xFFFFFC1Fu) == 0xD65F0000u || (in & 0xFC000000u) == 0x14000000u || (in & 0xFFFFFC1Fu) == 0xD61F0000u)
            if (i >= reach) return n;                                                          // terminal past all branches
    }
    return n;
}
static int is_leaf0(uint64_t t) { uint64_t c[1]; return scan_calls(t, c, 0) == 0; }            // no calls at all
// §B helps only on DEPTH (the RAS predicts nested returns). A leaf or a depth-2 "shallow" function
// (all its calls go to leaves: sqrt/sin/pow's helpers) gains nothing -> keep cheap Stage-B. Only a
// function that calls a NON-leaf (or recurses, or calls indirectly) is deep enough to pay the push.
static int target_is_leaf(uint64_t target) {
    uint64_t calls[16]; int n = scan_calls(target, calls, 16);
    if (n < 0) return 0;                                                     // indirect callee -> assume deep -> §B
    for (int i = 0; i < n && i < 16; i++) if (!is_leaf0(calls[i])) return 0; // calls a non-leaf -> deep -> §B
    return 1;                                                               // leaf / all-leaf-calls (shallow) -> Stage-B
}
// §B guest bl: push shadow, host `bl body(target)` (RAS pushes &Lcont), Lcont continues at gpc+4.
static void emit_bl_ras(uint64_t gpc, uint64_t target) {
    if (target_is_leaf(target)) { emit_set_x30(gpc + 4); emit_chain_exit(target); return; }   // leaf -> cheap Stage-B (IC return)
    uint32_t *p_adr = emit_shadow_push(gpc);
    void *body = map_body(target);
    uint32_t *slot = (uint32_t *)g_cp;
    if (body) { int64_t d = ((uint8_t *)body - (uint8_t *)slot) / 4; emit32(0x94000000u | ((uint32_t)d & 0x3FFFFFFu)); }  // host bl body (RAS pushes &Lcont)
    else { add_pend2(slot, target, 1); emit_exit_const(target, R_BRANCH); }  // not translated yet: spill-exit (slot patched to `bl body`)
    uint8_t *Lcont = g_cp;                                           // host ret lands here
    int64_t ao = Lcont - (uint8_t *)p_adr;
    *p_adr = 0x10000000u | ((uint32_t)(ao & 3) << 29) | (((uint32_t)((ao >> 2) & 0x7FFFF)) << 5) | 3;  // adr x3, Lcont
    emit_chain_exit(gpc + 4);                                        // after the call returns -> gpc+4
}
// §B guest ret: if cpu->x[30] == shadow-top guest_ret, pop + real x30=host_ret + host `ret`
// (hardware-RAS predicted). Else fall back to the dispatcher reading cpu->x[30]. Never lands wrong.
static void emit_shadow_ret(void) {
    int M = (int)OFF_MSCRATCH;
    e_stp(0, 1, CPUREG, M); e_stp(2, 3, CPUREG, M + 16);             // spill x0..x3 (paired)
    e_ldr(0, CPUREG, (int)OFF_SSP);                                  // x0 = ssp
    uint32_t *p_cbz = (uint32_t *)g_cp; emit32(0);                   // cbz x0, Lfb (empty shadow)
    e_subi(0, 0, 1);                                                // x0 = ssp-1 = idx (ssp<=1024 -> no wrap)
    e_addlsl4(1, CPUREG, 0); e_addi(1, 1, (unsigned)OFF_SSTK);       // x1 = &sstk[2*idx]
    e_ldp(2, 3, 1, 0);                                              // x2 = guest_ret, x3 = host_ret
    e_ldr(1, CPUREG, 30 * 8);                                       // x1 = cpu->x[30] (guest return target)
    emit32(0xCB000000u | (1 << 16) | (2 << 5) | 1);                 // sub x1, x2, x1 (guest_ret - x30; no flags)
    uint32_t *p_cb1 = (uint32_t *)g_cp; emit32(0);                   // cbnz x1, Lfb (foreign/longjmp)
    e_addlsl3(1, CPUREG, 0); e_ldr(2, 1, (int)OFF_GSP);            // x2 = gsp[idx] (guest SP captured at the bl)
    e_mov_from_sp(1);                                              // x1 = current guest SP
    emit32(0xCB000000u | (2 << 16) | (1 << 5) | 1);                // sub x1, x1, x2 (sp - gsp; no flags)
    uint32_t *p_cb2 = (uint32_t *)g_cp; emit32(0);                 // cbnz x1, Lfb (guest_ret matched but wrong frame -> slow)
    e_str(0, CPUREG, (int)OFF_SSP);                                 // FAST: ssp-- (pop)
    e_movr(30, 3);                                                  // real x30 = host_ret
    e_ldp(0, 1, CPUREG, M); e_ldp(2, 3, CPUREG, M + 16);            // restore x0..x3 (paired)
    e_hret();                                                       // host ret -> &Lcont (hardware-RAS predicted)
    uint8_t *Lfb = g_cp;
    *p_cbz = 0xB4000000u | (((uint32_t)(((uint8_t *)Lfb - (uint8_t *)p_cbz) / 4) & 0x7FFFF) << 5) | 0;
    *p_cb1 = 0xB5000000u | (((uint32_t)(((uint8_t *)Lfb - (uint8_t *)p_cb1) / 4) & 0x7FFFF) << 5) | 1;  // cbnz x1
    *p_cb2 = 0xB5000000u | (((uint32_t)(((uint8_t *)Lfb - (uint8_t *)p_cb2) / 4) & 0x7FFFF) << 5) | 1;  // cbnz x1
    e_ldp(0, 1, CPUREG, M); e_ldp(2, 3, CPUREG, M + 16);            // restore x0..x3 (paired)
    emit_ibranch(30);                                              // UNWIND/FOREIGN -> IBTC (per-site IC + hash), NOT the dispatcher
}
// Fast correct ret on the stolen x30: per-site monomorphic cache on cpu->x[30] (a `br`, not a host
// ret -> no RAS, but no stale-host_ret corruption either). Mirrors the IBTC per-site IC; the
// dispatcher fills Lsite_tgt/Lsite_body via ic_site. Reads cpu->x[30] (x30 is stolen).
static void emit_ret_ic(void) {
    e_stur(16, 31, -16); e_stur(17, 31, -24);                      // stash scratch (target body_ind restores it)
    e_ldr(16, CPUREG, 30 * 8);                                     // x16 = cpu->x[30]
    uint32_t *p_ldrt = (uint32_t *)g_cp; emit32(0);                // ldr x17, Lsite_tgt
    emit32(0xCB000000u | (16 << 16) | (17 << 5) | 17);             // sub x17, x17, x16
    uint32_t *p_cbnz = (uint32_t *)g_cp; emit32(0);                // cbnz x17, Lmiss
    uint32_t *p_ldrb = (uint32_t *)g_cp; emit32(0);                // ldr x16, Lsite_body
    e_br(16);                                                      // HIT -> body_ind
    uint32_t *miss = (uint32_t *)g_cp;
    e_ldur(16, 31, -16); e_ldur(17, 31, -24);
    emit_spill();
    e_ldr(9, 0, 30 * 8); e_str(9, 0, OFF_PC);                      // cpu->pc = cpu->x[30]
    e_movconst(9, R_BRANCH); e_str(9, 0, OFF_RSN);
    uint32_t *p_adr = (uint32_t *)g_cp; emit32(0);                 // adr x9, Lsite_tgt -> dispatcher fills the site
    e_str(9, 0, OFF_ICSITE);
    e_movconst(9, (uint64_t)block_return); e_br(9);
    if ((uint64_t)g_cp & 7) emit32(0);
    uint8_t *Lt = g_cp; *(uint64_t *)g_cp = 0; g_cp += 8;
    uint8_t *Lb = g_cp; *(uint64_t *)g_cp = 0; g_cp += 8;
    *p_ldrt = 0x58000000u | (((uint32_t)((Lt - (uint8_t *)p_ldrt) / 4) & 0x7FFFF) << 5) | 17;
    *p_cbnz = 0xB5000000u | (((uint32_t)(((uint8_t *)miss - (uint8_t *)p_cbnz) / 4) & 0x7FFFF) << 5) | 17;
    *p_ldrb = 0x58000000u | (((uint32_t)((Lb - (uint8_t *)p_ldrb) / 4) & 0x7FFFF) << 5) | 16;
    int64_t ao = Lt - (uint8_t *)p_adr;
    *p_adr = 0x10000000u | ((uint32_t)(ao & 3) << 29) | (((uint32_t)((ao >> 2) & 0x7FFFF)) << 5) | 9;
}

// ---------------- the translator ----------------
// Translate the basic block at guest address gpc; returns host entry pointer.
static uint32_t recode_cond(uint32_t in, int64_t d) {   // re-target a cond branch to offset d (instrs)
    if ((in & 0x7E000000u) == 0x34000000u) return (in & 0xFF00001Fu) | ((uint32_t)(d & 0x7FFFF) << 5); // cbz/cbnz
    if ((in & 0xFF000010u) == 0x54000000u) return (in & 0xFF00000Fu) | ((uint32_t)(d & 0x7FFFF) << 5); // b.cond
    return (in & 0xFFF8001Fu) | ((uint32_t)(d & 0x3FFF) << 5);                                          // tbz/tbnz
}

// ---- LSE atomics idiom upgrade ----
// Distro binaries are built ARMv8.0-baseline, so every atomic is an ldxr/stxr retry
// loop. Apple Silicon has FEAT_LSE: recognize the loop and emit a single atomic op
// (2.29x faster, and it removes the load/store-exclusive monitor region that
// complicates the translator). AL ordering is always safe.
static void e_lse(uint32_t base, int sz, int rs, int rt, int rn) {
    emit32(base | (sz == 3 ? 0x40000000u : 0) | (rs << 16) | (rn << 5) | rt);
}
static void e_op_reg(uint32_t base, int sz, int rd, int rn, int rm) {  // add/orr/eor/subs (shifted, no shift)
    emit32(base | (sz == 3 ? 0x80000000u : 0) | (rm << 16) | (rn << 5) | rd);
}
static void e_cas(int sz, int rs, int rt, int rn) {  // casal Rs(compare/old), Rt(new), [Rn]
    emit32((sz == 3 ? 0xC8E0FC00u : 0x88E0FC00u) | (rs << 16) | (rn << 5) | rt);
}
// Returns bytes consumed (12 or 16) if a known atomic loop at gpc was rewritten, else 0.
static int try_lse_atomic(uint64_t gpc) {
    uint32_t i0 = *(uint32_t *)gpc;
    if ((i0 & 0x3F400000u) != 0x08400000u) return 0;                 // load-exclusive?
    int sz = (i0 >> 30) & 3;
    if (sz < 2) return 0;                                            // word/dword only
    if (((i0 >> 16) & 0x1F) != 0x1F || ((i0 >> 10) & 0x1F) != 0x1F) return 0;  // non-pair
    int Wt = i0 & 31, Xn = (i0 >> 5) & 31;
    uint32_t i1 = *(uint32_t *)(gpc + 4);

    // SWP:  ldxr Wt,[Xn]; stxr Ws,Wv,[Xn]; cbnz Ws,loop
    if ((i1 & 0x3F400000u) == 0x08000000u && ((i1 >> 30) & 3) == sz
        && ((i1 >> 10) & 0x1F) == 0x1F && ((i1 >> 5) & 31) == Xn) {
        int Ws = (i1 >> 16) & 31, Wv = i1 & 31;
        uint32_t i2 = *(uint32_t *)(gpc + 8);
        if ((i2 & 0xFF000000u) == 0x35000000u && (i2 & 31) == Ws
            && (gpc + 8 + (uint64_t)(sext((i2 >> 5) & 0x7FFFF, 19) << 2)) == gpc) {
            if (is_stolen(Wt) || is_stolen(Xn) || is_stolen(Wv)) return 0;
            e_lse(0xB8E08000u, sz, Wv, Wt, Xn); g_lse_n++;          // swpal Wv, Wt, [Xn]
            return 12;
        }
    }
    // LDADD/LDSET/LDEOR/LDCLR/LDADD-neg:  ldxr Wt,[Xn]; <op> Ws2,Wt,Wm; stxr Ws,Ws2,[Xn]; cbnz Ws,loop
    int op = -1;                                                    // 0 add 1 orr 2 eor 3 and 4 sub
    if ((i1 & 0xFFE0FC00u) == (sz == 3 ? 0x8B000000u : 0x0B000000u)) op = 0;
    else if ((i1 & 0xFFE0FC00u) == (sz == 3 ? 0xAA000000u : 0x2A000000u)) op = 1;
    else if ((i1 & 0xFFE0FC00u) == (sz == 3 ? 0xCA000000u : 0x4A000000u)) op = 2;
    else if ((i1 & 0xFFE0FC00u) == (sz == 3 ? 0x8A000000u : 0x0A000000u)) op = 3;
    else if ((i1 & 0xFFE0FC00u) == (sz == 3 ? 0xCB000000u : 0x4B000000u)) op = 4;
    if (op >= 0) {
        int Ws2 = i1 & 31, n = (i1 >> 5) & 31, m = (i1 >> 16) & 31, Wm = -1;
        if (op == 4) { if (n == Wt) Wm = m; }                      // sub: not commutative, Rn must be Wt
        else { if (n == Wt) Wm = m; else if (m == Wt) Wm = n; }
        uint32_t i2 = *(uint32_t *)(gpc + 8), i3 = *(uint32_t *)(gpc + 12);
        if (Wm >= 0 && (i2 & 0x3F400000u) == 0x08000000u && ((i2 >> 30) & 3) == sz
            && (i2 & 31) == Ws2 && ((i2 >> 5) & 31) == Xn && ((i2 >> 10) & 0x1F) == 0x1F) {
            int Ws = (i2 >> 16) & 31;
            if ((i3 & 0xFF000000u) == 0x35000000u && (i3 & 31) == Ws
                && (gpc + 12 + (uint64_t)(sext((i3 >> 5) & 0x7FFFF, 19) << 2)) == gpc) {
                if (is_stolen(Wt) || is_stolen(Xn) || is_stolen(Wm) || is_stolen(Ws2) || is_stolen(Ws)) return 0;
                if (op >= 3 && Wm == Ws) return 0;                  // need Ws as scratch, can't be Wm
                uint32_t rec = i1;                                  // reconstruct = the original op (re-emit)
                if (op <= 2) {
                    uint32_t lse = op == 0 ? 0xB8E00000u : op == 1 ? 0xB8E03000u : 0xB8E02000u;
                    e_lse(lse, sz, Wm, Wt, Xn);                     // ldaddal/ldsetal/ldeoral
                } else if (op == 3) {                              // fetch_and: *Xn &= Wm  ==  ldclr ~Wm
                    e_op_reg(0x2A200000u, sz, Ws, 31, Wm);         // mvn Ws, Wm  (orn Ws, wzr, Wm)
                    e_lse(0xB8E01000u, sz, Ws, Wt, Xn);            // ldclral Ws, Wt, [Xn]
                } else {                                           // fetch_sub: *Xn -= Wm  ==  ldadd -Wm
                    e_op_reg(0x4B000000u, sz, Ws, 31, Wm);         // neg Ws, Wm  (sub Ws, wzr, Wm)
                    e_lse(0xB8E00000u, sz, Ws, Wt, Xn);            // ldaddal Ws, Wt, [Xn]
                }
                emit32(rec); g_lse_n++;                            // reconstruct new value (re-emit original op)
                return 16;
            }
        }
    }
    // LDADD immediate (fetch_add of a constant -- the headline refcount/counter case):
    //   ldxr Wt,[Xn]; add Ws2,Wt,#imm (sh=0); stxr Ws,Ws2,[Xn]; cbnz Ws,loop
    uint32_t addib = sz == 3 ? 0x91000000u : 0x11000000u;
    if ((i1 & 0xFFC00000u) == addib && ((i1 >> 5) & 31) == Wt) {
        int Ws2 = i1 & 31; unsigned imm = (i1 >> 10) & 0xFFF;
        uint32_t i2 = *(uint32_t *)(gpc + 8), i3 = *(uint32_t *)(gpc + 12);
        if ((i2 & 0x3F400000u) == 0x08000000u && ((i2 >> 30) & 3) == sz
            && (i2 & 31) == Ws2 && ((i2 >> 5) & 31) == Xn && ((i2 >> 10) & 0x1F) == 0x1F) {
            int Ws = (i2 >> 16) & 31;
            if ((i3 & 0xFF000000u) == 0x35000000u && (i3 & 31) == Ws
                && (gpc + 12 + (uint64_t)(sext((i3 >> 5) & 0x7FFFF, 19) << 2)) == gpc) {
                if (is_stolen(Wt) || is_stolen(Xn) || is_stolen(Ws2) || is_stolen(Ws)) return 0;
                e_movz(Ws, imm, 0);                          // Ws (dead status reg) = imm
                e_lse(0xB8E00000u, sz, Ws, Wt, Xn);          // ldaddal Ws, Wt, [Xn]
                emit32(i1); g_lse_n++;                       // re-emit add Ws2, Wt, #imm (reconstruct)
                return 16;
            }
        }
    }
    // CAS:  ldxr Wt,[Xn]; cmp Wt,Wexp; b.ne out; stxr Ws,Wnew,[Xn]; cbnz Ws,loop; out:
    uint32_t subsb = sz == 3 ? 0xEB00001Fu : 0x6B00001Fu;            // subs wzr, Wt, Wexp (cmp)
    if ((i1 & 0xFFE0FC1Fu) == subsb && ((i1 >> 5) & 31) == Wt) {
        int Wexp = (i1 >> 16) & 31;
        uint32_t i2 = *(uint32_t *)(gpc + 8), i3 = *(uint32_t *)(gpc + 12), i4 = *(uint32_t *)(gpc + 16);
        if ((i2 & 0xFF00001Fu) == 0x54000001u                       // b.ne
            && (i3 & 0x3F400000u) == 0x08000000u && ((i3 >> 30) & 3) == sz
            && ((i3 >> 10) & 0x1F) == 0x1F && ((i3 >> 5) & 31) == Xn
            && (i4 & 0xFF000000u) == 0x35000000u && (i4 & 31) == ((i3 >> 16) & 31)
            && (gpc + 16 + (uint64_t)(sext((i4 >> 5) & 0x7FFFF, 19) << 2)) == gpc        // cbnz -> loop
            && (gpc + 8 + (uint64_t)(sext((i2 >> 5) & 0x7FFFF, 19) << 2)) == gpc + 20) { // b.ne -> out
            int Wnew = i3 & 31;
            if (is_stolen(Wt) || is_stolen(Xn) || is_stolen(Wexp) || is_stolen(Wnew) || Wt == Wexp) return 0;
            e_op_reg(0x2A000000u, sz, Wt, 31, Wexp);     // mov Wt, Wexp (orr Wt, wzr, Wexp)
            e_cas(sz, Wt, Wnew, Xn);                      // casal Wt, Wnew, [Xn]; Wt = old
            e_op_reg(0x6B000000u, sz, 31, Wt, Wexp); g_lse_n++; // cmp Wt, Wexp (reproduce NZCV)
            return 20;
        }
    }
    return 0;
}

// ---- tier-2 substrate: the purity gate (the analyze() of trace_pipeline.c) ----
// Given a formed trace's instructions, return 1 only if it is safe to MEMOIZE:
// no syscall (svc) and no memory access at all -- so the result is fully determined
// by the input registers and there are no side effects. Conservative by construction:
// any load/store or syscall -> impure -> emit unoptimized (side effects must run).
// This is the gate that refuses the impure region in the pipeline (a wrong gate here
// is a miscompile). Linear in trace length, run once on promotion. Verified by
// TIER2_SELFTEST; wired into specialization when trace formation (the "form trace"
// step) lands -- the remaining substrate brick.
static int region_pure(const uint32_t *code, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t in = code[i];
        if (in == 0xD4000001u) return 0;                  // svc -> side effect
        if ((in & 0x0A000000u) == 0x08000000u) return 0;  // any load/store -> not register-determined
    }
    return 1;                                             // pure: register-to-register computation only
}

// ---- §B shadow-stack return prediction: the validated mechanism (PoC: shadow_stack.c) ----
// At a guest `bl`, record the guest return address. At a guest `ret`, classify the guest's x30:
//   FAST    -> matches the top of the shadow stack: the normal return; take a host `ret` (the
//              hardware RAS predicts it in ~1 insn instead of the ~14-insn ret-IBTC).
//   UNWIND  -> matches a deeper frame (longjmp / multi-frame return): pop to it, still correct.
//   FOREIGN -> not on the shadow (computed/tail return): fall back to the IBTC.
// Conservative: ONLY the FAST path takes the host ret; UNWIND/FOREIGN fall back, so a return can
// never land at the wrong target. The codegen that emits host bl/ret + the x30 steal wires onto
// this (the one subtlety past the PoC is x30's dual role: host return address vs guest-visible
// link value -- handled by keeping guest x30 in cpu->x[30] and validating here).
enum { SS_FAST, SS_UNWIND, SS_FOREIGN };
static inline void shadow_push(struct cpu *c, uint64_t guest_ret, uint64_t host_ret) {
    if (c->ssp < 1024) { c->sstk[2 * c->ssp] = guest_ret; c->sstk[2 * c->ssp + 1] = host_ret; c->ssp++; }
}
static int shadow_classify(struct cpu *c, uint64_t guest_x30) {   // matches on guest_ret (even index)
    if (c->ssp > 0 && c->sstk[2 * (c->ssp - 1)] == guest_x30) { c->ssp--; return SS_FAST; }
    for (uint64_t d = 2; d <= c->ssp && d <= 64; d++)
        if (c->sstk[2 * (c->ssp - d)] == guest_x30) { c->ssp -= d; return SS_UNWIND; }
    return SS_FOREIGN;
}

static void *translate_block(uint64_t gpc) {
    uint64_t start = gpc;   // gpc is mutated by the decode loop; key the cache by START
    void *host = g_cp;
    emit_prologue();
    void *body = g_cp;   // chained jumps land here (regs already live)
    // ldxr/ldaxr..stxr/stlxr exclusive regions must stay in ONE block with no injected
    // memory ops between them, else the monitor clears and stxr retries forever. While
    // inside such a region, conditional branches are emitted inline and their exits are
    // deferred to stubs after the store-exclusive.
    int in_excl = 0;
    struct { uint32_t *patch; uint64_t target; uint32_t in; } defer[64]; int ndefer = 0;
    for (;;) {
        uint32_t in = *(uint32_t *)gpc;

        if (!in_excl && !getenv("NOLSE")) { int n = try_lse_atomic(gpc); if (n) { gpc += n; continue; } }  // ldxr/stxr loop -> LSE
        if ((in & 0x3F400000u) == 0x08400000u) in_excl = 1;                  // load-exclusive
        else if (in_excl && (in & 0x3F400000u) == 0x08000000u) in_excl = 0;  // store-exclusive

        if (in == 0xD4000001u) {                 // svc #0
            emit_exit_const(gpc, R_SYSCALL); break;
        }
        if ((in & 0xFC000000u) == 0x14000000u) { // b
            int64_t off = sext(in & 0x3FFFFFF, 26) << 2;
            emit_chain_exit(gpc + off); break;
        }
        if ((in & 0xFC000000u) == 0x94000000u) { // bl
            int64_t off = sext(in & 0x3FFFFFF, 26) << 2;
            emit_bl_ras(gpc, gpc + off); break;  // §B: shadow push + host bl (RAS) + Lcont continuation
        }
        if ((in & 0xFFFFFC1Fu) == 0xD65F0000u) { // ret xN
            int rrn = (in >> 5) & 31;
            if (rrn == 30) emit_shadow_ret();    // §B: FAST shadow ret (guest_ret+guest_sp match -> host ret) / IBTC fallback
            else emit_ibranch(rrn);              // ret xN via another reg -> ordinary indirect branch
            break;
        }
        if ((in & 0xFFFFFC1Fu) == 0xD61F0000u) { // br
            emit_ibranch((in >> 5) & 31); break;
        }
        if ((in & 0xFFFFFC1Fu) == 0xD63F0000u) { // blr
            emit_set_x30(gpc + 4);               // guest x30 lives in cpu->x[30] (stolen); RAS push needs a host blr
            emit_ibranch((in >> 5) & 31); break; //   (Section 3) -- deferred; Stage-B IBTC for the function-ptr return
        }
        if ((in & 0xFF000010u) == 0x54000000u) { // b.cond
            int cond = in & 0xF;
            int64_t off = sext((in >> 5) & 0x7FFFF, 19) << 2;
            uint64_t taken = gpc + off, fall = gpc + 4;
            if (in_excl) { defer[ndefer].patch = (uint32_t *)g_cp; defer[ndefer].target = taken; defer[ndefer].in = in; ndefer++; emit32(0); gpc += 4; continue; }
            uint32_t *patch = (uint32_t *)g_cp; emit32(0);   // b.cond -> taken (backpatched)
            emit_chain_exit(fall);
            int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
            *patch = 0x54000000u | ((uint32_t)(d & 0x7FFFF) << 5) | cond;
            emit_chain_exit(taken); break;
        }
        if ((in & 0x7E000000u) == 0x34000000u) { // cbz / cbnz
            int64_t off = sext((in >> 5) & 0x7FFFF, 19) << 2;
            uint64_t taken = gpc + off, fall = gpc + 4;
            int sf = in >> 31, op = (in >> 24) & 1, rt = in & 31;
            if (in_excl) { defer[ndefer].patch = (uint32_t *)g_cp; defer[ndefer].target = taken; defer[ndefer].in = in; ndefer++; emit32(0); gpc += 4; continue; }
            if (is_stolen(rt)) {                             // tested reg stolen -> test cpu->x[rt] via a saved scratch
                int S = 0;
                e_str(S, CPUREG, (int)OFF_MSCRATCH); e_ldr(S, CPUREG, rt * 8);
                uint32_t *patch = (uint32_t *)g_cp; emit32(0);                 // cbz/cbnz S -> taken
                e_ldr(S, CPUREG, (int)OFF_MSCRATCH); emit_chain_exit(fall);    // fall: restore S
                int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
                *patch = 0x34000000u | ((unsigned)sf << 31) | ((unsigned)op << 24) | ((uint32_t)(d & 0x7FFFF) << 5) | S;
                e_ldr(S, CPUREG, (int)OFF_MSCRATCH); emit_chain_exit(taken); break;  // taken: restore S
            }
            uint32_t *patch = (uint32_t *)g_cp; emit32(0);   // cbz/cbnz rt -> taken
            emit_chain_exit(fall);
            int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
            *patch = 0x34000000u | ((unsigned)sf << 31) | ((unsigned)op << 24)
                   | ((uint32_t)(d & 0x7FFFF) << 5) | rt;
            emit_chain_exit(taken); break;
        }
        if ((in & 0x7E000000u) == 0x36000000u) { // tbz / tbnz
            int b40 = (in >> 19) & 0x1F, bit5 = (in >> 31) & 1;
            int64_t off = sext((in >> 5) & 0x3FFF, 14) << 2;
            uint64_t taken = gpc + off, fall = gpc + 4;
            int op = (in >> 24) & 1, rt = in & 31;
            if (in_excl) { defer[ndefer].patch = (uint32_t *)g_cp; defer[ndefer].target = taken; defer[ndefer].in = in; ndefer++; emit32(0); gpc += 4; continue; }
            if (is_stolen(rt)) {                             // tested reg stolen -> test cpu->x[rt] via a saved scratch
                int S = 0;
                e_str(S, CPUREG, (int)OFF_MSCRATCH); e_ldr(S, CPUREG, rt * 8);
                uint32_t *patch = (uint32_t *)g_cp; emit32(0);                 // tbz/tbnz S,#bit -> taken
                e_ldr(S, CPUREG, (int)OFF_MSCRATCH); emit_chain_exit(fall);    // fall: restore S
                int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
                *patch = 0x36000000u | ((unsigned)bit5 << 31) | ((unsigned)op << 24) | ((unsigned)b40 << 19) | ((uint32_t)(d & 0x3FFF) << 5) | S;
                e_ldr(S, CPUREG, (int)OFF_MSCRATCH); emit_chain_exit(taken); break;  // taken: restore S
            }
            uint32_t *patch = (uint32_t *)g_cp; emit32(0);   // tbz/tbnz rt,#bit -> taken
            emit_chain_exit(fall);
            int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
            *patch = 0x36000000u | ((unsigned)bit5 << 31) | ((unsigned)op << 24)
                   | ((unsigned)b40 << 19) | ((uint32_t)(d & 0x3FFF) << 5) | rt;
            emit_chain_exit(taken); break;
        }

        // --- TLS: the whole point. mrs/msr tpidr_el0 become a single NATIVE
        //     load/store from cpu->tls. No trap, no Mach round-trip. ---
        if ((in & 0xFFFFFFE0u) == 0xD53BD040u) {   // mrs xN, tpidr_el0  (TLS read, hot)
            int n = in & 31;
            if (is_stolen(n)) { x18_prolog(); e_ldr(0, 1, OFF_TLS); e_str(0, 1, n * 8); x18_epilog(); }
            else { e_load_cpu(n); e_ldr(n, n, OFF_TLS); }
            gpc += 4; continue;
        }
        if ((in & 0xFFFFFFE0u) == 0xD51BD040u) {   // msr tpidr_el0, xN  (TLS write, rare)
            int n = in & 31, t = (n == 16) ? 15 : 16;
            if (is_stolen(n)) { x18_prolog(); e_ldr(0, 1, n * 8); e_str(0, 1, OFF_TLS); x18_epilog(); }
            else { e_stur(t, 31, -16); e_load_cpu(t); e_str(n, t, OFF_TLS); e_ldur(t, 31, -16); }
            gpc += 4; continue;
        }

        // --- non-branch, PC-relative: rewrite to materialize the (relocated) addr ---
        if ((in & 0x9F000000u) == 0x10000000u) { // adr
            int rd = in & 31;
            int64_t imm = sext((((in >> 5) & 0x7FFFF) << 2) | ((in >> 29) & 3), 21);
            if (is_stolen(rd)) { x18_prolog(); e_movconst(0, gpc + imm); e_str(0, 1, rd * 8); x18_epilog(); }
            else e_movconst(rd, gpc + imm);
            gpc += 4; continue;
        }
        if ((in & 0x9F000000u) == 0x90000000u) { // adrp
            int rd = in & 31;
            int64_t imm = sext((((in >> 5) & 0x7FFFF) << 2) | ((in >> 29) & 3), 21) << 12;
            uint64_t v = (gpc & ~0xFFFull) + imm;
            if (is_stolen(rd)) { x18_prolog(); e_movconst(0, v); e_str(0, 1, rd * 8); x18_epilog(); }
            else e_movconst(rd, v);
            gpc += 4; continue;
        }
        if ((in & 0xBF000000u) == 0x18000000u) { // ldr (literal) 32/64
            int rt = in & 31, is64 = (in >> 30) & 1;
            int64_t off = sext((in >> 5) & 0x7FFFF, 19) << 2;
            if (is_stolen(rt)) { x18_prolog(); e_movconst(0, gpc + off);
                            if (is64) e_ldr(0, 0, 0); else emit32(0xB9400000u | (0 << 5) | 0);
                            e_str(0, 1, rt * 8); x18_epilog(); }
            else { e_movconst(rt, gpc + off); if (is64) e_ldr(rt, rt, 0); else emit32(0xB9400000u | (rt << 5) | rt); }
            gpc += 4; continue;
        }

        int mask = gpr_field_mask(in);                          // everything else: verbatim,
        if (uses_x18(in, mask)) emit_mangled_x18(in, mask);     // unless it names x18 -> mangle
        else emit32(in);
        gpc += 4;
    }
    // emit the deferred exit stubs for branches taken inside an exclusive region
    for (int i = 0; i < ndefer; i++) {
        int64_t d = ((uint8_t *)g_cp - (uint8_t *)defer[i].patch) / 4;
        *defer[i].patch = recode_cond(defer[i].in, d);
        emit_chain_exit(defer[i].target);
    }
    map_put(start, host, body);
    // patch_links_to is MOVED to the dispatcher, AFTER the new block's icache is invalidated:
    // chaining an existing block X -> this new block before its code is icache-coherent on a peer
    // core lets that core fetch stale instructions. Only chain to it once it's visible everywhere.
    return host;
}

