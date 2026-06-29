// dd/runtime/frontend/x86_64 -- the x86-64 -> arm64 translator (flag synthesis, SSE/x87 lowering, the
// big translate_block) + host entry trampolines.

// ---------------- the translator ----------------
static void report_unimpl(uint64_t pc, struct insn *I);

// MUL/IMUL (group3 F6/F7 /4,/5) set x86 CF=OF when the high half of the product is significant
// (MUL: high half != 0; IMUL: high half != sign-extension of the low half); SF/ZF/AF/PF are
// x86-undefined. cfreg holds the computed CF/OF as 0/1. Write the stored NZCV using the engine's
// borrow convention (stored C = NOT x86 CF at bit 29, OF = V at bit 28) with N=Z=0; scratch x20/x23.
static void e_mul_set_oc(int cfreg) {
    e_movconst(23, 1);
    e_rrr(A_EOR, 23, cfreg, 23, 0, 0);  // x23 = NOT cf (cf is 0/1)
    e_movconst(20, 0);
    e_rrr(A_ORR, 20, 20, 23, 1, 29);    // stored C (bit 29) = NOT x86 CF
    e_rrr(A_ORR, 20, 20, cfreg, 1, 28); // V (bit 28) = OF = cf
    e_str(20, 28, OFF_NZCV);
    emit32(0xD51B4200u | 20); // msr nzcv, x20 (sync live flags)
}

// imul reg<-a*b (two-/three-operand forms 0F AF, 69, 6B): truncated product into dst, and x86
// CF=OF = (the full signed product differs from the sign-extension of the truncated result).
// Scratch x21..x25 (x21 carries the 0/1 CF into e_mul_set_oc); callers must not pass a/b in those.
static void e_imul2(int dst, int a, int b, int w) {
    if (w == 8) {
        e_smulh(24, a, b);               // x24 = signed high 64 bits of the product
        e_mul(dst, a, b, 1);             // dst = low 64 (a,b already consumed by smulh)
        e_asr_i(25, dst, 63, 1);         // x25 = sign-extension of the low half
        e_rrr(A_SUBS, 22, 24, 25, 1, 0); // overflow iff high != sign(low)
        e_cset(21, 1 /*NE*/, 1);
    } else { // 32- or 16-bit: full signed product, overflow iff it != sxt of the truncated result
        e_sxt(24, a, w);
        e_sxt(25, b, w);
        e_mul(22, 24, 25, 1); // x22 = full signed product (operands fit in 32, so 64 is exact)
        e_sxt(23, 22, w);     // x23 = sign-extension of the low w bytes
        e_rrr(A_SUBS, 25, 22, 23, 1, 0);
        e_cset(21, 1 /*NE*/, 1);
        if (w == 4)
            e_mov_rr(dst, 22, 0);      // 32-bit dest: low 32, zero-extended
        else
            e_bfi(dst, 22, 0, 16, 1);  // 16-bit dest: insert low 16, preserve upper bits
    }
    e_mul_set_oc(21);
}

// ALU operation selector from the primary opcode group (00..3D) or group1 /digit.
// returns: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP, or -1.
static int alu_kind_primary(uint8_t op) {
    int k = (op >> 3) & 7;
    return ((op & 7) <= 5) ? k : -1;
}

// 32/64-bit core ALU into `out`, rn<op>rm, setting ARM flags. out=31 -> discard (cmp/test).
static void alu_core(int kind, int out, int rn, int rm, int sf) {
    switch (kind) {
    case 0: e_rrr(A_ADDS, out, rn, rm, sf, 0); break; // add
    case 4: e_rrr(A_ANDS, out, rn, rm, sf, 0); break; // and / test
    case 5: e_rrr(A_SUBS, out, rn, rm, sf, 0); break; // sub / cmp
    case 1:
        e_rrr(A_ORR, out, rn, rm, sf, 0); // or
        emit32((sf ? 0xEA00001Fu : 0x6A00001Fu) | (out << 16) | (out << 5));
        break; // tst
    case 6:
        e_rrr(A_EOR, out, rn, rm, sf, 0); // xor
        emit32((sf ? 0xEA00001Fu : 0x6A00001Fu) | (out << 16) | (out << 5));
        break;
    default: break;
    }
}
// Byte-register operands: without REX, encodings 4..7 are the HIGH bytes ah/ch/dh/bh
// (bits[15:8] of the first 4 regs); with any REX they're the low bytes spl/bpl/sil/dil.
static int is_hi8(struct insn *I, int regnum) { return !I->has_rex && regnum >= 4 && regnum < 8; }
// value of an 8-bit register operand, in the LOW 8 bits of the returned reg (rest is
// don't-care -- do_alu's <<24 trick keeps only the low byte). hi8 -> extract via >>8.
static int byte_val(struct insn *I, int regnum, int scratch) {
    if (is_hi8(I, regnum)) {
        e_lsr_i(scratch, regnum - 4, 8, 1);
        return scratch;
    }
    return regnum;
}
// write the low byte of `val` into an 8-bit register operand (preserving other bits).
static void byte_wb(struct insn *I, int regnum, int val) {
    if (is_hi8(I, regnum))
        e_bfi(regnum - 4, val, 8, 8, 1);
    else
        e_bfi(regnum, val, 0, 8, 1);
}
// W6A item 1 (non-PIE): the link range + bias of a biased ET_EXEC image (defined in os/linux/container/vfs.c,
// set in elf.c's load_elf -- both later in the unity TU), plus the Go type section [md.types, md.etypes) in
// low link coords (set by elf.c's go_rebase_nonpie; the range whose lea-materialized *_type pointers are made
// low so they match the image's baked-absolute type pointers and Go's type identity holds). Forward tentative
// declarations so the rip-relative `lea` rewrite below can see them. All zero for PIE/static-PIE / non-Go
// images -> the rewrite is inert.
static uint64_t g_nonpie_lo, g_nonpie_hi, g_nonpie_bias, g_nonpie_types_lo, g_nonpie_types_hi;

// r/m operand: mem -> EA to x17, load value to x16 (returns 16); reg -> value reg.
static void emit_ea(struct insn *I, uint64_t next_rip);
static int rm_load(struct insn *I, uint64_t next, int w, int *mem) {
    if (I->is_mem) {
        emit_ea(I, next);
        e_load(w, 16, 17);
        *mem = 1;
        return 16;
    }
    *mem = 0;
    if (w == 1) return byte_val(I, I->rm_reg, 23); // handle ah/ch/dh/bh
    return I->rm_reg;
}
static void rm_store(struct insn *I, int w, int val) { // val -> r/m (EA already in x17 if mem)
    if (I->is_mem) {
        e_store(w, val, 17);
        return;
    }
    if (w == 1) {
        byte_wb(I, I->rm_reg, val);
        return;
    }
    if (val != I->rm_reg) {
        if (w >= 4)
            e_mov_rr(I->rm_reg, val, w == 8);
        else
            e_bfi(I->rm_reg, val, 0, 8 * w, 1);
    }
}
// RCL/RCR (group2 /2,/3): rotate the r/m operand THROUGH the x86 carry flag by a CONSTANT count -- the
// by-1 (D0/D1) and immediate (C0/C1) forms; the by-CL form is left to defer (report_unimpl). The operand
// and CF together form a (W+1)-bit value rotated by `ec`; only CF and -- for a 1-bit rotate -- OF are
// affected, with SF/ZF/PF preserved. Carry-in is taken from cpu->nzcv (stored ARM C = NOT x86 CF; the
// lazy-flag pre-pass has already materialized any pending producer), the result and the new CF/OF are
// emitted with compile-time-constant shifts, and CF/OF are written back to cpu->nzcv. Scratch x19..x24.
static void emit_rcl_rcr(struct insn *I, uint64_t next, int w, int rcr, int cnt_raw) {
    int ssf = (w >= 4) ? (w == 8) : 1; // operate 64-bit for byte/word (operand is zero-extended)
    int W = 8 * w, bw = ssf ? 64 : 32;
    int ec = (w < 4) ? (cnt_raw % (W + 1)) : cnt_raw; // effective rotate through the (W+1)-bit value
    int count1 = (cnt_raw == 1);                       // OF defined only for a single-bit rotate
    int mem;
    int raw = rm_load(I, next, w, &mem);
    if (ec == 0) {                       // a 0-count rotate is a no-op and affects no flags
        if (mem) e_store(w, raw, 17);
        return;
    }
    if (w < 4)
        e_uxt(19, raw, w); // x19 = zero-extended operand
    else
        e_mov_rr(19, raw, ssf);
    // x24 = carry-in (x86 CF) = NOT(stored ARM C, nzcv bit 29)
    e_ldr(20, 28, OFF_NZCV);
    e_lsr_i(20, 20, 29, 1);
    e_movconst(23, 1);
    e_rrr(A_AND, 20, 20, 23, 0, 0);
    e_rrr(A_EOR, 24, 20, 23, 0, 0);
    // x21 = new x86 CF: RCR -> bit (ec-1) of operand, RCL -> bit (W-ec) of operand
    e_lsr_i(21, 19, rcr ? ec - 1 : W - ec, ssf);
    e_rrr(A_AND, 21, 21, 23, 0, 0);
    // x16 = result (low W bits valid). Terms emitted only when non-trivial -> no out-of-range shifts.
    if (rcr) {
        if (ec < W)
            e_lsr_i(16, 19, ec, ssf); // operand bits that fall straight down
        else
            e_movconst(16, 0);
        if (W - ec == 0) // carry-in lands at result bit (W-ec)
            e_rrr(A_ORR, 16, 16, 24, ssf, 0);
        else {
            e_lsl_i(20, 24, W - ec, ssf);
            e_rrr(A_ORR, 16, 16, 20, ssf, 0);
        }
        if (ec >= 2) { // operand bits below carry wrap to the top: (operand & ((1<<(ec-1))-1)) << (W-ec+1)
            e_lsl_i(20, 19, bw - (ec - 1), ssf);
            e_lsr_i(20, 20, bw - (ec - 1), ssf);
            e_lsl_i(20, 20, W - ec + 1, ssf);
            e_rrr(A_ORR, 16, 16, 20, ssf, 0);
        }
    } else { // RCL
        if (ec < W)
            e_lsl_i(16, 19, ec, ssf);
        else
            e_movconst(16, 0);
        if (ec - 1 == 0) // carry-in lands at result bit (ec-1)
            e_rrr(A_ORR, 16, 16, 24, ssf, 0);
        else {
            e_lsl_i(20, 24, ec - 1, ssf);
            e_rrr(A_ORR, 16, 16, 20, ssf, 0);
        }
        if (ec >= 2) { // top operand bits wrap to the bottom: (operand >> (W+1-ec)) keeping low (ec-1) bits
            e_lsr_i(20, 19, W + 1 - ec, ssf);
            e_lsl_i(20, 20, bw - (ec - 1), ssf);
            e_lsr_i(20, 20, bw - (ec - 1), ssf);
            e_rrr(A_ORR, 16, 16, 20, ssf, 0);
        }
    }
    // OF (single-bit rotate only): RCL -> newCF ^ result_MSB ; RCR -> result top two bits XORed.
    if (count1) {
        e_lsr_i(20, 16, W - 1, ssf); // x20 = result MSB
        if (rcr) {
            e_lsr_i(19, 16, W - 2, ssf);
            e_rrr(A_EOR, 22, 20, 19, ssf, 0);
        } else
            e_rrr(A_EOR, 22, 21, 20, ssf, 0);
        e_rrr(A_AND, 22, 22, 23, 0, 0); // x22 = OF (0/1)  (x23 still == 1)
    }
    rm_store(I, w, 16);
    // Write back CF (stored C = NOT newCF) and, for a 1-bit rotate, OF (V); preserve N/Z (and V otherwise).
    e_ldr(20, 28, OFF_NZCV);
    e_movconst(19, 1u << 29);
    e_rrr(A_BIC, 20, 20, 19, 1, 0);  // clear stored C
    e_rrr(A_EOR, 19, 21, 23, 0, 0);  // x19 = NOT newCF
    e_rrr(A_ORR, 20, 20, 19, 1, 29); // stored C = (NOT newCF) << 29
    if (count1) {
        e_movconst(19, 1u << 28);
        e_rrr(A_BIC, 20, 20, 19, 1, 0);  // clear V
        e_rrr(A_ORR, 20, 20, 22, 1, 28); // V = OF
    }
    e_str(20, 28, OFF_NZCV);
    emit32(0xD51B4200u | 20); // sync live ARM nzcv
}

// Lazy flags (x86-perf PR1 + opt3): pending-finalizer record. Translate-time only -- no guest
// state, never exists at runtime. A width-4/8 do_alu producer *defers* its NZCV materialization:
// the LIVE ARM NZCV currently holds that op's result flags, and g_fl_pending names which finalizer
// would spill them to cpu->nzcv in the canonical borrow convention (== exactly the bytes the inline
// finalizer would have emitted, and what x86cc_to_arm() assumes). Consumed live only by an
// *immediately following* Jcc; any other instruction or block boundary materializes it to membank
// and clears it -- so the cross-block cpu->nzcv ABI is byte-identical. Reset per block.
//   FL_SUB   -> e_nzcv_save     (sub/cmp: ARM SUBS already canonical; PR1 baseline path)
//   FL_ADD   -> e_nzcv_save_ci  (x86 add: invert ARM add-carry)
//   FL_LOGIC -> e_nzcv_save_c1  (and/or/xor/test: x86 CF=0,OF=0)
enum { FL_NONE, FL_SUB, FL_ADD, FL_LOGIC };
static int g_fl_pending;

// x86 direction flag (DF), tracked at translate time. Compilers/libc emit `std`/`cld` straight-line
// around the string op they govern (e.g. runtime.memmove's backward `std; rep movsq; cld`), so the
// flag is always block-local -- no runtime cpu state needed. Reset to 0 (forward) at each block entry;
// `std` sets it, `cld` clears it, and the string-op lowering picks forward/backward stride accordingly.
static int g_df;

// opt3 kill-switch: NOLAZY=1 (any non-"0") reverts to the PR1 partial scheme (only sub/cmp defers;
// add/logical materialize inline; no dead-flag elimination). Read once, cached.
static int lazyflags_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *s = getenv("NOLAZY");
        v = (s && *s && *s != '0') ? 0 : 1;
    }
    return v;
}

// Spill the deferred flags to cpu->nzcv with the producer-correct finalizer (byte-identical to the
// old inline finalizer) and clear the pending state. Every finalizer also msr's the corrected value
// back, so the live ARM NZCV is left canonical for an immediately-following Jcc to branch off.
static void flags_materialize(void) {
    switch (g_fl_pending) {
    case FL_SUB: e_nzcv_save(); break;
    case FL_ADD: e_nzcv_save_ci(); break;
    case FL_LOGIC: e_nzcv_save_c1(); break;
    default: break;
    }
    g_fl_pending = FL_NONE;
}

// PUSHFQ/POPFQ flag shuffling: OR the single bit at position `sp` of x[src] into x[dst] at
// position `dp`, via a scratch reg `tmp`. (ubfx wtmp,wsrc,#sp,#1 ; orr xdst,xdst,xtmp,lsl #dp)
static void e_bit_move(int dst, int src, int sp, int dp, int tmp) {
    emit32(0x53000000u | (sp << 16) | (sp << 10) | (src << 5) | tmp); // ubfx wtmp,wsrc,#sp,#1
    e_rrr(A_ORR, dst, dst, tmp, 0, dp);                               // orr xdst,xdst,xtmp,lsl #dp
}

// opt3 dead-flag elimination: 1 iff I's handler provably writes the FULL NZCV while reading no
// flags -- so a pending producer's flags are dead (overwritten before any read) and need not be
// materialized at all. Conservative whitelist: add/or/and/sub/xor/cmp/test/neg only. EXCLUDES
// adc/sbb (read CF), inc/dec (preserve CF), shifts, mul/div, not (flags untouched) -> default 0.
static int insn_is_flagkill(const struct insn *I) {
    if (I->two) return 0;
    uint8_t op = I->op;
    // primary ALU 00..3D (reg/rm + AL/imm forms): kinds add/or/and/sub/xor/cmp, not adc(2)/sbb(3)
    if (op < 0x40 && alu_kind_primary(op) >= 0) {
        int k = alu_kind_primary(op);
        return (k != 2 && k != 3);
    }
    // group1 (80/81/83): ALU r/m, imm
    if (op == 0x80 || op == 0x81 || op == 0x83) {
        int k = I->reg & 7;
        return (k != 2 && k != 3);
    }
    if (op == 0x84 || op == 0x85 || op == 0xA8 || op == 0xA9) return 1; // test
    if (op == 0xF6 || op == 0xF7) {                                     // group3
        int k = I->reg & 7;
        return (k == 0 || k == 3); // /0 test, /3 neg (full NZCV overwrite, read nothing)
    }
    return 0;
}

// opt3 carry-value consumer (adc/sbb): 1 iff I reaches do_alu kind 2/3 with width>=4 -- the forms that
// can pull their x86 CF carry-in straight from an immediately-preceding deferred producer's LIVE NZCV
// (so the main loop must NOT eagerly materialize the pending flags before it; do_alu consumes them).
// Byte/word adc/sbb (report_unimpl) and every non-adc/sbb op return 0 -> normal materialize path.
static int insn_is_carry_consumer(const struct insn *I) {
    if (I->two) return 0;
    uint8_t op = I->op;
    // primary reg/rm forms 10/11/12/13 (adc) 18/19/1A/1B (sbb): width>=4 needs (op&1) && opsize>=4
    if (op < 0x40 && (op & 7) <= 3 && alu_kind_primary(op) >= 0) {
        int k = alu_kind_primary(op);
        return (k == 2 || k == 3) && (op & 1) && I->opsize >= 4;
    }
    // imm-to-acc 15 (adc eax,imm) 1D (sbb eax,imm): (op&7)==5 is the word/dword form
    if (op < 0x40 && (op & 7) == 5 && alu_kind_primary(op) >= 0) {
        int k = alu_kind_primary(op);
        return (k == 2 || k == 3) && I->opsize >= 4;
    }
    // group1 81/83 (/2 adc, /3 sbb); 80 is byte-only -> not a carry consumer here
    if (op == 0x81 || op == 0x83) {
        int k = I->reg & 7;
        return (k == 2 || k == 3) && I->opsize >= 4;
    }
    return 0;
}

// opt3 carry-flow: adjust ONLY the C bit of the LIVE ARM NZCV in place (no cpu->nzcv round-trip), so an
// adc/sbb can read its x86 CF carry-in directly from a deferred producer's live flags. `alu_base` selects
// the bit op on bit 29 (C): A_EOR flips it, A_BIC clears it, A_ORR sets it. Scratch x20/x22 match the
// e_nzcv_* convention (callee-saved, never an x86 guest reg x0..x15 nor a do_alu operand reg).
static void e_nzcv_C_op(uint32_t alu_base) {
    emit32(0xD53B4200u | 20);          // mrs x20, nzcv
    e_movconst(22, 1u << 29);          // C is bit 29 of nzcv
    e_rrr(alu_base, 20, 20, 22, 1, 0); // x20 = x20 <op> (1<<29)   (EOR=flip / BIC=clear / ORR=set)
    emit32(0xD51B4200u | 20);          // msr nzcv, x20
}

// Stash the x86 PF source: the low byte of an integer op's result (the consumer computes even-parity).
// A non-flag str -> leaves the live ARM NZCV untouched (safe to interleave with the lazy-flag path).
static void e_pf_save(int reg) { e_str(reg, 28, OFF_PF); }
// Width-correct ALU: dst = a <kind> b, set cpu->nzcv.  dst<0 => cmp/test (no write).
// 4/8-byte: direct ARM op. 1/2-byte: operate in the HIGH bits (<<sh) so ARM NZCV matches
// x86 byte/word flags exactly, then merge the low w bytes back (preserving upper bits).
static void do_alu(int kind, int dst, int a, int b, int w) {
    int sf = w == 8, out = dst < 0 ? 31 : dst;
    int ak = kind == 7 ? 5 : kind; // cmp == sub(discard); test == and(discard)
    if (kind == 7) ak = 5;
    if (kind == 2 || kind == 3) { // ADC / SBB -- carry-VALUE consumers (opt3 lazy carry-flow)
        // ARM ADCS computes a+b+C, SBCS computes a-b-(NOT C). x86 ADC/SBB use x86 CF directly.
        // Borrow convention: cpu->nzcv stores ARM C = NOT x86 CF. Hence the required LIVE ARM C is:
        //   ADC -> C = x86 CF        SBB -> C = NOT x86 CF (so SBCS' -(NOT C) = -CF).
        // The op's OWN result is itself deferrable: after ADCS, live C = x86 carry-out, so the canonical
        // spill is the FL_ADD finalizer (e_nzcv_save_ci, flip-C); after SBCS, live C is already the borrow
        // convention, so it is the FL_SUB finalizer (e_nzcv_save). FL_ADC/FL_SBB therefore FOLD into
        // FL_ADD/FL_SUB with bit-identical finalizer bytes, and every downstream Jcc/boundary/SETcc
        // consumer handles them unchanged.
        int adc = (kind == 2);
        uint32_t opc = adc ? 0x3A000000u : 0x7A000000u; // adcs / sbcs
        if (lazyflags_on() && g_fl_pending != FL_NONE) {
            // Carry-in is derivable from the deferred producer's LIVE NZCV with a single C-bit fixup --
            // no cpu->nzcv load/store. Producer live ARM C: FL_SUB -> NOT CF, FL_ADD -> CF, FL_LOGIC ->
            // (x86 CF forced to 0, since AND/OR/XOR/TEST clear CF). An adc;adc;… / sbb;sbb;… bignum chain
            // thus stays in registers with the host carry flowing, never touching cpu->nzcv per step.
            switch (g_fl_pending) {
            case FL_SUB:
                if (adc) e_nzcv_C_op(A_EOR); /* NOT CF -> CF; SBB needs NOT CF already */
                break;
            case FL_ADD:
                if (!adc) e_nzcv_C_op(A_EOR); /* CF ok for ADC; SBB needs NOT CF */
                break;
            case FL_LOGIC:
                e_nzcv_C_op(adc ? A_BIC : A_ORR); /* x86 CF=0: ADC C=0, SBB C=1 */
                break;
            default: break;
            }
            e_rrr(opc, out, a, b, sf, 0);         // adcs / sbcs off the live carry
            e_pf_save(out);                       // x86 PF source = result low byte (incl. carry)
            g_fl_pending = adc ? FL_ADD : FL_SUB; // defer own flags (FL_ADC==FL_ADD, FL_SBB==FL_SUB)
            return;
        }
        // No live producer (FL_NONE) under lazy, OR NOLAZY: carry-in from cpu->nzcv (membank).
        if (adc)
            e_nzcv_load_ci(); // live ARM C = x86 CF
        else
            e_nzcv_load(); // live ARM C = stored borrow (= NOT x86 CF)
        e_rrr(opc, out, a, b, sf, 0);
        e_pf_save(out); // x86 PF source = result low byte (incl. carry)
        if (lazyflags_on())
            g_fl_pending = adc ? FL_ADD : FL_SUB; // keep the chain alive: defer (same finalizer bytes)
        else if (adc)
            e_nzcv_save_ci(); // NOLAZY: exact pre-opt3 inline path (spill to membank)
        else
            e_nzcv_save();
        return;
    }
    int logical = (kind == 1 || kind == 4 || kind == 6); // or/and/xor (and test): x86 clears CF
    // x86 PF: stash the result's low byte (computed from pristine a,b before alu_core may overwrite `out`).
    // PF depends only on the low 8 bits, so a non-flag, non-width-extended op gives the right source byte.
    {
        uint32_t pfop = (kind == 0) ? A_ADD : (kind == 1) ? A_ORR : (kind == 6) ? A_EOR : (kind == 4) ? A_AND : A_SUB;
        e_rrr(pfop, 25, a, b, 0, 0);
        e_pf_save(25);
    }
    if (w >= 4) {
        alu_core(ak, out, a, b, sf);
        // opt3: defer the NZCV materialization (record which finalizer would spill it). The live ARM
        // NZCV holds the result flags; an immediately-following Jcc branches off them directly and any
        // other consumer/boundary calls flags_materialize() -- emitting the exact same finalizer bytes.
        // Sub/cmp always defers (the PR1 baseline path). Under NOLAZY, add/logical materialize inline
        // (exactly the pre-opt3 behavior) so only sub/cmp stays deferred.
        int lazy = lazyflags_on();
        if (kind == 0) {
            if (lazy)
                g_fl_pending = FL_ADD;
            else
                e_nzcv_save_ci();
        } else if (logical) {
            if (lazy)
                g_fl_pending = FL_LOGIC;
            else
                e_nzcv_save_c1();
        } else {
            g_fl_pending = FL_SUB;
        }
        return;
    }
    int sh = 8 * (4 - w);                       // 24 for byte, 16 for word
    e_lsl_i(21, a, sh, 0);                      // x21 = a << sh
    e_lsl_i(22, b, sh, 0);                      // x22 = b << sh
    alu_core(ak, dst < 0 ? 31 : 21, 21, 22, 0); // op in high bits -> correct NZCV
    if (kind == 0)
        e_nzcv_save_ci();
    else if (logical)
        e_nzcv_save_c1();
    else
        e_nzcv_save();
    if (dst >= 0) {
        e_lsr_i(21, 21, sh, 0);
        e_bfi(dst, 21, 0, 8 * w, 1);
    } // merge low w bytes
}

// Byte/word ADC/SBB. do_alu only handles width>=4 (ARM ADCS/SBCS); ARM has no narrow add-with-carry, and
// the high-bit trick can't inject the carry at the byte's LSB. So compute the masked result + the EXACT
// x86 CF/OF/SF/ZF explicitly, then store the borrow-convention NZCV via e_nzcv_save_setcf. `dst`>=0 gets
// the low w bytes merged (bfi); a/b are value regs. Scratch x19..x27 (callee-saved host regs the
// trampoline preserves; never a guest x0..x15, the value x16, or the EA x17 -- so a mem dest still works).
static void narrow_adcsbb(int adc, int dst, int a, int b, int w) {
    int bits = 8 * w;
    e_uxt(21, a, w); // x21 = a & mask  (read operands FIRST -- a/b may alias scratch like x19/x16)
    e_uxt(22, b, w); // x22 = b & mask
    e_movconst(25, 1);
    // x19 = x86 CF (0/1): stored nzcv C (bit29) is the BORROW (= NOT x86 CF), so x86CF = NOT bit29.
    e_ldr(19, 28, OFF_NZCV);
    e_lsr_i(19, 19, 29, 1);
    e_rrr(A_AND, 19, 19, 25, 0, 0);
    e_rrr(A_EOR, 19, 19, 25, 0, 0);
    if (adc) {
        e_rrr(A_ADD, 23, 21, 22, 0, 0);
        e_rrr(A_ADD, 23, 23, 19, 0, 0); // x23 = a8 + b8 + cf
    } else {
        e_rrr(A_SUB, 23, 21, 22, 0, 0);
        e_rrr(A_SUB, 23, 23, 19, 0, 0); // x23 = a8 - b8 - cf (negative -> bits>=`bits` set = borrow)
    }
    e_uxt(24, 23, w);         // x24 = result (low w bytes)
    e_lsr_i(20, 23, bits, 0); // new x86 CF / borrow = bit `bits` of the wide result
    e_rrr(A_AND, 20, 20, 25, 0, 0);
    // OF: add = ((a^res)&(b^res))msb ; sub = ((a^b)&(a^res))msb
    if (adc) {
        e_rrr(A_EOR, 26, 21, 24, 0, 0);
        e_rrr(A_EOR, 27, 22, 24, 0, 0);
    } else {
        e_rrr(A_EOR, 26, 21, 22, 0, 0);
        e_rrr(A_EOR, 27, 21, 24, 0, 0);
    }
    e_rrr(A_AND, 26, 26, 27, 0, 0);
    e_lsr_i(26, 26, bits - 1, 0);
    e_rrr(A_AND, 26, 26, 25, 0, 0); // x26 = OF (0/1)
    e_lsr_i(27, 24, bits - 1, 0);
    e_rrr(A_AND, 27, 27, 25, 0, 0); // x27 = SF (0/1)
    e_rrr(A_SUBS, 31, 24, 31, 0, 0);
    e_cset(23, 0 /*EQ*/, 0); // x23 = ZF
    e_lsl_i(27, 27, 31, 1);
    e_lsl_i(23, 23, 30, 1);
    e_lsl_i(26, 26, 28, 1);
    e_rrr(A_ORR, 27, 27, 23, 1, 0);
    e_rrr(A_ORR, 27, 27, 26, 1, 0);
    emit32(0xD51B4200u | 27);                 // msr nzcv, x27  (live N/Z/V)
    e_nzcv_save_setcf(20);                    // store N/Z/V, set stored C = NOT new-CF
    e_pf_save(24);                            // x86 PF source = result low byte
    if (dst >= 0) e_bfi(dst, 24, 0, bits, 1); // merge low w bytes into dst
}

// LOCK-prefixed read-modify-write to a memory operand, done ATOMICALLY via an LSE op (x17 = EA already
// computed). `rs` is the operand value register. `k` is the alu kind (0 add, 1 or, 4 and, 5 sub, 6 xor).
// x86 flags are set from (old OP operand); x19/x20 are scratch. Returns 1 if it emitted an atomic, 0 if
// `k` has no atomic form here (caller falls back to the non-atomic load-op-store).
static int lock_rmw(int k, int w, int rs) {
    int sf = (w == 8);
    uint32_t lse;
    int rsu = rs;
    switch (k) {
    case 0: lse = LSE_LDADD; break;
    case 5:
        e_rrr(A_SUB, 20, 31, rs, sf, 0);
        rsu = 20;
        lse = LSE_LDADD;
        break;                      // sub: atomic add(-v)
    case 1: lse = LSE_LDSET; break; // or
    case 6: lse = LSE_LDEOR; break; // xor
    case 4:
        e_rrr(A_ORN, 20, 31, rs, sf, 0);
        rsu = 20;
        lse = LSE_LDCLR;
        break; // and: clear ~v
    default: return 0;
    }
    e_lse(lse, w, rsu, 19, 17); // x19 = old; [x17] op= rsu  (acquire-release)
    do_alu(k, -1, 19, rs, w);   // x86 flags from (old OP original-operand)
    return 1;
}

// x86 condition (opcode low nibble) -> ARM cond, or -1 if unsupported (parity).
static int x86cc_to_arm(int cc) {
    // Parity (idx 10/11, jp/jnp/setp/.../cmovp/...) is NOT routed here -- it reads the real PF lane
    // (cpu->pf) via e_pf_compute, so its slots below (mapping onto ARM V) are dead. Everything else is
    // a direct NZCV condition.
    static const int t[16] = {6, 7, 3, 2, 0, 1, 9, 8, 4, 5, 6, 7, 11, 10, 13, 12};
    return t[cc & 0xF];
}

// jp/jnp (parity jcc): spill any deferred flags to membank (this is a block boundary for the
// successor blocks), then compute the real x86 PF lane into the live ARM Z flag and return the ARM
// condition the branch machinery should test. Mirrors setp/setnp + cmovp/cmovnp, which already read
// cpu->pf instead of the stale ARM V flag. `lo` is the opcode low nibble (0xA=jp, 0xB=jnp).
static int emit_parity_jcc_cond(int lo) {
    if (g_fl_pending) flags_materialize(); // spill the deferred producer to membank (boundary)
    e_pf_compute(19);                      // x19 = x86 PF in {0,1} (scratch x16; x17/EA preserved)
    e_rrr(A_SUBS, 31, 19, 31, 0, 0);       // live ARM Z = (PF == 0)
    return (lo == 0xA) ? 1 /*NE: PF==1*/ : 0 /*EQ: PF==0*/;
}

#include "translate/trace.c"

#include "translate/repstr.c"

#include "translate/x87.c"

// SSE2 variable-count packed shift (PSLLW/D/Q, PSRLW/D/Q, PSRAW/D by xmm/m): shift every
// `esize`-bit lane of `vn` by the SCALAR count held in the low 64 bits of `vs`, result -> `vd`.
// x86 saturates the count: any count >= esize yields 0 (logical) or the sign bit replicated
// (arithmetic right). NEON USHL/SSHL take a per-lane signed amount from the low byte of each
// lane, so we clamp the (unsigned) count to esize -- which is < 128, keeping the signed byte
// valid -- and DUP it across all lanes (negated for a right shift).
static void e_sse_var_shift(int vd, int vn, int vs, int esize, int left, int arith) {
    uint32_t sz = esize == 16 ? 1u : esize == 32 ? 2u : 3u; // NEON element size field
    uint32_t imm5 = esize == 16 ? 2u : esize == 32 ? 4u : 8u;
    emit32(0x4E083C00u | (vs << 5) | 16);     // umov x16, vs.d[0]   (the 64-bit count)
    e_movconst(19, esize);
    e_rrr(A_SUBS, 31, 16, 19, 1, 0);          // cmp x16, esize
    e_csel(16, 19, 16, 8 /*HI*/, 1);          // x16 = (count u> esize) ? esize : count
    if (!left) e_rrr(A_SUB, 16, 31, 16, 1, 0); // right shift -> negative NEON amount (neg x16)
    emit32(0x4E000C00u | (imm5 << 16) | (16 << 5) | 17);             // dup v17.<T>, w16/x16
    uint32_t shl = (arith ? 0x4E204400u : 0x6E204400u) | (sz << 22); // SSHL (arith) / USHL
    emit32(shl | (17 << 16) | (vn << 5) | vd);                       // [s|u]shl vd, vn, v17
}

// Emit a REAL host trap (UDF -> SIGILL, BRK -> SIGTRAP) for an x86 fault/trap the guest may handle.
// It is emitted INLINE with the 16 guest GPRs still live in host x0..x15 and xmm in v0..v15, so the
// synchronous-fault guard (jit86_syncguard -> deliver_guest_fault -> sigframe_capture_fault, frontend/
// x86_64/sigframe.c) reconstructs guest state from the host fault context and either delivers the signal
// to the guest's handler or default-terminates when there is none. cpu->rip is set to the architectural
// PC the handler observes and sigreturn resumes at -- the faulting insn for a #UD/#DE fault, the
// following insn for an int3 (#BP) trap. Lazy flags and the x87 shadow top are spilled first so the
// rt_sigframe (built from cpu->nzcv / cpu->st[]) is current.
static void emit_guest_trap(uint64_t rip, uint32_t trap) {
    if (g_fl_pending) flags_materialize();
    if (g_fp_known) fp_drop();
    e_movconst(16, rip);   // scratch x16 (not a guest GPR) -> cpu->rip
    e_str(16, 28, OFF_RIP);
    emit32(trap); // guest GPRs x0..x15 + xmm v0..v15 are still live at the trap
}

// Integer DIV/IDIV by zero raises #DE (SIGFPE) on x86, but ARM sdiv/udiv quietly return 0 -- a guest
// #DE would be silently swallowed. Guard the inline (8/16/32-bit) divides: when the (width-extended)
// divisor in divreg is zero, route to the C div path (R_DIV/R_IDIV in dispatch.c), which reports the
// #DE -- the same exit the 64-bit DIV already uses. The non-zero path falls straight through to the
// inline divide, so normal division is unaffected.
static void emit_div_zero_check(int divreg, uint64_t next, int idiv) {
    uint32_t *patch = (uint32_t *)g_cp;
    emit32(0);                    // cbnz divreg, ok  (divisor != 0): offset patched below
    e_str(divreg, 28, OFF_DIVOP); // divisor (== 0) -> cpu->divop for the C #DE path
    emit_exit_const(next, idiv ? R_IDIV : R_DIV);
    int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
    *patch = 0xB5000000u | (((uint32_t)d & 0x7FFFF) << 5) | (uint32_t)divreg; // cbnz x[divreg], ok
}

// 0F 0B (UD2): an explicitly-undefined opcode that real software (e.g. ruby's unreachable/trap paths,
// libc CPU-feature probes) uses as a deliberate trap. On x86 it raises #UD -> SIGILL; with a guest
// handler that runs, otherwise the process dies with status 128+SIGILL = 132. Emit a host UDF so the
// SIGILL guard delivers it to the guest handler (or default-terminates), instead of the old always-
// terminate hack. This is distinct from report_unimpl's "engine aborted" path (status 70), which would
// mislabel a legitimate guest fault as an unimplemented-opcode bug of ours.
static void emit_sigill(uint64_t pc) {
    // Quiet by default: UD2 frequently sits on never-taken paths (compiler trap/unreachable slots) that get
    // translated as block fall-through but never run; an unconditional message would falsely imply delivery.
    if (getenv("CRASHDBG")) fprintf(stderr, "[jit86] #UD ud2 at rip=%llx -> SIGILL\n", (unsigned long long)pc);
    emit_guest_trap(pc, 0x00000000u); // udf #0 -> host SIGILL
}

// Translate the basic block at guest address gpc; returns host entry pointer.
static void *translate_block(uint64_t gpc) {
    uint64_t start = gpc;
    void *host = g_cp;
    emit_prologue();
    void *body = g_cp;
    g_fl_pending = FL_NONE; // lazy flags: nothing deferred at block entry
    g_df = 0;               // direction flag: forward at block entry (std/cld are block-local around string ops)
    g_fp_known = 0;         // x87: top unknown at block entry until a finit anchors it
    g_fp_dirty = 0;
    g_prof_xlate++; // PROF (measurement-only): translate_block calls
    if (g_stitch < 0) g_stitch = (getenv("NOSTITCH") == NULL);
    // W3-A superblock state: guest block-starts already laid in this region + region budget.
    uint64_t seen[TRACE_MAX_BLK];
    int nseen = 0, trace_blk = 0;
    seen[nseen++] = start;
#define STITCH_OK                                                                                                      \
    (g_stitch && !g_nochain && !g_trace && !g_itrace && trace_blk < TRACE_MAX_BLK - 1 &&                               \
     (size_t)((uint8_t *)g_cp - (uint8_t *)host) < TRACE_MAX_BYTES)
    for (;;) {
        if (g_itrace && gpc != start) {
            if (g_fl_pending) flags_materialize(); // materialize before boundary
            fp_drop();                             // x87: spill the shadow top before the boundary
            emit_chain_exit(gpc);
            break;
        } // 1 insn/block: per-instruction register dump
        struct insn I;
        decode(gpc, &I);
        uint64_t next = gpc + I.len;
        uint8_t op = I.op;
        int sf = I.opsize == 8;
        // VEX/EVEX (AVX/AVX2/AVX-512): not lowered to NEON. Exit the block and emulate this single insn in C
        // (do_avx), which owns the full v[]/vhi/vz/vx/kreg register file + memory. rip := gpc so do_avx
        // decodes the insn at rip and advances past it. Done BEFORE the lazy-flag classifier (which only
        // knows legacy opcodes) -- AVX touches no EFLAGS we model, so just spill any pending flags first.
        if (I.vex) {
            if (g_fl_pending) flags_materialize();
            emit_exit_const(gpc, R_AVX);
            break;
        }
        // Legacy (non-VEX) 0F38/0F3A SSSE3/SSE4/AES/SHA/PCLMUL/CRC32/MOVBE: emulate this single insn in C
        // (do_sse3b) -- correctness-first, mirroring the AVX path. These touch no EFLAGS we lazily defer
        // (cmp-string sets flags but writes them through cpu->nzcv in C), so just spill any pending flags.
        if (I.map3) {
            if (g_fl_pending) flags_materialize();
            emit_exit_const(gpc, R_SSE3B);
            break;
        }
        if (g_trace)
            fprintf(stderr, "[dec] %llx %s%02x len=%d mod%d rm%d reg%d mem%d base%d idx%d disp=%lld imm=%lld\n",
                    (unsigned long long)gpc, I.two ? "0F " : "", op, I.len, I.mod, I.rm_reg, I.reg, I.is_mem,
                    I.m_hasbase ? I.m_base : -1, I.m_hasindex ? I.m_index : -1, (long long)I.disp, (long long)I.imm);

        // Lazy flags: a pending width-4/8 producer left its result flags live in NZCV instead of
        // spilling to cpu->nzcv. The ONLY consumer allowed to read them live is an immediately-
        // following Jcc (rel8 70-7F / rel32 0F 80-8F). For every other instruction:
        //   - opt3 dead-flag elimination: if this instruction fully overwrites NZCV and reads no
        //     flags (insn_is_flagkill), the pending flags are dead -- drop them, emitting nothing;
        //   - otherwise (a non-Jcc flag reader/consumer or a block-ender): materialize to membank
        //     NOW, before it emits anything, with the exact finalizer the producer would have used.
        // Both keep the cross-block cpu->nzcv ABI byte-identical (intra-block only). NOLAZY disables
        // dead-flag elimination, so the pending sub/cmp always materializes (PR1 behavior).
        int is_jcc = (!I.two && op >= 0x70 && op <= 0x7F) || (I.two && (op & 0xF0) == 0x80);
        if (g_fl_pending && !is_jcc) {
            int lazy = lazyflags_on();
            if (lazy && insn_is_carry_consumer(&I)) {
                // opt3 carry-flow: leave the producer's flags LIVE; do_alu's adc/sbb pulls its x86 CF
                // carry-in straight from them (FL_SUB/FL_ADD/FL_LOGIC) -- no eager materialize.
            } else if (lazy && insn_is_flagkill(&I))
                g_fl_pending = FL_NONE; // dead: next op fully overwrites the flags before any read
            else
                flags_materialize();
        }

        // x87 static-top tracking ends at any non-x87 instruction: spill the shadow top to
        // cpu->fptop and drop to the runtime-top model (the run only spans consecutive x87 ops, so
        // no top assumption ever crosses a non-x87 op, a branch target, or a block boundary).
        if (g_fp_known && !(!I.two && op >= 0xD8 && op <= 0xDF)) fp_drop();

        if (!I.two) {
            // ---- mov r8, imm8 (B0+r) ----
            if (op >= 0xB0 && op <= 0xB7) {
                int rnum = (op - 0xB0) | (I.rexB << 3);
                e_movz(16, (uint32_t)(I.imm & 0xff), 0);
                byte_wb(&I, rnum, 16);
                gpc = next;
                continue;
            }
            // ---- mov r, imm (B8+r) ----
            if (op >= 0xB8 && op <= 0xBF) {
                int rd = (op - 0xB8) | (I.rexB << 3);
                e_movconst(rd, sf ? (uint64_t)I.imm : (uint64_t)(uint32_t)I.imm);
                gpc = next;
                continue;
            }
            // ---- mov r/m, imm (C7 /0, C6 /0) ----
            if (op == 0xC7 || op == 0xC6) {
                int w = op == 0xC6 ? 1 : I.opsize;
                if (I.is_mem) {
                    emit_ea(&I, next);
                    e_movconst(16, (uint64_t)I.imm);
                    e_store(w, 16, 17);
                } else
                    e_movconst(I.rm_reg, sf ? (uint64_t)I.imm : (uint64_t)(uint32_t)I.imm);
                gpc = next;
                continue;
            }
            // ---- mov r/m,r (88/89) and r,r/m (8A/8B) ----
            if (op == 0x88 || op == 0x89 || op == 0x8A || op == 0x8B) {
                int w = (op & 1) ? I.opsize : 1;
                int to_reg = (op & 2); // 8A/8B: dest is reg
                if (I.is_mem) {
                    if (to_reg) {     // mov reg, [mem]  -- folded into one ldr when [base+disp]
                        if (w == 1) { // byte dest: ah/bh/ch/dh -> bits 8-15; lo8 preserves upper
                            emit_load_mem(&I, next, 1, 16);
                            byte_wb(&I, I.reg, 16);
                        } else
                            emit_load_mem(&I, next, w, I.reg);
                    } else { // mov [mem], reg  -- folded into one str when [base+disp]
                        int rn, off, f = ea_imm_fold(&I, w, &rn, &off);
                        if (f) {
                            int sv = (w == 1) ? byte_val(&I, I.reg, 16) : I.reg;
                            if (f == 1)
                                e_store_uoff(w, sv, rn, (unsigned)off);
                            else
                                e_stur(w, sv, rn, off);
                        } else {
                            emit_ea(&I, next);                                   // may clobber x16
                            int sv = (w == 1) ? byte_val(&I, I.reg, 16) : I.reg; // byte src: ah/bh/ch/dh -> bits 8-15
                            e_store(w, sv, 17);
                        }
                    }
                } else if (w == 1) {
                    // byte reg-to-reg (88/8A, mod=3): copy ONE byte, hi8-aware, preserving the dest's other
                    // bits. The full-width e_mov_rr below was wrong here -- `mov bl,cl` copied all of ecx into
                    // ebx (and high-byte src/dst like `mov al,dh` were garbage), only masked when the upper
                    // bytes happened to be 0. That polluted icu's TinyStr niche math -> dropped 'n' in "en-US".
                    int srcreg = to_reg ? I.rm_reg : I.reg;
                    int dstreg = to_reg ? I.reg : I.rm_reg;
                    int sv = byte_val(&I, srcreg, 16);
                    byte_wb(&I, dstreg, sv);
                } else {
                    if (to_reg)
                        e_mov_rr(I.reg, I.rm_reg, sf);
                    else
                        e_mov_rr(I.rm_reg, I.reg, sf);
                }
                gpc = next;
                continue;
            }
            // ---- lea (8D) ----
            if (op == 0x8D) {
                // W6A item 1: a rip-relative lea that materializes a *_type pointer in a biased non-PIE Go
                // image. The image maps high (+bias) but its baked-absolute type pointers keep their low link
                // addresses, so a high lea result compared against a baked low type pointer (Go type identity:
                // interface assertions, SetFinalizer's `fint == etyp`, itab keys) diverges. When the target
                // lands in the type section [md.types, md.etypes) emit the low link address so every *_type
                // pointer is low-consistent; the eventual low type-struct access is served by nonpie_fixup.
                // The rewrite is confined to the type section so string/rodata pointers (used as bulk memory
                // operands, e.g. by rep movs) and code/data pointers keep their high mapped addresses. Only
                // the 64-bit form (sf). Inert for PIE/static-PIE and non-Go images (types range == 0).
                if (sf && I.rip_rel && g_nonpie_types_lo) {
                    uint64_t lo = (next - g_nonpie_bias) + (uint64_t)I.disp; // low link target
                    if (lo >= g_nonpie_types_lo && lo < g_nonpie_types_hi) {
                        e_movconst(I.reg, lo);
                        gpc = next;
                        continue;
                    }
                }
                emit_ea_core(&I, next, 0); // lea returns the guest (low) effective ADDRESS -> no bias-fold
                e_mov_rr(I.reg, 17, sf);
                gpc = next;
                continue;
            }
            // ---- push/pop r (50-5F) ----
            if (op >= 0x50 && op <= 0x57) {
                int r = (op - 0x50) | (I.rexB << 3);
                e_subi(RSP, RSP, 8, 1);
                e_store(8, r, RSP);
                gpc = next;
                continue;
            } // push (64-bit)
            if (op >= 0x58 && op <= 0x5F) {
                int r = (op - 0x58) | (I.rexB << 3);
                e_load(8, r, RSP);
                e_addi(RSP, RSP, 8, 1);
                gpc = next;
                continue;
            } // pop
            // ---- movsxd (0x63): r64 = sign-extend r/m32 ----
            if (op == 0x63) {
                if (I.is_mem) {
                    emit_ea(&I, next);
                    e_ldrs(4, I.reg, 17);
                } else
                    e_sxt(I.reg, I.rm_reg, 4);
                gpc = next;
                continue;
            }
            // ---- ALU primary (00..3D): /r reg,r/m forms ----
            // gate on op<0x40: bits[7:6]==00 is primary ALU. 0x80-0x83 (group1) handled below.
            if (op < 0x40 && (op & 7) <= 3 && alu_kind_primary(op) >= 0) {
                int k = alu_kind_primary(op), dir = op & 2; // dir 0: r/m,reg ; 2: reg,r/m
                int w = (op & 1) ? I.opsize : 1, mem;
                if ((k == 2 || k == 3) && w < 4) { // byte/word ADC/SBB -> narrow_adcsbb (do_alu is 32/64 only)
                    int adc = (k == 2), m2;
                    int rmv2 = rm_load(&I, next, w, &m2);
                    int regv2 = (w == 1) ? byte_val(&I, I.reg, 24) : I.reg;
                    if (dir) { // dst = reg
                        narrow_adcsbb(adc, 16, regv2, rmv2, w);
                        if (w == 1)
                            byte_wb(&I, I.reg, 16);
                        else
                            e_bfi(I.reg, 16, 0, 8 * w, 1);
                    } else { // dst = r/m
                        narrow_adcsbb(adc, 16, rmv2, regv2, w);
                        rm_store(&I, w, 16);
                    }
                    gpc = next;
                    continue;
                }
                int rmv = rm_load(&I, next, w, &mem);
                int regv = (w == 1) ? byte_val(&I, I.reg, 24) : I.reg; // reg operand value (handle ah/ch)
                if (dir) {                                             // dst = reg
                    if (k == 7)
                        do_alu(7, -1, regv, rmv, w); // cmp: no write
                    else if (w == 1) {
                        do_alu(k, 16, regv, rmv, w);
                        byte_wb(&I, I.reg, 16);
                    } else
                        do_alu(k, I.reg, I.reg, rmv, w);
                } else { // dst = r/m
                    if (!(I.lock && mem && k != 7 && lock_rmw(k, w, regv))) {
                        do_alu(k, (k == 7) ? -1 : 16, rmv, regv, w);
                        if (k != 7) rm_store(&I, w, 16);
                    }
                }
                gpc = next;
                continue;
            }
            // ALU al/eax/rax, imm (04/05 ... 3C/3D)
            if (op < 0x40 && ((op & 7) == 4 || (op & 7) == 5) && alu_kind_primary(op) >= 0) {
                int k = alu_kind_primary(op), w = (op & 7) == 4 ? 1 : I.opsize;
                if (!((k == 2 || k == 3) && w < 4)) {
                    e_movconst(16, (uint64_t)I.imm);
                    do_alu(k, k == 7 ? -1 : RAX, RAX, 16, w);
                } else { // byte/word ADC/SBB al/ax, imm -- do_alu is 32/64-only, mirror the group1 narrow path
                    e_movconst(19, (uint64_t)I.imm);
                    narrow_adcsbb(k == 2, 16, RAX, 19, w);
                    e_bfi(RAX, 16, 0, 8 * w, 1); // write low w bytes into the accumulator, preserve upper
                }
                gpc = next;
                continue;
            }
            // ---- group1 (80/81/83): ALU r/m, imm ----
            if (op == 0x80 || op == 0x81 || op == 0x83) {
                int k = I.reg & 7, w = op == 0x80 ? 1 : I.opsize, mem;
                if (!((k == 2 || k == 3) && w < 4)) {     // ADC/SBB ok for 32/64-bit
                    int rmv = rm_load(&I, next, w, &mem); // mem -> x16 (val), x17 (EA)
                    if (I.lock && mem && k != 7) {        // LOCK op [mem], imm -> atomic (e.g. lock add $1)
                        e_movconst(21, (uint64_t)I.imm);  // operand in x21 (x19/x20 are lock_rmw scratch)
                        if (lock_rmw(k, w, 21)) {
                            gpc = next;
                            continue;
                        }
                    }
                    e_movconst(19, (uint64_t)I.imm); // imm in x19 (x16 holds the loaded value)
                    // compute into scratch x16, then rm_store -> correct dest (handles mem + hi/lo byte regs)
                    do_alu(k, (k == 7) ? -1 : 16, rmv, 19, w);
                    if (k != 7) rm_store(&I, w, 16);
                    gpc = next;
                    continue;
                } else { // byte/word ADC/SBB r/m, imm
                    int adc = (k == 2);
                    int rmv = rm_load(&I, next, w, &mem); // value -> x16
                    e_movconst(19, (uint64_t)I.imm);      // imm in x19 (narrow_adcsbb reads b before clobbering x19)
                    narrow_adcsbb(adc, 16, rmv, 19, w);
                    rm_store(&I, w, 16);
                    gpc = next;
                    continue;
                }
            }
            // ---- test (84/85, A8/A9, F6/F7 /0) ----
            if (op == 0x84 || op == 0x85) {
                int w = (op & 1) ? I.opsize : 1, mem;
                int rmv = rm_load(&I, next, w, &mem);
                int regv = (w == 1) ? byte_val(&I, I.reg, 24) : I.reg; // reg operand: handle ah/bh/ch/dh
                do_alu(4, -1, rmv, regv, w);
                gpc = next;
                continue; // test = and(discard)
            }
            if (op == 0xA8 || op == 0xA9) {
                int w = op == 0xA8 ? 1 : I.opsize;
                e_movconst(16, (uint64_t)I.imm);
                do_alu(4, -1, RAX, 16, w);
                gpc = next;
                continue;
            }
            // ---- shifts: group2 (C0/C1 imm, D0/D1 by 1, D2/D3 by CL) ----
            if (op == 0xC0 || op == 0xC1 || op == 0xD0 || op == 0xD1 || op == 0xD2 || op == 0xD3) {
                int k = I.reg & 7;
                if (k == 6) k = 4; // SAL == SHL
                int w = (op & 1) ? I.opsize : 1, mem;
                int bycl = (op == 0xD2 || op == 0xD3), by1 = (op == 0xD0 || op == 0xD1);
                // RCL(/2)/RCR(/3) through carry: the constant-count forms (by-1, immediate) are emitted here;
                // the by-CL form still defers (caught by the unimpl check below).
                if ((k == 2 || k == 3) && !bycl) {
                    int cmask = (w == 8) ? 63 : 31;
                    emit_rcl_rcr(&I, next, w, k == 3, by1 ? 1 : ((int)I.imm & cmask));
                    gpc = next;
                    continue;
                }
                if (k != 0 && k != 1 && k != 4 && k != 5 && k != 7) {
                    report_unimpl(gpc, &I);
                    break;
                } // RCL/RCR by CL defer
                int raw = rm_load(&I, next, w, &mem);
                if ((k == 0 || k == 1) && w < 4) {        // 8/16-bit ROL/ROR -- rotate WITHIN the operand width
                    int width = 8 * w;                    // (a 64-bit ROR would wrap the wrong bits, e.g. rolw $8)
                    e_uxt(16, raw, w);                    // x16 = zero-extended operand (low `width` bits)
                    e_bfi(16, 16, width, width, 0);       // replicate v -> [2w-1:w] (16-bit: now 32 bits = v|v)
                    if (w == 1) e_bfi(16, 16, 16, 16, 0); // byte: replicate the pair again -> 4 copies fill 32 bits
                    if (bycl) {                           // count = CL masked to the operand width
                        e_movconst(19, width - 1);
                        e_rrr(A_AND, 20, RCX, 19, 0, 0); // x20 = CL & (width-1)
                        if (k == 0) {
                            e_movconst(19, width);
                            e_rrr(A_SUB, 20, 19, 20, 0, 0); // ROL by n == ROR by (width-n)
                            e_movconst(19, width - 1);
                            e_rrr(A_AND, 20, 20, 19, 0, 0);
                        }
                        e_shv(S_RORV, 16, 16, 20, 0); // 32-bit RORV of the replicated value -> low `width` bits correct
                    } else {
                        int ce = (((int)(I.imm) % width) + width) % width;
                        int rr = (k == 1) ? ce : (width - ce) % width;
                        if (rr) e_ror_i(16, 16, rr, 0); // 32-bit ROR; low `width` bits are the answer
                    }
                    rm_store(&I, w, 16); // stores low w bytes; x86 rotates leave SF/ZF unchanged -> no flag save
                    gpc = next;
                    continue;
                }
                int ssf = (w >= 4) ? sf : 1; // operate 64-bit on extended byte/word
                // bring the operand into x16, zero/sign-extended for w<4
                if (w < 4) {
                    if (k == 5)
                        e_uxt(16, raw, w);
                    else if (k == 7)
                        e_sxt(16, raw, w);
                    else
                        e_mov_rr(16, raw, 0);
                } else if (raw != 16)
                    e_mov_rr(16, raw, sf);
                int src = 16;
                int cnt = by1 ? 1 : (bycl ? -1 : (int)(I.imm & (ssf ? 63 : 31)));
                // exact x86 CF (last bit shifted out) for SHL/SHR/SAR immediate at 32/64-bit
                int want_cf = (!bycl && w >= 4 && (k == 4 || k == 5 || k == 7) && cnt >= 1);
                if (want_cf) e_mov_rr(19, src, ssf); // save original operand for CF
                if (bycl) {
                    if (k == 0) { // ROL r/m32|64 by CL == ROR by (width - n); leaves SF/ZF unchanged (no flag save)
                        int width = ssf ? 64 : 32;
                        e_movconst(19, width - 1);
                        e_rrr(A_AND, 20, RCX, 19, ssf, 0); // x20 = n = CL & (width-1)
                        e_movconst(19, width);
                        e_rrr(A_SUB, 20, 19, 20, ssf, 0); // x20 = width - n
                        e_movconst(19, width - 1);
                        e_rrr(A_AND, 20, 20, 19, ssf, 0); // x20 = (width - n) & (width-1)  -> n==0 maps to rot 0
                        e_shv(S_RORV, 16, src, 20, ssf);  // rorv x16, src, x20
                        rm_store(&I, w, 16);
                        gpc = next;
                        continue;
                    }
                    uint32_t b = k == 4 ? S_LSLV : k == 5 ? S_LSRV : k == 7 ? S_ASRV : S_RORV;
                    e_shv(b, 16, src, RCX, ssf);
                } else {
                    if (cnt == 0) {
                        if (mem) e_store(w, raw, 17);
                        gpc = next;
                        continue;
                    } // no flags change
                    if (k == 4)
                        e_lsl_i(16, src, cnt, ssf);
                    else if (k == 5)
                        e_lsr_i(16, src, cnt, ssf);
                    else if (k == 7)
                        e_asr_i(16, src, cnt, ssf);
                    else if (k == 1)
                        e_ror_i(16, src, cnt, ssf);
                    else /*k==0 ROL*/
                        e_ror_i(16, src, (ssf ? 64 : 32) - cnt, ssf);
                }
                // SF/ZF from result (byte/word via high-bits); CF exact for immediate SHL/SHR/SAR, else approximate
                if (w < 4) {
                    e_lsl_i(21, 16, 8 * (4 - w), 0);
                    e_tst(21, 0);
                } else
                    e_tst(16, sf);
                if (bycl) {
                    // x86 leaves ALL flags unchanged when the runtime count (CL masked to the operand width) is
                    // 0 -- exactly when the emitted variable shift was a no-op. Capture the would-be flags, then
                    // keep the OLD nzcv (and PF, for the SHL/SHR/SAR forms) if the masked count is zero.
                    emit32(0xD53B4200u | 20);           // mrs x20, nzcv  (new N/Z from result; C/V from the tst)
                    e_ldr(24, 28, OFF_NZCV);            // x24 = old nzcv
                    e_movconst(19, ssf ? 63 : 31);
                    e_rrr(A_ANDS, 31, RCX, 19, ssf, 0); // Z = ((CL & (width-1)) == 0)
                    e_csel(20, 24, 20, 0 /*EQ: count==0*/, 1);
                    e_str(20, 28, OFF_NZCV);
                    if (k == 4 || k == 5 || k == 7) {   // SHL/SHR/SAR set PF from the result; rotates leave it
                        e_ldr(25, 28, OFF_PF);          // old PF
                        e_csel(23, 25, 16, 0, 1);       // EQ -> keep old PF, else result low byte (x16)
                        e_pf_save(23);
                    }
                } else {
                    if (want_cf) {
                        int width = ssf ? 64 : 32, bit = (k == 4) ? (width - cnt) : (cnt - 1);
                        if (bit > width - 1) bit = width - 1;
                        e_lsr_i(19, 19, bit, ssf);
                        e_movconst(23, 1);
                        e_rrr(A_AND, 19, 19, 23, ssf, 0); // x19 = x86 CF bit
                        e_nzcv_save_setcf(19);
                    } else
                        e_nzcv_save();
                    // x86 PF: SHL/SHR/SAR set SF/ZF/PF from the result; rotates (ROL/ROR) leave PF unchanged.
                    if (k == 4 || k == 5 || k == 7) e_pf_save(16); // result low byte -> PF lane (x16 holds result)
                }
                rm_store(&I, w, 16);
                gpc = next;
                continue;
            }
            // ---- group3 (F6/F7): /0 test /2 not /3 neg /4 mul /5 imul /6 div /7 idiv ----
            if (op == 0xF6 || op == 0xF7) {
                int k = I.reg & 7, w = op == 0xF6 ? 1 : I.opsize, mem;
                if (k == 0) {
                    int rmv = rm_load(&I, next, w, &mem);
                    e_movconst(19, (uint64_t)I.imm);
                    do_alu(4, -1, rmv, 19, w);
                    gpc = next;
                    continue;
                } // test r/m, imm
                if (k == 2) {
                    int rmv = rm_load(&I, next, w, &mem); // not -> x16, then rm_store
                    emit32(0xAA2003E0u | (rmv << 16) | 16);
                    rm_store(&I, w, 16);
                    gpc = next;
                    continue;
                }
                if (k == 3) {
                    // neg r/m == 0 - r/m. For byte/word, a raw 32-bit SUBS got OF wrong for negb 0x80 /
                    // negw 0x8000 (the INT_MIN overflow case) -- e.g. icu_locid's Option-niche PartialEq does
                    // `negb; jno` on the 0x80 None marker. do_alu shifts byte/word operands into the ARM high
                    // bits so N/Z/V are width-correct. For 32/64-bit the direct SUBS flags are already exact,
                    // and do_alu's deferred (FL_SUB) path would let flags_materialize clobber the x16 result
                    // before rm_store (it broke `neg ebx`), so keep the original inline path there.
                    int rmv = rm_load(&I, next, w, &mem); // neg -> x16
                    if (w < 4) {
                        do_alu(5, 16, 31, rmv, w); // x16 = 0 - rmv, x86 SUB flags at width w (sets PF too)
                    } else {
                        e_rrr(A_SUBS, 16, 31, rmv, w == 8, 0);
                        e_nzcv_save();
                        e_pf_save(16); // x86 PF source = result low byte (do_alu handles the w<4 path)
                    }
                    rm_store(&I, w, 16);
                    gpc = next;
                    continue;
                }
                if (w == 1 && k >= 4) { // 8-bit mul/div (0xF6 /4../7) -- were UNIMPL (glibc inet_ntoa aborts)
                    int rmv = rm_load(&I, next, 1, &mem);
                    if (k == 4 || k == 5) { // mul/imul r/m8: AX = AL * r/m8 (16-bit result)
                        if (k == 4) {
                            e_uxt(19, RAX, 1);
                            e_uxt(20, rmv, 1);
                        } // zero-extend AL + src (uxtb)
                        else {
                            e_sxt(19, RAX, 1);
                            e_sxt(20, rmv, 1);
                        } // sign-extend (sxtb)
                        e_mul(21, 19, 20, 0);
                        e_bfi(RAX, 21, 0, 16, 1); // write AX (low 16), preserve upper RAX (x86 8/16-bit semantics)
                    } else {                      // div/idiv r/m8: AL = AX / r/m8, AH = AX % r/m8
                        if (k == 6) {
                            e_uxt(19, RAX, 2);
                            e_uxt(20, rmv, 1);
                            emit_div_zero_check(20, gpc, 0); // #DE on /0 (rip = the div insn)
                            e_udiv(21, 19, 20, 0);
                        } // AX uxth, src uxtb
                        else {
                            e_sxt(19, RAX, 2);
                            e_sxt(20, rmv, 1);
                            emit_div_zero_check(20, gpc, 1);
                            e_sdiv(21, 19, 20, 0);
                        } // AX sxth, src sxtb
                        e_msub(22, 21, 20, 19, 0); // rem = dividend - quot*divisor
                        e_bfi(RAX, 21, 0, 8, 1);   // AL = quotient
                        e_bfi(RAX, 22, 8, 8, 1);   // AH = remainder
                    }
                    gpc = next;
                    continue;
                }
                if (w == 2 && k >= 4) { // 16-bit mul/div (66 F7 /4../7) -- e.g. uutils `date` does `div si`
                    int rmv = rm_load(&I, next, 2, &mem);
                    if (k == 4 || k == 5) { // mul/imul r/m16: DX:AX = AX * r/m16 (32-bit product)
                        if (k == 4) {
                            e_uxt(19, RAX, 2);
                            e_uxt(20, rmv, 2);
                        } // AX + src zero-extended (uxth)
                        else {
                            e_sxt(19, RAX, 2);
                            e_sxt(20, rmv, 2);
                        } // sign-extended (sxth)
                        e_mul(21, 19, 20, 0);
                        e_bfi(RAX, 21, 0, 16, 1); // AX = low 16
                        e_lsr_i(21, 21, 16, 0);
                        e_bfi(RDX, 21, 0, 16, 1); // DX = high 16
                    } else {                      // div/idiv r/m16: AX = (DX:AX)/r/m16, DX = remainder
                        e_uxt(19, RAX, 2);
                        e_bfi(19, RDX, 16, 16, 0); // x19 = (DX<<16)|AX -- the 32-bit dividend
                        if (k == 6) {
                            e_uxt(20, rmv, 2);
                            emit_div_zero_check(20, gpc, 0); // #DE on /0
                            e_udiv(21, 19, 20, 0);
                        } // unsigned: divisor uxth
                        else {
                            e_sxt(20, rmv, 2);
                            emit_div_zero_check(20, gpc, 1);
                            e_sdiv(21, 19, 20, 0);
                        } // signed: x19 already the 32-bit pattern
                        e_msub(22, 21, 20, 19, 0); // rem = dividend - quot*divisor
                        e_bfi(RAX, 21, 0, 16, 1);  // AX = quotient
                        e_bfi(RDX, 22, 0, 16, 1);  // DX = remainder
                    }
                    gpc = next;
                    continue;
                }
                if (w == 4 || w == 8) {
                    if (k == 4 || k == 5) { // mul / imul (rdx:rax = rax * r/m)
                        int rmv = rm_load(&I, next, w, &mem);
                        // zero/sign-extend operands to 64, full product, lo->rax hi->rdx
                        if (k == 4) {
                            e_mul(19, RAX, rmv, 1);
                            e_umulh(RDX, RAX, rmv);
                        } // unsigned (assumes w==8); for w==4 see below
                        else {
                            e_mul(19, RAX, rmv, 1);
                            e_smulh(RDX, RAX, rmv);
                        }
                        if (w == 4) {
                            e_lsr_i(RDX, 19, 32, 1);
                            e_mov_rr(RAX, 19, 0);
                        } // eax=lo32, edx=hi32
                        else
                            e_mov_rr(RAX, 19, 1);
                        // x86 CF=OF: high half significant? (jc/jo/setc/seto consume these; e.g. glibc's
                        // divide-by-constant idioms after a widening multiply). x19=full lo product, RDX=hi.
                        if (k == 4) { // MUL: CF=OF = (high half != 0)
                            if (w == 4)
                                e_lsr_i(22, 19, 32, 1); // x22 = product[63:32]
                            else
                                e_mov_rr(22, RDX, 1); // x22 = umulh(rax, r/m)
                            e_subi_s(23, 22, 0, 1);
                            e_cset(21, 1 /*NE*/, 1); // cf = (x22 != 0)
                        } else { // IMUL: CF=OF = (high half != sign-extension of low half)
                            if (w == 4) {
                                e_sxt(22, 19, 4);            // x22 = sign-extend product[31:0]
                                e_rrr(A_SUBS, 23, 19, 22, 1, 0); // cmp full64, sxt(low32)
                            } else {
                                e_asr_i(22, 19, 63, 1);          // x22 = sign bits of low half
                                e_rrr(A_SUBS, 23, RDX, 22, 1, 0); // cmp smulh(hi), sign(lo)
                            }
                            e_cset(21, 1 /*NE*/, 1);
                        }
                        e_mul_set_oc(21);
                        gpc = next;
                        continue;
                    }
                    if (k == 6 || k == 7) { // div / idiv
                        int rmv = rm_load(&I, next, w, &mem);
                        if (w == 8) { // 128/64: rdx may be nonzero -> exact division in C
                            e_str(rmv, 28, OFF_DIVOP);
                            emit_exit_const(next, k == 6 ? R_DIV : R_IDIV);
                            break;
                        }
                        // 32-bit: dividend = edx:eax (64-bit), 32-bit divisor (zero/sign-extend), 32-bit quotient
                        e_lsl_i(19, RDX, 32, 1);
                        e_bfi(19, RAX, 0, 32, 1); // x19 = (edx<<32)|eax
                        if (k == 6) {
                            e_uxt(22, rmv, 4);
                            emit_div_zero_check(22, gpc, 0); // #DE on /0
                            e_udiv(20, 19, 22, 1);
                        } // unsigned: zero-extend divisor
                        else {
                            e_sxt(22, rmv, 4);
                            emit_div_zero_check(22, gpc, 1);
                            e_sdiv(20, 19, 22, 1);
                        } // signed: sign-extend divisor (edx:eax already 64-bit signed)
                        e_msub(21, 20, 22, 19, 1); // rem = x19 - q*divisor
                        e_mov_rr(RAX, 20, 0);
                        e_mov_rr(RDX, 21, 0); // eax=quot, edx=rem (32-bit)
                        gpc = next;
                        continue;
                    }
                }
                report_unimpl(gpc, &I);
                break;
            }
            // ---- group4/5 (FE/FF): inc/dec, and FF: call/jmp/push (indirect) ----
            if (op == 0xFE || op == 0xFF) {
                int k = I.reg & 7, w = op == 0xFE ? 1 : I.opsize, mem;
                if (k == 0 || k == 1) { // inc / dec: set N/Z/V (OF correct), PRESERVE CF
                    int rmv = rm_load(&I, next, w, &mem); // mem -> x16 (val), x17 (EA)
                    if (I.lock && mem) {
                        // LOCK inc/dec [mem] -> atomic RMW (e.g. glibc's spinlock `lock decl`): a plain
                        // load-op-store races under contention and strands the lock with no owner -> hang.
                        // LDADDAL of +1/-1 updates memory indivisibly and yields the old value for flags.
                        e_movconst(19, k == 0 ? 1 : (uint64_t)-1); // delta (LSE size truncates to width w)
                        e_lse(LSE_LDADD, w, 19, 20, 17);           // x20 = old; [x17] += delta (acq-rel)
                        if (w >= 4) {
                            if (k == 0)
                                e_addi_s(21, 20, 1, sf);
                            else
                                e_subi_s(21, 20, 1, sf);
                            e_nzcv_save_keepC();
                            e_pf_save(21);
                        } else { // byte/word: flags from the high bits (mirror the non-atomic path)
                            int sh = 8 * (4 - w);
                            e_lsl_i(21, 20, sh, 0);
                            e_movconst(19, 1u << sh);
                            if (k == 0)
                                e_rrr(A_ADDS, 21, 21, 19, 0, 0);
                            else
                                e_rrr(A_SUBS, 21, 21, 19, 0, 0);
                            e_nzcv_save_keepC();
                            e_lsr_i(21, 21, sh, 0);
                            e_pf_save(21);
                        }
                        gpc = next;
                        continue;
                    }
                    int o = mem ? 16 : I.rm_reg;
                    if (w >= 4) {
                        if (k == 0)
                            e_addi_s(o, rmv, 1, sf);
                        else
                            e_subi_s(o, rmv, 1, sf);
                        e_nzcv_save_keepC();
                        e_pf_save(o); // x86 PF source = result low byte
                        rm_store(&I, w, o);
                    } else { // byte/word: flags from the high bits
                        int sh = 8 * (4 - w);
                        e_lsl_i(21, rmv, sh, 0);
                        e_movconst(19, 1u << sh);
                        if (k == 0)
                            e_rrr(A_ADDS, 21, 21, 19, 0, 0);
                        else
                            e_rrr(A_SUBS, 21, 21, 19, 0, 0);
                        e_nzcv_save_keepC();
                        e_lsr_i(21, 21, sh, 0);
                        e_pf_save(21); // x86 PF source = result low byte
                        rm_store(&I, w, 21);
                    }
                    gpc = next;
                    continue;
                }
                if (op == 0xFF && (k == 4 || k == 2)) { // jmp / call r/m (indirect)
                    int mem2;
                    int tgt = rm_load(&I, next, 8, &mem2);
                    if (tgt != 16) e_mov_rr(16, tgt, 1); // target -> x16
                    e_movconst(19, gpc);
                    e_str(19, 28, OFF_IBSRC); // debug
                    if (k == 2) {
                        e_subi(RSP, RSP, 8, 1);
                        e_movconst(19, next);
                        e_store(8, 19, RSP);
                    } // call: push ret
                    emit_ibranch();
                    break; // IBTC inline probe (target in x16)
                }
                if (op == 0xFF && k == 6) { // push r/m
                    int mem2;
                    int v = rm_load(&I, next, 8, &mem2);
                    if (v != 16) e_mov_rr(16, v, 1);
                    e_subi(RSP, RSP, 8, 1);
                    e_store(8, 16, RSP);
                    gpc = next;
                    continue;
                }
            }
            // ---- xchg (86/87) ----
            if (op == 0x86 || op == 0x87) {
                int w = (op & 1) ? I.opsize : 1, mem;
                if (I.is_mem) {
                    emit_ea(&I, next);
                    int sv = (w == 1) ? byte_val(&I, I.reg, 19) : I.reg; // reg->mem: handle ah/bh/ch/dh
                    // xchg with memory is IMPLICITLY atomic on x86 (no LOCK needed) -> SWP, not load+store.
                    // glibc's mutex fast-path acquires the lock with xchg, so this must be a real atomic.
                    e_lse(LSE_SWP, w, sv, 16, 17); // x16 = old [mem]; [mem] = sv (atomic swap)
                    if (w >= 4)
                        e_mov_rr(I.reg, 16, w == 8);
                    else if (w == 1)
                        byte_wb(&I, I.reg, 16);
                    else
                        e_bfi(I.reg, 16, 0, 8 * w, 1);
                } else {
                    e_mov_rr(19, I.rm_reg, sf);
                    e_mov_rr(I.rm_reg, I.reg, sf);
                    e_mov_rr(I.reg, 19, sf);
                }
                (void)mem;
                gpc = next;
                continue;
            }
            // ---- push imm (68 iz, 6A ib) ----
            if (op == 0x68 || op == 0x6A) {
                e_movconst(16, (uint64_t)I.imm);
                e_subi(RSP, RSP, 8, 1);
                e_store(8, 16, RSP);
                gpc = next;
                continue;
            }
            // ---- pop r/m (8F /0) ----
            if (op == 0x8F) {
                e_load(8, 16, RSP);
                e_addi(RSP, RSP, 8, 1);
                if (I.is_mem) {
                    emit_ea(&I, next);
                    e_store(8, 16, 17);
                } else
                    e_mov_rr(I.rm_reg, 16, 1);
                gpc = next;
                continue;
            }
            // ---- imul reg, r/m, imm (69 iz, 6B ib) ----
            if (op == 0x69 || op == 0x6B) {
                int mem;
                int rmv = rm_load(&I, next, I.opsize, &mem);
                e_movconst(19, (uint64_t)I.imm);
                e_imul2(I.reg, rmv, 19, I.opsize); // dst = r/m * imm, sets x86 CF/OF on overflow
                gpc = next;
                continue;
            }
            // ---- string ops: stos (AA/AB), movs (A4/A5), lods (AC/AD). DF assumed 0 (fwd). ----
            if (op == 0xAA || op == 0xAB || op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD) {
                int w = (op & 1) ? I.opsize : 1;
                int movs = (op == 0xA4 || op == 0xA5), lods = (op == 0xAC || op == 0xAD);
                // opt5: `rep movs`/`rep stos` -> one optimized host memcpy/memset call (bit-exact
                // with the scalar loop below). Fall back to that loop for:
                //   - NOREP=1 (kill-switch),
                //   - `lods` (its result is RAX = last element, not a bulk move),
                //   - a segment override (I.seg) or 32-bit address size (I.addr32) -- the scalar
                //     loop ignores both too, so behavior is identical, but stay conservative,
                //   - DF=1 (g_df): the host helper only copies forward; the backward case takes the
                //     per-element scalar loop below (decrementing stride) which matches x86 exactly.
                if (I.rep && !lods && !I.seg && !I.addr32 && !g_df && (w == 1 || w == 2 || w == 4 || w == 8) &&
                    !norep_disabled()) {
                    int shift = w == 1 ? 0 : w == 2 ? 1 : w == 4 ? 2 : 3;
                    emit_rep_string(movs, w, shift);
                    gpc = next;
                    continue;
                }
                uint32_t *cbz = NULL, *top = NULL;
                if (I.rep) {
                    top = (uint32_t *)g_cp;
                    cbz = (uint32_t *)g_cp;
                    emit32(0);
                } // cbz RCX,done
                // DF: forward (g_df==0) advances pointers by +w; backward (std) by -w.
                void (*e_step)(int, int, unsigned, int) = g_df ? e_subi : e_addi;
                if (movs) {
                    e_load(w, 16, RSI);
                    e_store(w, 16, RDI);
                    e_step(RSI, RSI, w, 1);
                    e_step(RDI, RDI, w, 1);
                } else if (lods) {
                    e_load(w, RAX, RSI);
                    e_step(RSI, RSI, w, 1);
                } else {
                    e_store(w, RAX, RDI);
                    e_step(RDI, RDI, w, 1);
                } // stos
                if (I.rep) {
                    e_subi(RCX, RCX, 1, 1);
                    int64_t back = (int64_t)(top - (uint32_t *)g_cp);
                    emit32(0x14000000u | ((uint32_t)back & 0x3FFFFFFu)); // b top
                    int64_t d = ((uint32_t *)g_cp - cbz);
                    *cbz = 0xB4000000u | (((uint32_t)d & 0x7FFFF) << 5) | RCX; // cbz x_rcx,done
                }
                gpc = next;
                continue;
            }
            // ---- cmps (A6/A7) / scas (AE/AF): memcmp/memchr/strlen building blocks. ----
            // The whole (possibly REP/REPE/REPNE) compare+scan is done in ONE C round-trip (like cpuid/div):
            // bit-exact RCX/RSI/RDI + ZF/SF/CF/OF end-state, fast host memcmp/memchr inside on the forward
            // path (gate NOREPCMP for the naive per-element oracle loop; DF=1 uses that loop with a
            // decrementing stride). Descriptor (width | isscas | isrepne | isrep | df) -> cpu->divop.
            if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
                int w = (op & 1) ? I.opsize : 1;
                int isscas = (op == 0xAE || op == 0xAF);
                int isrep = (I.rep || I.repne);
                uint64_t desc = (uint64_t)w | ((uint64_t)isscas << 8) | ((uint64_t)(I.repne ? 1 : 0) << 9) |
                                ((uint64_t)isrep << 10) | ((uint64_t)(g_df ? 1 : 0) << 11);
                e_movconst(16, desc);
                e_str(16, 28, OFF_DIVOP);
                emit_exit_const(next, R_REPSTR); // spills regs+flags; do_repstr() resumes at `next`
                break;                           // block ends here (helper runs, dispatcher continues)
            }
            if (op == 0xFC) {
                g_df = 0; // cld: forward string ops
                gpc = next;
                continue;
            }
            if (op == 0xFD) {
                g_df = 1; // std: backward string ops (consumed at translate time by the lowering below)
                gpc = next;
                continue;
            }
            // ---- jmp rel (E9/EB) ----
            if (op == 0xE9 || op == 0xEB) {
                uint64_t tgt = next + (uint64_t)I.imm;
                // STITCH: follow the unconditional edge inline. g_fl_pending is FL_NONE here -- the
                // top-of-loop materialized any deferred flags before this non-Jcc insn, so the membank
                // cpu->nzcv is current for the inlined successor. Skip if the target is the region head,
                // already laid in this region, an already-registered block, or a dead trap arm.
                if (STITCH_OK && tgt != start && !seen_has(seen, nseen, tgt) && !map_body(tgt) && !trap_head(tgt)) {
                    seen[nseen++] = tgt;
                    trace_blk++;
                    gpc = tgt;
                    continue;
                }
                emit_chain_exit(tgt);
                break;
            }
            // ---- call rel32 (E8) ----
            if (op == 0xE8) {
                e_subi(RSP, RSP, 8, 1);
                e_movconst(16, next);
                e_store(8, 16, RSP);
                emit_chain_exit(next + (uint64_t)I.imm);
                break;
            }
            // ---- jrcxz rel8 (E3): jump if RCX == 0 ----
            if (op == 0xE3) {
                uint64_t taken = next + (uint64_t)I.imm;
                uint32_t *patch = (uint32_t *)g_cp;
                emit32(0);             // cbz x_rcx -> taken
                emit_chain_exit(next); // RCX != 0: fall through
                int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
                *patch = 0xB4000000u | (((uint32_t)d & 0x7FFFF) << 5) | RCX; // cbz x_rcx, taken
                emit_chain_exit(taken);
                break;
            }
            // ---- jcc rel8 (70-7F) ----
            if (op >= 0x70 && op <= 0x7F) {
                int lo = op & 0xF, parity = (lo == 0xA || lo == 0xB);
                int cc;
                if (parity) {
                    cc = emit_parity_jcc_cond(lo); // jp/jnp: PF lane -> live ARM Z, branch off it
                } else {
                    cc = x86cc_to_arm(lo);
                    if (cc < 0) {
                        if (g_fl_pending) flags_materialize(); // materialize before boundary
                        report_unimpl(gpc, &I);
                        break;
                    }
                }
                uint64_t taken = next + (uint64_t)I.imm;
                // W5B tier-2: single-block self-loop (taken back-edge == block start). Detected BEFORE the
                // flag handling / superblock stitch below so the self-loop owns the back-edge; emit the
                // hotness counter (tier-1) or the folded back-edge (tier-2). g_fl_pending is still pending
                // here -- emit_selfloop_x86 does the flag handling itself. Parity already set the live Z
                // (and spilled any pending producer) above, so it skips this purely-NZCV-flag path.
                if (!parity && taken == start && !notier2x() &&
                    !loop_has_rmw_hazard((uint64_t)body, (uint64_t)g_cp)) {
                    int slot = g_tier2_build ? 0 : t2_slot(start);
                    if (g_tier2_build || slot >= 0) {
                        emit_selfloop_x86(cc, start, next, body, slot);
                        break;
                    }
                }
                if (parity) {
                    // live ARM Z already holds (PF==0) from emit_parity_jcc_cond; flags spilled there.
                } else if (g_fl_pending) {
                    // Fast path: live NZCV still holds the immediately-preceding width-4/8 producer's
                    // flags. flags_materialize() spills them to membank for the successor blocks (the
                    // exact finalizer bytes the producer deferred) AND leaves the live ARM NZCV
                    // canonical (each finalizer msr's the corrected value back) -- so we branch
                    // straight off the live flags, dropping the redundant e_nzcv_load (ldr;msr).
                    flags_materialize();
                } else {
                    e_nzcv_load();
                }
                uint64_t fall = next;
                // STITCH: lay the fall-through (`next`) inline; the taken side becomes a tiny
                // out-of-line exit reached by the INVERTED condition. cpu->nzcv was just materialized
                // above (membank canonical, live NZCV msr'd back), so the inline fall-through and the
                // out-of-line taken exit agree, and g_fl_pending == FL_NONE for the inlined successor.
                if (STITCH_OK && fall != start && !seen_has(seen, nseen, fall) && !map_body(fall) && !trap_head(fall)) {
                    int inv = (cc ^ 1) & 0xF; // not-taken -> branch over the taken exit (x86cc_to_arm is 0..13)
                    uint32_t *patch = (uint32_t *)g_cp;
                    emit32(0); // b.inv -> fall (inline)
                    emit_chain_exit(taken);
                    int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
                    *patch = 0x54000000u | (((uint32_t)d & 0x7FFFF) << 5) | (uint32_t)inv;
                    seen[nseen++] = fall;
                    trace_blk++;
                    gpc = fall;
                    continue;
                }
                uint32_t *patch = (uint32_t *)g_cp;
                emit32(0); // b.cond -> taken
                emit_chain_exit(next);
                int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
                *patch = 0x54000000u | (((uint32_t)d & 0x7FFFF) << 5) | (cc & 0xF);
                emit_chain_exit(taken);
                break;
            }
            // ---- ret (C3) / ret imm16 (C2) ----
            if (op == 0xC3 || op == 0xC2) {
                e_load(8, 16, RSP);
                e_addi(RSP, RSP, 8, 1);
                if (op == 0xC2) {
                    e_movconst(19, (uint64_t)(uint16_t)I.imm);
                    e_rrr(A_ADD, RSP, RSP, 19, 1, 0);
                }
                e_movconst(19, gpc);
                e_str(19, 28, OFF_IBSRC); // debug
                emit_ibranch();
                break; // IBTC inline probe (target in x16)
            }
            // ---- leave (C9) ----
            if (op == 0xC9) {
                e_mov_rr(RSP, RBP, 1);
                e_load(8, RBP, RSP);
                e_addi(RSP, RSP, 8, 1);
                gpc = next;
                continue;
            }
            // ---- nop (90) / xchg rAX, rN (91-97) ----
            if (op == 0x90 && !I.rep) {
                // `90` is XCHG eAX,rN — only a NOP when N==rAX. With REX.B it targets r8 (`49 90` =
                // xchg rax,r8), a REAL swap; dropping it (stale r8) is the busybox `sort` SIGSEGV (the
                // `call malloc; xchg %rax,%r8` allocator idiom). Mirror the 0x91-0x97 sibling.
                if (I.rexB) {
                    int r = I.rexB << 3; // r8
                    e_mov_rr(19, RAX, sf);
                    e_mov_rr(RAX, r, sf);
                    e_mov_rr(r, 19, sf);
                }
                gpc = next;
                continue;
            } // (F3 90 = pause -> also nop)
            if (op == 0x9B) {
                gpc = next;
                continue;
            } // fwait/wait -> nop (FPU sync)
            // sahf (9E): AH -> flags. We map SF=AH.7, ZF=AH.6, CF=AH.0 into cpu->nzcv (N/Z/C) and
            // restore PF=AH.2 into the PF lane (the consumer takes even-parity, so store NOT(PF) as the
            // source byte: a 0 byte -> even popcount -> PF=1, a 1 byte -> odd -> PF=0).
            if (op == 0x9E) {
                emit32(0x53083C00u | (RAX << 5) | 16); // ubfx w16, w_rax, #8, #8  (AH)
                emit32(0x53000000u | (16 << 5) | 17);  // ubfx w17, w16, #0, #1  (CF)
                e_lsl_i(17, 17, 29, 0);
                emit32(0x53061800u | (16 << 5) | 18); // ubfx w18, w16, #6, #1  (ZF)
                e_lsl_i(18, 18, 30, 0);
                e_rrr(A_ORR, 17, 17, 18, 0, 0);
                emit32(0x53071C00u | (16 << 5) | 18); // ubfx w18, w16, #7, #1  (SF)
                e_lsl_i(18, 18, 31, 0);
                e_rrr(A_ORR, 17, 17, 18, 0, 0);
                e_str(17, 28, OFF_NZCV);
                emit32(0x53020800u | (16 << 5) | 19); // ubfx w19, w16, #2, #1  (PF)
                e_movconst(20, 1);
                e_rrr(A_EOR, 19, 19, 20, 0, 0); // PF source byte = NOT PF (parity-even <-> PF=1)
                e_str(19, 28, OFF_PF);
                gpc = next;
                continue;
            }
            // lahf (9F): flags -> AH (SF,ZF,--,AF,--,PF,1,CF). We fill SF/ZF/CF + the always-1 bit.
            if (op == 0x9F) {
                e_ldr(16, 28, OFF_NZCV);
                emit32(0x53000000u | (31 << 16) | (31 << 10) | (16 << 5) | 17); // ubfx w17,w16,#31,#1 (N->SF)
                e_lsl_i(17, 17, 7, 0);
                emit32(0x53000000u | (30 << 16) | (30 << 10) | (16 << 5) | 18); // ubfx w18,w16,#30,#1 (Z->ZF)
                e_lsl_i(18, 18, 6, 0);
                e_rrr(A_ORR, 17, 17, 18, 0, 0);
                emit32(0x53000000u | (29 << 16) | (29 << 10) | (16 << 5) | 18); // ubfx w18,w16,#29,#1 (C->CF)
                e_rrr(A_ORR, 17, 17, 18, 0, 0);
                e_movconst(18, 2);
                e_rrr(A_ORR, 17, 17, 18, 0, 0); // bit1 reads as 1
                e_bfi(RAX, 17, 8, 8, 1);
                gpc = next;
                continue; // AH = w17
            }
            // pushfq (9C): materialize x86 RFLAGS from the flag substrate and push it. Bits assembled in
            // x17: reserved bit1=1, IF(bit9)=1 (userspace), DF(bit10)=g_df (block-local), then CF/PF/ZF/
            // SF/OF from cpu->nzcv + the PF lane. AF (bit4) is not modeled (read as 0).
            if (op == 0x9C) {
                if (g_fl_pending) flags_materialize(); // make cpu->nzcv current
                e_ldr(16, 28, OFF_NZCV);               // x16 = ARM NZCV substrate
                e_movconst(17, 0x202u | (g_df ? 0x400u : 0u));
                emit32(0x53000000u | (29 << 16) | (29 << 10) | (16 << 5) | 18); // ubfx w18,w16,#29,#1 (borrow C)
                e_movconst(19, 1);
                e_rrr(A_EOR, 18, 18, 19, 0, 0); // x86 CF = NOT stored-C (borrow convention)
                e_rrr(A_ORR, 17, 17, 18, 0, 0); // -> bit0
                e_bit_move(17, 16, 30, 6, 18);  // ZF: NZCV.Z(30) -> bit6
                e_bit_move(17, 16, 31, 7, 18);  // SF: NZCV.N(31) -> bit7
                e_bit_move(17, 16, 28, 11, 18); // OF: NZCV.V(28) -> bit11
                e_pf_compute(18);               // x18 = x86 PF (0/1); clobbers x16
                e_rrr(A_ORR, 17, 17, 18, 0, 2); // -> bit2
                e_subi(RSP, RSP, 8, 1);
                e_store(8, 17, RSP);
                gpc = next;
                continue;
            }
            // popfq (9D): pop RFLAGS and distribute back into the flag substrate (cpu->nzcv + PF lane).
            // DF (bit10) is intentionally not restored: runtime DF lives only as translate-time g_df, so a
            // dynamic DF cannot be threaded here (a pre-existing limitation -- libc brackets string ops with
            // straight-line std/cld, never with popfq).
            if (op == 0x9D) {
                e_load(8, 16, RSP); // x16 = popped RFLAGS
                e_addi(RSP, RSP, 8, 1);
                e_movconst(17, 0);
                e_bit_move(17, 16, 6, 30, 18);  // ZF(bit6) -> NZCV.Z(30)
                e_bit_move(17, 16, 7, 31, 18);  // SF(bit7) -> NZCV.N(31)
                e_bit_move(17, 16, 11, 28, 18); // OF(bit11) -> NZCV.V(28)
                emit32(0x53000000u | (0 << 16) | (0 << 10) | (16 << 5) | 18); // ubfx w18,w16,#0,#1 (CF)
                e_movconst(19, 1);
                e_rrr(A_EOR, 18, 18, 19, 0, 0);  // stored borrow-C = NOT x86 CF
                e_rrr(A_ORR, 17, 17, 18, 0, 29); // -> NZCV.C(29)
                e_str(17, 28, OFF_NZCV);
                emit32(0xD51B4200u | 17); // msr nzcv, x17  (sync live ARM flags)
                emit32(0x53000000u | (2 << 16) | (2 << 10) | (16 << 5) | 18); // ubfx w18,w16,#2,#1 (PF)
                e_movconst(19, 1);
                e_rrr(A_EOR, 18, 18, 19, 0, 0); // PF lane source byte = NOT PF (consumer takes even-parity)
                e_str(18, 28, OFF_PF);
                g_fl_pending = FL_NONE; // flags now materialized directly into cpu->nzcv
                gpc = next;
                continue;
            }
            // ===== x87 FPU (D8-DF): double-precision stack emulation =====
            if (op >= 0xD8 && op <= 0xDF) {
                int reg = I.reg & 7, rm = I.rm_reg & 7;
#define FAd(d, n, m) emit32(0x1E602800u | ((m) << 16) | ((n) << 5) | (d)) /* fadd d */
#define FSd(d, n, m) emit32(0x1E603800u | ((m) << 16) | ((n) << 5) | (d)) /* fsub d */
#define FMd(d, n, m) emit32(0x1E600800u | ((m) << 16) | ((n) << 5) | (d)) /* fmul d */
#define FDd(d, n, m) emit32(0x1E601800u | ((m) << 16) | ((n) << 5) | (d)) /* fdiv d */
// fucomi/fcomi/fucomip/fcomip set integer EFLAGS exactly like COMISD (ZF/PF/CF, unordered -> all 1), so
// use the same unordered+PF fixup (this also writes the real PF lane the setp/setnp consumers read).
#define FCMPd(n, m)                                                                                                    \
    do {                                                                                                               \
        emit32(0x1E602000u | ((m) << 16) | ((n) << 5));                                                                \
        e_nzcv_save_fcmp();                                                                                            \
    } while (0)
                if (I.is_mem) {
                    // x87 mem forms do a faulting guest load/store and the m80 forms exit to a C
                    // helper -- both can escape, so make cpu->fptop reflect the (pre-op) shadow first.
                    fp_materialize();
                    emit_ea(&I, next);
                    e_mov_rr(19, 17, 1); // x19 = EA (helpers clobber x17)
                    if (op == 0xD9) {    // f32 mem
                        if (reg == 0) {
                            e_ldr_s(16, 19);
                            emit32(0x1E22C000u | (16 << 5) | 16);
                            fp_push(16);
                        } // fld m32
                        else if (reg == 2 || reg == 3) {
                            fp_ld(16, 0);
                            emit32(0x1E624000u | (16 << 5) | 16);
                            e_str_s(16, 19);
                            if (reg == 3) fp_settop(1);
                        } // fst/fstp
                        else if (reg == 5) { /* fldcw: ignore */
                        } else if (reg == 7) {
                            e_movconst(16, 0x037f);
                            emit32(0x79000000u | (19 << 5) | 16);
                        } // fnstcw
                        else {
                            report_unimpl(gpc, &I);
                            break;
                        }
                    } else if (op == 0xDD) { // f64 mem
                        if (reg == 0) {
                            e_ldr_d(16, 19);
                            fp_push(16);
                        } // fld m64
                        else if (reg == 2 || reg == 3) {
                            fp_ld(16, 0);
                            e_str_d(16, 19);
                            if (reg == 3) fp_settop(1);
                        } // fst/fstp
                        else if (reg == 7) {
                            e_ldr(16, 28, OFF_FPSW);
                            emit32(0x79000000u | (19 << 5) | 16);
                        } // fnstsw m16
                        else {
                            report_unimpl(gpc, &I);
                            break;
                        }
                    } else if (op == 0xDB) { // i32 mem / m80
                        if (reg == 0) {
                            emit32(0xB9400000u | (19 << 5) | 16);
                            emit32(0x1E620000u | (16 << 5) | 16);
                            fp_push(16);
                        } // fild m32
                        else if (reg == 2 || reg == 3) {
                            fp_ld(16, 0);
                            emit32(0x1E780000u | (16 << 5) | 16);
                            emit32(0xB9000000u | (19 << 5) | 16);
                            if (reg == 3) fp_settop(1);
                        } // fist/fistp m32
                        else if (reg == 5) {
                            e_str(19, 28, OFF_X87EA);
                            emit_exit_const(next, R_X87FLD);
                            break;
                        } // fld m80 -> C
                        else if (reg == 7) {
                            e_str(19, 28, OFF_X87EA);
                            emit_exit_const(next, R_X87FSTP);
                            break;
                        } // fstp m80 -> C
                        else {
                            report_unimpl(gpc, &I);
                            break;
                        }
                    } else if (op == 0xDF) { // i16/i64 mem
                        if (reg == 0) {
                            emit32(0x79C00000u | (19 << 5) | 16);
                            emit32(0x1E620000u | (16 << 5) | 16);
                            fp_push(16);
                        } // fild m16 (ldrsh)
                        else if (reg == 3) {
                            fp_ld(16, 0);
                            emit32(0x1E780000u | (16 << 5) | 16);
                            emit32(0x79000000u | (19 << 5) | 16);
                            fp_settop(1);
                        } // fistp m16
                        else if (reg == 5) {
                            e_ldr(16, 19, 0);
                            emit32(0x9E620000u | (16 << 5) | 16);
                            fp_push(16);
                        } // fild m64
                        else if (reg == 7) {
                            fp_ld(16, 0);
                            emit32(0x9E780000u | (16 << 5) | 16);
                            e_str(16, 19, 0);
                            fp_settop(1);
                        } // fistp m64
                        else {
                            report_unimpl(gpc, &I);
                            break;
                        }
                    } else { // D8 (f32) / DC (f64) arith with ST0
                        if (op == 0xD8) {
                            e_ldr_s(16, 19);
                            emit32(0x1E22C000u | (16 << 5) | 16);
                        } else
                            e_ldr_d(16, 19);
                        if (reg == 2 || reg == 3) {
                            fp_ld(18, 0);
                            e_fcom_setfpsw(18, 16);
                            if (reg == 3) fp_settop(1);
                            gpc = next;
                            continue;
                        } // fcom/fcomp
                        fp_ld(18, 0);
                        if (reg == 0)
                            FAd(18, 18, 16);
                        else if (reg == 1)
                            FMd(18, 18, 16);
                        else if (reg == 4)
                            FSd(18, 18, 16);
                        else if (reg == 5)
                            FSd(18, 16, 18);
                        else if (reg == 6)
                            FDd(18, 18, 16);
                        else if (reg == 7)
                            FDd(18, 16, 18);
                        else {
                            report_unimpl(gpc, &I);
                            break;
                        }
                        fp_st(18, 0);
                    }
                    gpc = next;
                    continue;
                }
                // ---- register forms (mod=3) ----
                if (op == 0xD9) {
                    if (reg == 0) {
                        fp_ld(16, rm);
                        fp_push(16);
                    } // fld ST(i)
                    else if (reg == 1) {
                        fp_ld(16, 0);
                        fp_ld(18, rm);
                        fp_st(18, 0);
                        fp_st(16, rm);
                    } // fxch
                    else if (reg == 4 && rm == 0) {
                        fp_ld(16, 0);
                        emit32(0x1E614000u | (16 << 5) | 16);
                        fp_st(16, 0);
                    } // fchs
                    else if (reg == 4 && rm == 1) {
                        fp_ld(16, 0);
                        emit32(0x1E60C000u | (16 << 5) | 16);
                        fp_st(16, 0);
                    } // fabs
                    else if (reg == 5) { // fld const
                        static const uint64_t k[8] = {0x3FF0000000000000ull /*1*/,
                                                      0x400A934F0979A371ull /*l2t*/,
                                                      0x3FF71547652B82FEull /*l2e*/,
                                                      0x400921FB54442D18ull /*pi*/,
                                                      0x3FD34413509F79FFull /*lg2*/,
                                                      0x3FE62E42FEFA39EFull /*ln2*/,
                                                      0x0ull /*0*/,
                                                      0x0ull};
                        e_movconst(16, k[rm]);
                        e_fmov_to_d(16, 16);
                        fp_push(16);
                    } else if (reg == 7 && rm == 2) {
                        fp_ld(16, 0);
                        emit32(0x1E61C000u | (16 << 5) | 16);
                        fp_st(16, 0);
                    } // fsqrt
                    else {
                        report_unimpl(gpc, &I);
                        break;
                    }
                } else if (op == 0xD8 || op == 0xDC || op == 0xDE) { // arith ST0/ST(i) [+pop for DE]
                    fp_ld(18, 0);
                    fp_ld(16, rm);                     // v18=ST0, v16=ST(rm)
                    int dst_i = (op == 0xD8) ? 0 : rm; // D8 -> ST0; DC/DE -> ST(i)
                    if (reg == 2 || reg == 3) {
                        e_fcom_setfpsw(18, 16);
                        if (op == 0xDE && rm == 1) fp_settop(1);
                        if (reg == 3) fp_settop(1);
                        gpc = next;
                        continue;
                    } // fcom[p]/fcompp
                    int a = 18, b = 16;
                    if (op != 0xD8) {
                        a = 16;
                        b = 18;
                    } // DC/DE: dst=ST(i)=v16, other=ST0=v18
                    if (reg == 0)
                        FAd(a, a, b);
                    else if (reg == 1)
                        FMd(a, a, b);
                    else if (reg == 4) {
                        if (op == 0xD8)
                            FSd(a, a, b);
                        else
                            FSd(a, b, a);
                    } // DC/DE reverse sub
                    else if (reg == 5) {
                        if (op == 0xD8)
                            FSd(a, b, a);
                        else
                            FSd(a, a, b);
                    } else if (reg == 6) {
                        if (op == 0xD8)
                            FDd(a, a, b);
                        else
                            FDd(a, b, a);
                    } else if (reg == 7) {
                        if (op == 0xD8)
                            FDd(a, b, a);
                        else
                            FDd(a, a, b);
                    } else {
                        report_unimpl(gpc, &I);
                        break;
                    }
                    fp_st(a, dst_i);
                    if (op == 0xDE) fp_settop(1); // pop
                } else if (op == 0xDD) {
                    if (reg == 0) { /* ffree: no tag tracking -> nop */
                    } else if (reg == 2) {
                        fp_ld(16, 0);
                        fp_st(16, rm);
                    } // fst ST(i)
                    else if (reg == 3) {
                        fp_ld(16, 0);
                        fp_st(16, rm);
                        fp_settop(1);
                    } // fstp ST(i)
                    else if (reg == 4 || reg == 5) {
                        fp_ld(18, 0);
                        fp_ld(16, rm);
                        e_fcom_setfpsw(18, 16);
                        if (reg == 5) fp_settop(1);
                    } // fucom[p]
                    else {
                        report_unimpl(gpc, &I);
                        break;
                    }
                } else if (op == 0xDB) {
                    if (reg == 4 && rm == 3) {
                        e_movconst(16, 0);
                        e_str(16, 28, OFF_FPTOP);
                        if (x87opt_on()) { // anchor the translate-time shadow: top is now statically 0
                            g_fp_known = 1;
                            g_fp_top = 0;
                            g_fp_dirty = 0; // memory just written, shadow == cpu->fptop
                        }
                    } // finit -> top=0
                    else if (reg == 4) { /* fclex/etc */
                    } else if (reg == 5 || reg == 6) {
                        fp_ld(18, 0);
                        fp_ld(16, rm);
                        FCMPd(18, 16);
                    } // fucomi/fcomi
                    else {
                        report_unimpl(gpc, &I);
                        break;
                    }
                } else if (op == 0xDF) {
                    if (reg == 4 && rm == 0) {
                        e_ldr(16, 28, OFF_FPSW);
                        e_bfi(RAX, 16, 0, 16, 1);
                    } // fnstsw ax
                    else if (reg == 5 || reg == 6) {
                        fp_ld(18, 0);
                        fp_ld(16, rm);
                        FCMPd(18, 16);
                        fp_settop(1);
                    } // fucomip/fcomip
                    else {
                        report_unimpl(gpc, &I);
                        break;
                    }
                } else if (op == 0xDA) { // fcmovcc ST0,ST(i) (reg 0/1/2/3 = B/E/BE/U)
                    if (reg <= 3) {      // condition from integer EFLAGS
                        int jcc = (reg == 0) ? 2 : (reg == 1) ? 4 : (reg == 2) ? 6 : 10; // jb/je/jbe/jp
                        int armc = x86cc_to_arm(jcc);
                        e_nzcv_load();
                        fp_ld(18, 0);
                        fp_ld(16, rm); // v18=ST0, v16=ST(i)
                        emit32(0x1E600C00u | (18 << 16) | ((armc & 0xF) << 12) | (16 << 5) |
                               17); // fcsel d17, STi, ST0, cond
                        fp_st(17, 0);
                    } else if (reg == 5 && rm == 1) { // DA E9: fucompp (compare ST0,ST1; pop twice)
                        fp_ld(18, 0);
                        fp_ld(16, 1);
                        e_fcom_setfpsw(18, 16);
                        fp_settop(1);
                        fp_settop(1);
                    } else {
                        report_unimpl(gpc, &I);
                        break;
                    }
                } else {
                    report_unimpl(gpc, &I);
                    break;
                }
#undef FAd
#undef FSd
#undef FMd
#undef FDd
#undef FCMPd
                gpc = next;
                continue;
            }
            if (op == 0x90) {
                gpc = next;
                continue;
            }
            // ---- int3 (CC): software breakpoint -> #BP, a TRAP delivered as SIGTRAP. rip points PAST
            // the int3 (trap semantics). Emit a host BRK so the SIGTRAP guard runs the guest handler (or
            // default-terminates); previously this fell through to report_unimpl -> engine abort (70).
            if (op == 0xCC) {
                emit_guest_trap(next, 0xD4200000u); // brk #0 -> host SIGTRAP
                break;
            }
            if (op >= 0x91 && op <= 0x97) {
                int r = (op - 0x90) | (I.rexB << 3);
                e_mov_rr(19, RAX, sf);
                e_mov_rr(RAX, r, sf);
                e_mov_rr(r, 19, sf);
                gpc = next;
                continue;
            }
            // ---- cbw/cwde/cdqe (98) and cwd/cdq/cqo (99) ----
            if (op == 0x98) {
                if (sf)
                    e_sxt(RAX, RAX, 4); // cdqe: rax = sext32(eax)
                else if (I.p66)
                    emit32(0x13001C00u | (RAX << 5) | RAX); // cbw: ax = sext8(al) (sxtb Wd,Wn)
                else
                    emit32(0x13003C00u | (RAX << 5) | RAX); // cwde: eax = sext16(ax) (sxth Wd,Wn)
                gpc = next;
                continue;
            }
            if (op == 0x99) {
                if (sf)
                    e_asr_i(RDX, RAX, 63, 1); // cqo: rdx = rax>>63 (arith)
                else if (I.p66) {
                    e_asr_i(19, RAX, 15, 0);
                    e_bfi(RDX, 19, 0, 16, 1);
                } // cwd: dx=sign(ax)
                else
                    e_asr_i(RDX, RAX, 31, 0); // cdq: edx = eax>>31 (arith)
                gpc = next;
                continue;
            }
        } else {
            // ===== two-byte (0F xx) =====
            if (op == 0x05) {
                if (g_fastsys) { // S1: inline time fast path (no service round-trip for clock_gettime/gettimeofday)
                    emit_fast_syscall(next);
                    // The inline-served path falls through here; end the block with a chained branch to
                    // `next` (regs stay live, no spill) instead of decoding inline -- decoding past the
                    // syscall would run the decoder off the end of guest .text (SIGBUS). The slow path
                    // inside emit_fast_syscall already ended the block via emit_exit_const(next,R_SYSCALL).
                    emit_chain_exit(next);
                    break;
                }
                emit_exit_const(next, R_SYSCALL);
                break;
            } // syscall
            if (op == 0x0B) {
                emit_sigill(gpc);
                break;
            } // ud2 -> guest SIGILL (terminate like real Linux), not an engine abort
            // ===== SSE / SSE2 (guest xmm0..15 == host v0..v15) =====
            // mandatory prefix selects the variant: 66=packed-int/double, F3=scalar-single,
            // F2=scalar-double, none=packed-single. reg/rm fields index xmm directly.
            {
                int handled = 1, mem;
                int vd = I.reg, vm = I.rm_reg;
                if (op == 0x6E) { // movd/movq xmm, r/m  (66)
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        if (I.rexW)
                            e_ldr_d(vd, 17);
                        else
                            e_ldr_s(vd, 17);
                    } else {
                        if (I.rexW)
                            e_fmov_to_d(vd, I.rm_reg);
                        else
                            e_fmov_to_s(vd, I.rm_reg);
                    }
                } else if (op == 0x7E && I.rep) { // F3 0F 7E: movq xmm, xmm/m64
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_d(vd, 17);
                    } else
                        e_vmov8(vd, vm);
                } else if (op == 0x7E) { // 66 0F 7E: movd/movq r/m, xmm
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        if (I.rexW)
                            e_str_d(vd, 17);
                        else
                            e_str_s(vd, 17);
                    } else {
                        if (I.rexW)
                            e_fmov_from_d(I.rm_reg, vd);
                        else
                            e_fmov_from_s(I.rm_reg, vd);
                    }
                } else if (op == 0xD6) { // 66 0F D6: movq xmm/m64, xmm
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_str_d(vd, 17);
                    } else
                        e_vmov8(vm, vd);
                } else if (op == 0x6F || op == 0x28 || (op == 0x10 && !I.rep && !I.repne)) { // load 128 -> xmm
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(vd, 17, 0);
                    } else
                        e_vmov(vd, vm);
                } else if (op == 0x7F || op == 0x29 || (op == 0x11 && !I.rep && !I.repne)) { // store xmm -> 128
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_str_q(vd, 17, 0);
                    } else
                        e_vmov(vm, vd);
                } else if ((op == 0x10 || op == 0x11) && I.rep) { // movss (32-bit)
                    int st = (op == 0x11);
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        if (st)
                            e_str_s(vd, 17);
                        else
                            e_ldr_s(vd, 17);
                    } else {
                        if (st)
                            emit32(0x6E040400u | (vd << 5) | vm);
                        else
                            emit32(0x6E040400u | (vm << 5) | vd);
                    } // ins .s[0]
                } else if ((op == 0x10 || op == 0x11) && I.repne) { // movsd (64-bit)
                    int st = (op == 0x11);
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        if (st)
                            e_str_d(vd, 17);
                        else
                            e_ldr_d(vd, 17);
                    } else {
                        if (st)
                            emit32(0x6E080400u | (vd << 5) | vm);
                        else
                            emit32(0x6E080400u | (vm << 5) | vd);
                    } // ins .d[0]
                } else if (op == 0x12 || op == 0x16) { // movlps/movhps (load) or movhlps/movlhps (reg)
                    int lane = (op == 0x16) ? 1 : 0;   // 12->low lane(d[0]), 16->high lane(d[1])
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_d(16, 17);
                        e_ins_d(vd, lane, 16, 0);
                    } else {
                        int srclane = (op == 0x12) ? 1 : 0; // movhlps: d[0]<-src d[1]; movlhps: d[1]<-src d[0]
                        e_ins_d(vd, lane, vm, srclane);
                    }
                } else if (op == 0x13 || op == 0x17) { // movlps/movhps store
                    int lane = (op == 0x17) ? 1 : 0;
                    emit_ea(&I, next);
                    e_ins_d(16, 0, vd, lane);
                    e_str_d(16, 17);
                } else if (op == 0x54 || op == 0x55 || op == 0x56 ||
                           op == 0x57) { // andps/andnps/orps/xorps (FP bitwise)
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    if (op == 0x54)
                        e_v3(0x4E201C00u, vd, vd, s); // and
                    else if (op == 0x55)
                        e_v3(0x4E601C00u, vd, s, vd); // andn: ~vd & s -> bic vd,s,vd
                    else if (op == 0x56)
                        e_v3(0x4EA01C00u, vd, vd, s); // or
                    else
                        e_v3(0x6E201C00u, vd, vd, s); // xor
                } else if (op == 0xC6 && I.p66) {     // shufpd: 64-bit lanes (d[0]<-dst, d[1]<-src)
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    unsigned im = (unsigned)I.imm;
                    e_vmov(18, vd);
                    e_ins_d(17, 0, 18, im & 1);
                    e_ins_d(17, 1, s, (im >> 1) & 1);
                    e_vmov(vd, 17);
                } else if (op == 0xC6) { // shufps xmm,xmm/m,imm8 (lanes 0,1 from dst; 2,3 from src)
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    unsigned im = (unsigned)I.imm;
                    e_vmov(18, vd);
                    e_ins_s(17, 0, 18, im & 3);
                    e_ins_s(17, 1, 18, (im >> 2) & 3);
                    e_ins_s(17, 2, s, (im >> 4) & 3);
                    e_ins_s(17, 3, s, (im >> 6) & 3);
                    e_vmov(vd, 17);
                } else if (op == 0x71 || op == 0x72 || op == 0x73) { // psrl/psra/psll w/d/q by imm8; psrldq/pslldq
                    int sub = I.reg & 7,
                        esz = op == 0x71   ? 16
                              : op == 0x72 ? 32
                                           : 64,
                        sh = (int)(I.imm & 0xff), x = I.rm_reg;
                    if (sub == 2)
                        e_vshr_imm(x, x, esz, sh, 0); // psrl
                    else if (sub == 4)
                        e_vshr_imm(x, x, esz, sh, 1); // psra
                    else if (sub == 6)
                        e_vshl_imm(x, x, esz, sh); // psll
                    else if (op == 0x73 && sub == 3) {
                        e_v3(0x6E201C00u, 18, 18, 18);
                        e_ext(x, x, 18, sh & 0xF);
                    } // psrldq
                    else if (op == 0x73 && sub == 7) {
                        e_v3(0x6E201C00u, 18, 18, 18);
                        e_ext(x, 18, x, (16 - (sh & 0xF)) & 0xF);
                    } // pslldq
                    else {
                        report_unimpl(gpc, &I);
                        break;
                    }
                } else if (op == 0x70 && I.p66) { // pshufd xmm, xmm/m, imm8
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    unsigned im = (unsigned)I.imm;
                    for (int i = 0; i < 4; i++)
                        e_ins_s(17, i, s, (im >> (2 * i)) & 3); // build in v17
                    e_vmov(vd, 17);
                } else if (op == 0x70 && (I.rep || I.repne)) { // pshufhw(F3=high) / pshuflw(F2=low): shuffle 4 words
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    unsigned im = (unsigned)I.imm;
                    int hi = I.rep; // F3 shuffles the HIGH 4 words, F2 the LOW 4
                    e_vmov(17, s);  // v17 = src (the un-shuffled half is preserved)
                    for (int i = 0; i < 4; i++) {
                        int dlane = hi ? 4 + i : i;
                        int slane = (hi ? 4 : 0) + (int)((im >> (2 * i)) & 3);
                        // INS v17.H[dlane], s.H[slane]
                        emit32(0x6E000400u | ((((unsigned)dlane << 2) | 2u) << 16) | (((unsigned)slane << 1) << 11) |
                               (s << 5) | 17);
                    }
                    e_vmov(vd, 17);
                } else if (op == 0xEF) { // pxor
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    e_v3(0x6E201C00u, vd, vd, s);
                } else if (op == 0xDB || op == 0xEB || op == 0xDF) { // pand / por / pandn
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    if (op == 0xDB)
                        e_v3(0x4E201C00u, vd, vd, s);
                    else if (op == 0xEB)
                        e_v3(0x4EA01C00u, vd, vd, s);
                    else
                        e_v3(0x4E601C00u, vd, s, vd);                // pandn: vd = ~vd & s  -> BIC vd, s, vd
                } else if (op == 0x74 || op == 0x75 || op == 0x76) { // pcmpeqb/w/d
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    uint32_t b = op == 0x74 ? 0x6E208C00u : op == 0x75 ? 0x6E608C00u : 0x6EA08C00u;
                    e_v3(b, vd, vd, s);
                } else if (op == 0x64 || op == 0x65 || op == 0x66) { // pcmpgtb/w/d -> CMGT (signed)
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    uint32_t b = op == 0x64 ? 0x4E203400u : op == 0x65 ? 0x4E603400u : 0x4EA03400u;
                    e_v3(b, vd, vd, s);
                } else if (op == 0xDE || op == 0xDA || op == 0xEE || op == 0xEA) { // pmaxub/pminub/pmaxsw/pminsw
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    uint32_t b = op == 0xDE   ? 0x6E206400u  // pmaxub -> UMAX  .16B (lane-wise, NOT UMAXP)
                                 : op == 0xDA ? 0x6E206C00u  // pminub -> UMIN  .16B (lane-wise, NOT UMINP)
                                 : op == 0xEE ? 0x4E606400u  // pmaxsw -> SMAX  .8H  (lane-wise, NOT SMAXP)
                                              : 0x4E606C00u; // pminsw -> SMIN  .8H  (lane-wise, NOT SMINP)
                    e_v3(b, vd, vd, s);
                } else if (op == 0xFC || op == 0xFD || op == 0xFE || op == 0xD4) { // paddb/w/d/q
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    uint32_t b = op == 0xFC   ? 0x4E208400u
                                 : op == 0xFD ? 0x4E608400u
                                 : op == 0xFE ? 0x4EA08400u
                                              : 0x4EE08400u;
                    e_v3(b, vd, vd, s);
                } else if (op == 0xF8 || op == 0xF9 || op == 0xFA || op == 0xFB) { // psubb/w/d/q
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    uint32_t b = op == 0xF8   ? 0x6E208400u
                                 : op == 0xF9 ? 0x6E608400u
                                 : op == 0xFA ? 0x6EA08400u
                                              : 0x6EE08400u;
                    e_v3(b, vd, vd, s);
                } else if (op == 0xDC || op == 0xDD || op == 0xEC || op == 0xED || op == 0xD8 || op == 0xD9 ||
                           op == 0xE8 || op == 0xE9) { // saturating add/sub: paddus/padds/psubus/psubs b/w
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    uint32_t b = op == 0xDC   ? 0x6E200C00u  // paddusb -> UQADD .16b
                                 : op == 0xDD ? 0x6E600C00u  // paddusw -> UQADD .8h
                                 : op == 0xEC ? 0x4E200C00u  // paddsb  -> SQADD .16b
                                 : op == 0xED ? 0x4E600C00u  // paddsw  -> SQADD .8h
                                 : op == 0xD8 ? 0x6E202C00u  // psubusb -> UQSUB .16b
                                 : op == 0xD9 ? 0x6E602C00u  // psubusw -> UQSUB .8h
                                 : op == 0xE8 ? 0x4E202C00u  // psubsb  -> SQSUB .16b
                                              : 0x4E602C00u; // psubsw  -> SQSUB .8h
                    e_v3(b, vd, vd, s);
                } else if (op == 0xE0 || op == 0xE3) { // pavgb/pavgw: unsigned rounding average -> URHADD
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    e_v3(op == 0xE0 ? 0x6E201400u : 0x6E601400u, vd, vd, s); // .16b : .8h
                } else if (op == 0xD5) { // pmullw: packed signed 16x16 -> low 16 bits -> MUL .8h
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    e_v3(0x4E609C00u, vd, vd, s);
                } else if (op == 0xE5 || op == 0xE4) { // pmulhw(signed)/pmulhuw(unsigned): 16x16 -> high 16 bits
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    // widen-multiply the low/high 4 lanes to 32-bit products, then UZP2 picks the high 16 of each.
                    uint32_t lo = op == 0xE5 ? 0x0E60C000u : 0x2E60C000u; // SMULL/UMULL  v18.4s, vd.4h, s.4h
                    uint32_t hi = op == 0xE5 ? 0x4E60C000u : 0x6E60C000u; // SMULL2/UMULL2 v19.4s, vd.8h, s.8h
                    emit32(lo | (s << 16) | (vd << 5) | 18);
                    emit32(hi | (s << 16) | (vd << 5) | 19);
                    emit32(0x4E405800u | (19 << 16) | (18 << 5) | vd); // uzp2 vd.8h, v18.8h, v19.8h
                } else if (op == 0xF5) { // pmaddwd: signed 16x16, add adjacent pairs -> 32-bit lanes
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    emit32(0x0E60C000u | (s << 16) | (vd << 5) | 18); // smull  v18.4s, vd.4h, s.4h
                    emit32(0x4E60C000u | (s << 16) | (vd << 5) | 19); // smull2 v19.4s, vd.8h, s.8h
                    emit32(0x4EA0BC00u | (19 << 16) | (18 << 5) | vd); // addp  vd.4s, v18.4s, v19.4s
                } else if (op == 0xF1 || op == 0xF2 || op == 0xF3 || op == 0xD1 || op == 0xD2 || op == 0xD3 ||
                           op == 0xE1 || op == 0xE2) { // psll/psrl/psra w/d/q by xmm/m (variable count)
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    int left = (op == 0xF1 || op == 0xF2 || op == 0xF3);
                    int arith = (op == 0xE1 || op == 0xE2);
                    int esize = (op == 0xF1 || op == 0xD1 || op == 0xE1)   ? 16
                                : (op == 0xF2 || op == 0xD2 || op == 0xE2) ? 32
                                                                          : 64;
                    e_sse_var_shift(vd, vd, s, esize, left, arith);
                } else if (op == 0x14 || op == 0x15) { // unpckl/hp{s,d}: interleave float lanes -> ZIP1/ZIP2
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    int hi = (op == 0x15);  // unpckh* -> ZIP2
                    int sz = I.p66 ? 3 : 2; // 66=pd (64-bit lanes, .2d); none=ps (32-bit lanes, .4s)
                    uint32_t b = (hi ? 0x4E007800u : 0x4E003800u) | ((uint32_t)sz << 22);
                    e_v3(b, vd, vd, s);
                } else if (op == 0xE6 && I.rep) { // cvtdq2pd (F3): low 2 packed s32 -> 2 packed f64
                    int s = vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_d(16, 17);
                        s = 16;
                    }
                    emit32(0x0F20A400u | (s << 5) | 16);  // SXTL v16.2d, vs.2s  (sign-extend the 2 int32)
                    emit32(0x4E61D800u | (16 << 5) | vd); // SCVTF vd.2d, v16.2d (int64 -> double)
                } else if (op == 0x60 || op == 0x61 || op == 0x62 || op == 0x6C || op == 0x68 || op == 0x69 ||
                           op == 0x6A || op == 0x6D) { // punpck l/h bw/wd/dq/qdq -> ZIP1/ZIP2
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    int hi = (op == 0x68 || op == 0x69 || op == 0x6A || op == 0x6D); // punpckh*; 0x6C(lqdq) is LOW
                    int sz = (op == 0x60 || op == 0x68)   ? 0
                             : (op == 0x61 || op == 0x69) ? 1
                             : (op == 0x62 || op == 0x6A) ? 2
                                                          : 3;
                    uint32_t b = (hi ? 0x4E007800u : 0x4E003800u) | ((uint32_t)sz << 22);
                    e_v3(b, vd, vd, s);
                } else if (op == 0x67 || op == 0x63 || op == 0x6B) {
                    // pack with saturation: 0x67 PACKUSWB (16->u8), 0x63 PACKSSWB (16->s8),
                    // 0x6B PACKSSDW (32->s16). dst.low half from dst's lanes, dst.high half from src's.
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    uint32_t sz = (op == 0x6B) ? 1u : 0u;     // source element: 0x6B = 16-bit, else 8-bit dest
                    uint32_t lo = (op == 0x67) ? 0x2E212800u  // SQXTUN  (signed->unsigned narrow)
                                               : 0x0E214800u; // SQXTN   (signed->signed narrow)
                    uint32_t hi = (op == 0x67) ? 0x6E212800u : 0x4E214800u; // ...2 (Q=1, high half)
                    emit32(lo | (sz << 22) | (vd << 5) | 17);               // narrow dst's lanes -> v17 low
                    emit32(hi | (sz << 22) | (s << 5) | 17);                // narrow src's lanes -> v17 high
                    e_vmov(vd, 17);
                } else if (op == 0xD7 && !nosseopt()) { // pmovmskb -> NEON (W3b SSE-SIMD idiom upgrade)
                    // Gather the 16 byte-MSBs into the low 16 bits of I.reg with a cascading
                    // shift-accumulate that needs NO memory round-trip and NO constant load
                    // (the proven sse2neon _mm_movemask_epi8 sequence). 7 host insns vs ~51.
                    //   ushr v17.16b, vm.16b, #7      ; t[i] = byte[i] MSB  (bit0 of each byte)
                    //   usra v17.8h,  v17.8h,  #7     ; pack 2 bits -> low byte of each halfword
                    //   usra v17.4s,  v17.4s,  #14    ; pack 4 bits -> low byte of each word
                    //   usra v17.2d,  v17.2d,  #28    ; pack 8 bits -> byte[0] and byte[8]
                    //   umov w16,   v17.b[0]          ; mask bits 0..7
                    //   umov wREG,  v17.b[8]          ; mask bits 8..15
                    //   orr  wREG,  w16, wREG, lsl #8 ; combine (W-form zero-extends to 64)
                    g_pmovmskb_n++;
                    e_vshr_imm(17, vm, 8, 7, 0);                           // ushr v17.16b, vm.16b, #7
                    emit32(0x6F001400u | (25u << 16) | (17 << 5) | 17);    // usra v17.8h, v17.8h, #7
                    emit32(0x6F001400u | (50u << 16) | (17 << 5) | 17);    // usra v17.4s, v17.4s, #14
                    emit32(0x6F001400u | (100u << 16) | (17 << 5) | 17);   // usra v17.2d, v17.2d, #28
                    emit32(0x0E003C00u | (1u << 16) | (17 << 5) | 16);     // umov w16, v17.b[0]
                    emit32(0x0E003C00u | (17u << 16) | (17 << 5) | I.reg); // umov wREG, v17.b[8]
                    e_rrr(A_ORR, I.reg, 16, I.reg, 0, 8);                  // orr wREG, w16, wREG, lsl #8
                } else if (op == 0xD7) {       // pmovmskb scalar fallback (NOSSEOPT=1 -> baseline codegen)
                    e_str_q(vm, 28, OFF_MM);   // spill the 16 bytes to scratch
                    e_addi(17, 28, OFF_MM, 1); // x17 = &scratch
                    e_movz(I.reg, 0, 0);       // result = 0
                    for (int i = 0; i < 16; i++) {
                        emit32(0x39400000u | ((unsigned)i << 10) | (17 << 5) | 16); // ldrb w16,[x17,#i]
                        emit32(0x53071C00u | (16 << 5) | 16);                       // ubfx w16,w16,#7,#1
                        emit32(0x2A000000u | (16 << 16) | ((unsigned)i << 10) | (I.reg << 5) |
                               I.reg); // orr reg,reg,w16,lsl#i
                    }
                } else if (op == 0x2A) { // cvtsi2sd/ss: int r/m -> xmm (F2=double,F3=single)
                    int src;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_load(I.rexW ? 8 : 4, 16, 17);
                        src = 16;
                    } else
                        src = I.rm_reg;
                    emit32(0x1E220000u | (I.rexW ? 0x80000000u : 0) | (I.repne ? 0x00400000u : 0) | (src << 5) |
                           vd);                        // scvtf vd,src
                } else if (op == 0x2C || op == 0x2D) { // cvttsd2si(2C trunc)/cvtsd2si(2D round): xmm/m -> GPR
                    int s = vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        if (I.repne)
                            e_ldr_d(16, 17);
                        else
                            e_ldr_s(16, 17);
                        s = 16;
                    }
                    if (op == 0x2D) { // cvtsd2si: honor MXCSR.RC -> round to integral (FRINTI uses FPCR.RMode)...
                        uint32_t frinti = I.repne ? 0x1E67C000u : 0x1E27C000u; // double : single
                        emit32(frinti | (s << 5) | 18);                        // frinti d18, ds
                        s = 18;                                                // ...then FCVTZS the integral value (exact)
                    }
                    // FCVTZS (toward zero): exact truncation for 0x2C; for 0x2D the FRINTI value is already integral.
                    emit32(0x1E380000u | (I.rexW ? 0x80000000u : 0) | (I.repne ? 0x00400000u : 0) | (s << 5) | I.reg);
                } else if (op == 0x58 || op == 0x59 || op == 0x5C || op == 0x5E || op == 0x5D || op == 0x5F ||
                           op == 0x51) {
                    // add/mul/sub/div/min/max/sqrt. Prefix selects width: F2=scalar double, F3=scalar
                    // single, 66=PACKED double (.2d), none=PACKED single (.4s).
                    int packed = !I.repne && !I.rep;
                    int s = vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        if (packed)
                            e_ldr_q(16, 17, 0);
                        else if (I.repne)
                            e_ldr_d(16, 17);
                        else
                            e_ldr_s(16, 17);
                        s = 16;
                    }
                    if (packed) { // vector FP: 66 -> .2d (sz bit), none -> .4s
                        uint32_t d = I.p66 ? 0x00400000u : 0;
                        uint32_t b = op == 0x58   ? 0x4E20D400u  // FADD
                                     : op == 0x59 ? 0x6E20DC00u  // FMUL
                                     : op == 0x5C ? 0x4EA0D400u  // FSUB
                                     : op == 0x5E ? 0x6E20FC00u  // FDIV
                                     : op == 0x5D ? 0x4EA0F400u  // FMIN
                                     : op == 0x5F ? 0x4E20F400u  // FMAX
                                                  : 0x6EA1F800u; // FSQRT (2-reg)
                        if (op == 0x51)
                            emit32(b | d | (s << 5) | vd); // FSQRT vd.T, s.T
                        else
                            emit32(b | d | (s << 16) | (vd << 5) | vd); // op vd.T, vd.T, s.T
                    } else {                                            // scalar FP: F2=double, F3=single
                        uint32_t ty = I.repne ? 0x00400000u : 0;
                        uint32_t b = op == 0x58   ? 0x1E202800u
                                     : op == 0x59 ? 0x1E200800u
                                     : op == 0x5C ? 0x1E203800u
                                     : op == 0x5E ? 0x1E201800u
                                     : op == 0x5D ? 0x1E205800u
                                     : op == 0x5F ? 0x1E204800u
                                                  : 0x1E21C000u;
                        if (op == 0x51)
                            emit32(b | ty | (s << 5) | vd); // FSQRT sd/ss, s
                        else
                            emit32(b | ty | (s << 16) | (vd << 5) | vd); // FADD/.../FMAX sd/ss
                    }
                } else if (op == 0x5A) { // cvtsd2ss(F2) / cvtss2sd(F3)
                    int s = vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        if (I.repne)
                            e_ldr_d(16, 17);
                        else
                            e_ldr_s(16, 17);
                        s = 16;
                    }
                    if (I.repne)
                        emit32(0x1E624000u | (s << 5) | vd); // FCVT Sd, Dn (double->single)
                    else
                        emit32(0x1E22C000u | (s << 5) | vd); // FCVT Dd, Sn (single->double)
                } else if (op == 0xC4) { // pinsrw: insert low 16 bits of r/m16 into xmm H-lane (imm8 & 7)
                    int lane = (int)I.imm & 7;
                    int src;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_load(2, 16, 17); // w16 = [addr] (16-bit)
                        src = 16;
                    } else {
                        src = I.rm_reg; // guest GPR mapped to host reg
                    }
                    // INS vd.H[lane], Wsrc  (imm5 = lane<<2 | 0b10 selects H)
                    emit32(0x4E001C00u | ((((unsigned)lane << 2) | 2u) << 16) | (src << 5) | vd);
                } else if (op == 0xC5) { // pextrw: extract xmm H-lane (imm8 & 7) -> r32, zero-extended (reg src only)
                    int lane = (int)I.imm & 7;
                    // UMOV Wreg, Vm.H[lane]  (imm5 = lane<<2 | 0b10 selects H; zero-extends into the GPR)
                    emit32(0x0E003C00u | ((((unsigned)lane << 2) | 2u) << 16) | (vm << 5) | I.reg);
                } else if (op == 0xC2) { // cmpps/pd/ss/sd: FP compare with predicate imm -> all-1s/0 mask
                    int packed = !I.repne && !I.rep;
                    int s = vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        if (packed)
                            e_ldr_q(16, 17, 0);
                        else if (I.repne)
                            e_ldr_d(16, 17);
                        else
                            e_ldr_s(16, 17);
                        s = 16;
                    }
                    int pred = (int)I.imm & 7;
                    // sz bit (bit22): packed 66 / scalar F2 -> double, else single
                    uint32_t szb = (packed ? I.p66 : I.repne) ? 0x00400000u : 0;
                    uint32_t EQ = (packed ? 0x4E20E400u : 0x5E20E400u) | szb; // FCMEQ
                    uint32_t GE = (packed ? 0x6E20E400u : 0x7E20E400u) | szb; // FCMGE
                    uint32_t GT = (packed ? 0x6EA0E400u : 0x7EA0E400u) | szb; // FCMGT
                    uint32_t ANDb = packed ? 0x4E201C00u : 0x0E201C00u;       // AND Vd.16b/8b
                    uint32_t NOTb = packed ? 0x6E205800u : 0x2E205800u;       // NOT (MVN) Vd.16b/8b
                    if (pred == 3 || pred == 7) {                             // UNORD/ORD: ordered(a)&ordered(b)
                        emit32(EQ | (vd << 16) | (vd << 5) | 17);             // v17 = a==a (ordered a)
                        emit32(EQ | (s << 16) | (s << 5) | vd);               // vd  = b==b (ordered b)
                        emit32(ANDb | (17 << 16) | (vd << 5) | vd);           // vd  = ORD
                        if (pred == 3) emit32(NOTb | (vd << 5) | vd);         // UNORD = ~ORD
                    } else {
                        int swap = (pred == 1 || pred == 2); // LT/LE: a<b == b>a -> swap
                        int n = swap ? s : vd, m = swap ? vd : s;
                        uint32_t fc = (pred == 0 || pred == 4) ? EQ : (pred == 1 || pred == 6) ? GT : GE;
                        emit32(fc | (m << 16) | (n << 5) | vd);       // FCMxx vd, n, m
                        if (pred == 4) emit32(NOTb | (vd << 5) | vd); // NEQ = ~EQ
                    }
                } else if (op == 0x2E || op == 0x2F) { // ucomisd/comisd (66=double, none=single) -> FCMP + flags
                    int s = vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        if (I.p66)
                            e_ldr_d(16, 17);
                        else
                            e_ldr_s(16, 17);
                        s = 16;
                    }
                    emit32((I.p66 ? 0x1E602000u : 0x1E202000u) | (s << 16) | (vd << 5)); // FCMP Dvd, Ds  (Rd=0)
                    e_nzcv_save_fcmp();  // unordered fixup: x86 ZF=PF=CF=1, SF=0 (ARM FCMP gives N0 Z0 C1 V1)
                } else if (op == 0xF4) { // pmuludq: vd.u64[i] = (u32)vd.even32[i] * (u32)src.even32[i]
                    // W3b: was UNIMPL -> blocked glibc strchr/strrchr (byte-broadcast via pmuludq).
                    // Gather the even (0,2) 32-bit lanes of each operand into the low 2 lanes (UZP1),
                    // then widening multiply -> two 64-bit products. Bit-exact, 3 NEON insns.
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    emit32(0x4E801800u | (vd << 16) | (vd << 5) | 17); // uzp1 v17.4s, vd.4s, vd.4s -> [d0,d2,..]
                    emit32(0x4E801800u | (s << 16) | (s << 5) | 18);   // uzp1 v18.4s, s.4s,  s.4s  -> [s0,s2,..]
                    emit32(0x2EA0C000u | (18 << 16) | (17 << 5) | vd); // umull vd.2d, v17.2s, v18.2s
                } else if (op == 0x50) {                               // movmskps(NP)/movmskpd(66): pack sign bits -> GPR
                    if (I.p66) {                                       // 2 doubles -> low 2 bits
                        e_vshr_imm(17, vm, 64, 63, 0);                 // ushr v17.2d, vm.2d, #63
                        emit32(0x4E003C00u | ((0u * 16 + 8) << 16) | (17 << 5) | I.reg); // umov xREG, v17.d[0]
                        emit32(0x4E003C00u | ((1u * 16 + 8) << 16) | (17 << 5) | 19);    // umov x19, v17.d[1]
                        e_rrr(A_ORR, I.reg, I.reg, 19, 1, 1);          // orr REG, REG, x19, lsl#1
                    } else {                                           // 4 floats -> low 4 bits
                        e_vshr_imm(17, vm, 32, 31, 0);                 // ushr v17.4s, vm.4s, #31
                        emit32(0x0E003C00u | ((0u * 8 + 4) << 16) | (17 << 5) | I.reg); // umov wREG, v17.s[0]
                        for (int i = 1; i < 4; i++) {
                            emit32(0x0E003C00u | (((unsigned)i * 8 + 4) << 16) | (17 << 5) | 19); // umov w19, v17.s[i]
                            e_rrr(A_ORR, I.reg, I.reg, 19, 0, i); // orr wREG, wREG, w19, lsl#i
                        }
                    }
                } else if (op == 0x5B) { // cvtdq2ps(NP)/cvtps2dq(66)/cvttps2dq(F3): packed 4-lane int<->float
                    int s = vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                        s = 16;
                    }
                    if (I.rep)
                        emit32(0x4EA1B800u | (s << 5) | vd); // F3: cvttps2dq -> FCVTZS .4S (truncate)
                    else if (I.p66)
                        emit32(0x4E21A800u | (s << 5) | vd); // 66: cvtps2dq  -> FCVTNS .4S (round to nearest)
                    else
                        emit32(0x4E21D800u | (s << 5) | vd); // NP: cvtdq2ps  -> SCVTF  .4S (s32->f32)
                } else if (op == 0xF6) {                     // psadbw (66): sum of abs byte diffs per 64-bit half
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    emit32(0x6E207400u | (s << 16) | (vd << 5) | 17); // uabd   v17.16b, vd.16b, s.16b
                    emit32(0x6E202800u | (17 << 5) | 17);             // uaddlp v17.8h,  v17.16b
                    emit32(0x6E602800u | (17 << 5) | 17);             // uaddlp v17.4s,  v17.8h
                    emit32(0x6EA02800u | (17 << 5) | 17);             // uaddlp v17.2d,  v17.4s
                    e_vmov(vd, 17);
                } else if (op == 0xE7 && I.p66) { // movntdq (66): non-temporal store xmm -> m128
                    emit_ea(&I, next);
                    e_str_q(vd, 17, 0);
                } else
                    handled = 0;
                if (handled) {
                    gpc = next;
                    continue;
                }
            }
            if (op == 0xA2) {
                emit_exit_const(next, R_CPUID);
                break;
            } // cpuid -> dispatcher helper
            if (op == 0x31) {             // rdtsc: edx:eax = cntvct
                emit32(0xD53BE040u | 16); // mrs x16, cntvct_el0
                e_mov_rr(RAX, 16, 0);
                e_lsr_i(RDX, 16, 32, 1);
                gpc = next;
                continue;
            }
            if (op == 0x01 && I.has_modrm && I.modrm == 0xF9) { // rdtscp: edx:eax = cntvct, ecx = TSC_AUX (0)
                emit32(0xD53BE040u | 16);                       // mrs x16, cntvct_el0
                e_mov_rr(RAX, 16, 0);
                e_lsr_i(RDX, 16, 32, 1);
                e_movz(RCX, 0, 0); // TSC_AUX = 0
                gpc = next;
                continue;
            }
            if (op == 0x01 && I.has_modrm && I.modrm == 0xD0) { // xgetbv (ecx=0): XCR0 = x87+SSE (no AVX)
                e_movz(RAX, 3, 0);
                e_movz(RDX, 0, 0);
                gpc = next;
                continue;
            }
            if (op == 0x01 && I.has_modrm && I.modrm == 0xD5) { // xend (TSX): no transaction -> NOP
                gpc = next;
                continue;
            }
            if (op == 0xC3) { // movnti: non-temporal store r32/r64 -> m
                emit_ea(&I, next);
                e_store(I.opsize, I.reg, 17);
                gpc = next;
                continue;
            }
            if (op == 0xC7 && (I.reg & 7) == 1 && I.is_mem) { // cmpxchg16b: REX.W 0F C7 /1 (128-bit compare+swap)
                // Non-atomic emulation (single 128-bit CAS): correct for the in-process model. Compares
                // RDX:RAX with [m]; on equal stores RCX:RBX and sets ZF=1, else loads [m] into RDX:RAX, ZF=0.
                emit_ea(&I, next);            // x17 = EA
                e_load_uoff(8, 19, 17, 0);    // x19 = lo
                e_load_uoff(8, 20, 17, 8);    // x20 = hi
                e_rrr(A_EOR, 21, 19, RAX, 1, 0);
                e_rrr(A_EOR, 22, 20, RDX, 1, 0);
                e_rrr(A_ORR, 21, 21, 22, 1, 0);  // x21 = (lo^RAX) | (hi^RDX)
                e_rrr(A_SUBS, 31, 21, 31, 1, 0); // cmp x21, 0 -> Z = (RDX:RAX == [m])
                e_nzcv_save();                   // x86 ZF <- ARM Z
                e_csel(23, RBX, 19, 0, 1);       // store-lo = EQ ? RBX : lo
                e_csel(24, RCX, 20, 0, 1);       // store-hi = EQ ? RCX : hi
                e_store(8, 23, 17);
                e_addi(25, 17, 8, 1);
                e_store(8, 24, 25);
                e_csel(RAX, RAX, 19, 0, 1); // RAX <- EQ ? RAX : lo
                e_csel(RDX, RDX, 20, 0, 1); // RDX <- EQ ? RDX : hi
                gpc = next;
                continue;
            }
            if (op == 0x1E && I.imm_bytes == 0) {
                gpc = next;
                continue;
            } // endbr (modrm consumed)
            if (op == 0x1F) {
                gpc = next;
                continue;
            } // nop r/m
            if (op == 0x18 || op == 0x0D || (op >= 0x19 && op <= 0x1D)) {
                gpc = next;
                continue;
            } // prefetch{nta,t0,t1,t2} (0F 18) / prefetchw (0F 0D) / reserved multi-byte NOP hints — hint only -> NOP
            // shld/shrd (0F A4 imm8, 0F A5 cl, 0F AC imm8, 0F AD cl):  dst=r/m, src=reg, count
            if (op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD) {
                int isleft = (op == 0xA4 || op == 0xA5), bycl = (op == 0xA5 || op == 0xAD);
                int w = I.opsize, mem;
                if (w == 2) {
                    report_unimpl(gpc, &I);
                    break;
                } // 16-bit shld/shrd: rare, EXTR can't do 16-bit lanes
                int ssf = (w == 8) ? 1 : 0, width = ssf ? 64 : 32;
                int dst = rm_load(&I, next, w, &mem), src = I.reg;
                if (!bycl) {
                    int n = (int)(I.imm & (ssf ? 63 : 31));
                    if (n == 0) {
                        if (mem) e_store(w, dst, 17);
                        gpc = next;
                        continue;
                    } // count 0 -> no change, flags intact
                    if (isleft)
                        e_extr(16, dst, src, width - n, ssf); // (dst<<n)|(src>>(W-n))
                    else
                        e_extr(16, src, dst, n, ssf); // (dst>>n)|(src<<(W-n))
                } else {
                    e_mov_rr(22, dst, ssf); // preserve orig dst for the n==0 select
                    e_movconst(19, ssf ? 63 : 31);
                    e_rrr(A_AND, 17, RCX, 19, ssf, 0); // n = cl & (W-1)
                    e_movconst(20, width);
                    e_rrr(A_SUB, 20, 20, 17, ssf, 0); // 20 = W - n
                    if (isleft) {
                        e_shv(S_LSLV, 19, dst, 17, ssf);
                        e_shv(S_LSRV, 20, src, 20, ssf);
                    } else {
                        e_shv(S_LSRV, 19, dst, 17, ssf);
                        e_shv(S_LSLV, 20, src, 20, ssf);
                    }
                    e_rrr(A_ORR, 16, 19, 20, ssf, 0); // combined = t1 | t2
                    e_tst(17, ssf);
                    e_csel(16, 22, 16, 0 /*EQ: n==0*/, ssf); // n==0 -> dst unchanged
                }
                e_tst(16, ssf);
                e_nzcv_save(); // SF/ZF from result (CF/OF approximate)
                rm_store(&I, w, 16);
                gpc = next;
                continue;
            }
            // imul reg, r/m (0F AF)
            if (op == 0xAF) {
                int mem;
                int rmv = rm_load(&I, next, I.opsize, &mem);
                e_imul2(I.reg, I.reg, rmv, I.opsize); // reg *= r/m, sets x86 CF/OF on overflow
                gpc = next;
                continue;
            }
            // bswap (0F C8+r): byte-reverse a register -> ARM REV
            if (op >= 0xC8 && op <= 0xCF) {
                int r = (op - 0xC8) | (I.rexB << 3);
                emit32((sf ? 0xDAC00C00u : 0x5AC00800u) | (r << 5) | r);
                gpc = next;
                continue;
            }
            // 0F AE: fences (lfence/mfence/sfence -> dmb), ldmxcsr/stmxcsr, fxsave/fxrstor (xmm area)
            if (op == 0xAE) {
                int sub = I.reg & 7;
                if (sub >= 5) {
                    emit32(0xD5033BBFu);
                    gpc = next;
                    continue;
                } // *fence -> dmb ish
                if (sub == 2) { // ldmxcsr: thread MXCSR.RC (bits 14:13) -> ARM FPCR.RMode (bits 23:22)
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_load(4, 16, 17);              // x16 = MXCSR
                        e_lsr_i(16, 16, 13, 0);         // x16 = MXCSR >> 13
                        e_movconst(19, 3);
                        e_rrr(A_AND, 16, 16, 19, 0, 0); // x16 = RC (0..3): 00 nearest,01 down,10 up,11 zero
                        // ARM RMode swaps the two RC bits: 00 RN,01 RP(up),10 RM(down),11 RZ -> arm = bitrev2(RC)
                        e_movconst(19, 1);
                        e_rrr(A_AND, 20, 16, 19, 0, 0); // x20 = RC&1
                        e_lsr_i(21, 16, 1, 0);          // x21 = RC>>1
                        e_rrr(A_ORR, 20, 21, 20, 0, 1); // x20 = x21 | (RC&1)<<1  = ARM RMode
                        emit32(0xD53B4400u | 19);       // mrs x19, fpcr
                        e_movconst(21, 3u << 22);
                        e_rrr(A_BIC, 19, 19, 21, 1, 0);  // clear RMode
                        e_rrr(A_ORR, 19, 19, 20, 1, 22); // FPCR.RMode = ARM RMode
                        emit32(0xD51B4400u | 19);        // msr fpcr, x19
                    }
                    gpc = next;
                    continue;
                }
                if (sub == 3) { // stmxcsr: report MXCSR default + current rounding mode (from FPCR.RMode)
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        emit32(0xD53B4400u | 19);       // mrs x19, fpcr
                        e_lsr_i(19, 19, 22, 0);         // x19 = FPCR >> 22
                        e_movconst(20, 3);
                        e_rrr(A_AND, 19, 19, 20, 0, 0); // x19 = ARM RMode
                        e_movconst(20, 1);
                        e_rrr(A_AND, 21, 19, 20, 0, 0);
                        e_lsr_i(22, 19, 1, 0);
                        e_rrr(A_ORR, 19, 22, 21, 0, 1);  // x19 = x86 RC (swap back)
                        e_movconst(16, 0x1f80);          // default MXCSR (all exceptions masked, RC=00)
                        e_rrr(A_ORR, 16, 16, 19, 0, 13); // MXCSR |= RC << 13
                        e_store(4, 16, 17);
                    }
                    gpc = next;
                    continue;
                }
                if ((sub == 0 || sub == 1) && I.is_mem) { // fxsave / fxrstor: XMM0-15 @+160, MXCSR @+24
                    emit_ea(&I, next);
                    for (int i = 0; i < 16; i++) {
                        if (sub == 0)
                            e_str_q(i, 17, 160 + i * 16);
                        else
                            e_ldr_q(i, 17, 160 + i * 16);
                    }
                    gpc = next;
                    continue; // (x87/MMX + MXCSR areas left as-is; we don't honor them)
                }
            }
            // bsf/tzcnt (0F BC), bsr/lzcnt (0F BD). The F3 prefix selects the BMI/ABM count form, which is
            // DISTINCT from the legacy bit-scan: tzcnt==bsf result for src!=0 but lzcnt != bsr (lzcnt = leading
            // ZERO COUNT = CLZ; bsr = bit INDEX = (w-1)-CLZ). Mixing them up silently corrupts BMI codegen
            // (e.g. tinystr length math) -- the bug behind uutils' garbled "en-US" locale.
            if (op == 0xBC || op == 0xBD) {
                int mem;
                int rmv = rm_load(&I, next, I.opsize, &mem);
                int cnt = I.rep;  // F3 -> tzcnt/lzcnt (counts; src==0 -> opsize, the ARM CLZ result naturally)
                // The destination write below clobbers the source register when dest==src (e.g.
                // `bsf %edx,%edx` -- exactly the form Go's bytealg.IndexByteString emits). The flag
                // computation below reads the source AFTER that write, so without this guard the x86
                // ZF/CF would reflect the RESULT, not the source -> a no-match bsf wrongly clears ZF and
                // the caller mis-reads a hit. Preserve the source in a scratch (x23) so flags stay correct.
                int src = rmv;
                if (!mem && I.reg == rmv) {
                    e_mov_rr(23, rmv, sf);
                    src = 23;
                }
                // Legacy bsf/bsr (no F3) compute the bit INDEX into x22 first: x86 leaves the
                // DESTINATION UNCHANGED when src==0 (real-hw behavior that glibc memrchr relies on -- its
                // not-found tail is `bsr; je; ret <dest>`), so the index is csel'd in only when src!=0.
                // tzcnt/lzcnt (F3) instead DEFINE src==0 -> opsize and write the dest unconditionally.
                int bdst = cnt ? I.reg : 22;
                if (op == 0xBC) { // tzcnt / bsf: trailing zeros = RBIT+CLZ (same value; src==0 -> opsize)
                    e_rbit(bdst, src, sf);
                    e_clz(bdst, bdst, sf);
                } else if (cnt) { // lzcnt: leading zeros = CLZ
                    e_clz(I.reg, src, sf);
                } else { // bsr: (w-1) - clz
                    e_clz(16, src, sf);
                    e_movconst(19, sf ? 63 : 31);
                    e_rrr(A_SUB, 22, 19, 16, sf, 0);
                }
                if (cnt) { // tzcnt/lzcnt: x86 CF = (src==0), ZF = (result==0)
                    e_rrr(A_SUBS, 31, src, 31, sf, 0);
                    e_cset(19, 0 /*EQ*/, sf);               // x19 = (src==0) = x86 CF
                    e_rrr(A_ANDS, 31, I.reg, I.reg, sf, 0); // live N/Z from the result
                    e_nzcv_save_setcf(19);                  // store N/Z, stored C = NOT(src==0)
                } else {                                    // bsf/bsr: ZF = (src==0), dest UNCHANGED if src==0
                    e_rrr(A_ANDS, 31, src, src, sf, 0);     // Z = (src == 0)
                    e_csel(I.reg, I.reg, 22, 0 /*EQ*/, sf); // src==0 -> keep dest, else the computed index
                    e_nzcv_save();
                }
                gpc = next;
                continue;
            }
            // popcnt (F3 0F B8): dest = popcount(src). x86 sets ZF=(src==0) and clears CF/OF/SF/AF/PF.
            // (Without F3, 0F B8 is the reserved JMPE -> falls through to report_unimpl.)
            if (op == 0xB8 && I.rep) {
                int mem;
                int rmv = rm_load(&I, next, I.opsize, &mem);
                // dest==src (e.g. `popcnt %rax,%rax`): the result write clobbers the source before the
                // ZF computation reads it, so preserve the source in a scratch first (mirrors bsf above).
                int src = rmv;
                if (!mem && I.reg == rmv) {
                    e_mov_rr(23, rmv, sf);
                    src = 23;
                }
                // NEON popcount: move src into scratch v16 (upper lanes zeroed), per-byte CNT, sum via ADDV.
                if (sf)
                    e_fmov_to_d(16, src); // fmov d16, x[src]  (zeroes bits[64:128])
                else
                    e_fmov_to_s(16, src);             // fmov s16, w[src] (zeroes bits[32:128])
                emit32(0x0E205800u | (16 << 5) | 16); // cnt v16.8b, v16.8b  (per-byte popcount)
                emit32(0x0E31B800u | (16 << 5) | 16); // addv b16, v16.8b    (sum the 8 byte counts -> 0..64)
                e_fmov_from_s(I.reg, 16);             // dest = count; the W-write zero-extends (correct for both widths)
                e_rrr(A_ANDS, 31, src, src, sf, 0);   // N/Z from the source: ZF = (src == 0)
                e_nzcv_save_c1();                     // store N/Z, force x86 CF=0/OF=0
                gpc = next;
                continue;
            }
            // bit ops: BT(A3) BTS(AB) BTR(B3) BTC(BB), and group BA /4..7 with imm8.
            if (op == 0xA3 || op == 0xAB || op == 0xB3 || op == 0xBB || op == 0xBA) {
                int isimm = (op == 0xBA);
                int sub = isimm ? (I.reg & 7) : (op == 0xA3 ? 4 : op == 0xAB ? 5 : op == 0xB3 ? 6 : 7);
                if (sub < 4) {
                    report_unimpl(gpc, &I);
                    break;
                }
                int w = I.opsize, mem, bits = w * 8;
                int logbits = w == 8 ? 6 : w == 4 ? 5 : 4; // log2(bits): 64/32/16
                int logw = w == 8 ? 3 : w == 4 ? 2 : 1;    // log2(w)
                int val;
                // x86 bit-string addressing: with a MEMORY base and a REGISTER bit offset, the high bits of
                // the (signed) offset select the addressed word (EA + (offset/bits)*w); only the low
                // log2(bits) bits index within it. (An immediate offset is taken modulo the operand size,
                // for both reg and mem.) Dropping the high bits -- the pre-fix behavior -- mis-tests a
                // 256-bit bitset (e.g. glibc/grep's DFA charclass `bt %reg,(%mem)`), the debian-grep miss.
                if (I.is_mem && !isimm) {
                    emit_ea(&I, next);            // x17 = base EA
                    if (w == 8)
                        e_mov_rr(20, I.reg, 1);
                    else
                        e_sxt(20, I.reg, w);           // sxtw/sxth: index as a 64-bit signed value
                    e_asr_i(20, 20, logbits, 1);       // x20 = signed word offset = index >> log2(bits)
                    e_rrr(A_ADD, 17, 17, 20, 1, logw); // EA += wordoff * w
                    e_load(w, 16, 17);
                    val = 16;
                    mem = 1;
                } else {
                    val = rm_load(&I, next, w, &mem);
                }
                if (isimm)
                    e_movconst(19, (uint64_t)(((uint64_t)I.imm) & (bits - 1))); // idx -> x19
                else {
                    e_movconst(21, bits - 1);
                    e_rrr(A_AND, 19, I.reg, 21, sf, 0);
                }
                e_shv(S_LSRV, 21, val, 19, sf);
                e_movconst(22, 1);
                e_rrr(A_AND, 21, 21, 22, sf, 0); // x21 = bit
                e_rrr(A_SUBS, 31, 31, 21, 1, 0);
                e_nzcv_save();  // ARM C = !bit  (subs convention -> x86 CF)
                if (sub != 4) { // BTS/BTR/BTC: modify the bit + write back
                    int o = mem ? 16 : I.rm_reg;
                    e_movconst(22, 1);
                    e_shv(S_LSLV, 22, 22, 19, sf); // mask = 1<<idx
                    if (sub == 5)
                        e_rrr(A_ORR, o, val, 22, sf, 0); // BTS
                    else if (sub == 6)
                        e_rrr(A_BIC, o, val, 22, sf, 0); // BTR
                    else
                        e_rrr(A_EOR, o, val, 22, sf, 0); // BTC
                    rm_store(&I, w, o);
                }
                gpc = next;
                continue;
            }
            // cmpxchg (0F B0 byte / B1): compare RAX with r/m; if eq, r/m=reg, ZF=1; else RAX=r/m.
            if (op == 0xB0 || op == 0xB1) {
                int w = op == 0xB0 ? 1 : I.opsize, sf2 = (w == 8);
                if (I.is_mem) {
                    emit_ea(&I, next);
                    e_mov_rr(19, RAX, sf2);    // expected
                    e_cas(w, 19, I.reg, 17);   // x19 = old; if old==expected [m]=reg
                    do_alu(7, -1, 19, RAX, w); // ZF = (old == rax)
                    if (w >= 4)
                        e_mov_rr(RAX, 19, sf2);
                    else
                        e_bfi(RAX, 19, 0, 8 * w, 1); // rax = old
                } else if (w >= 4) {
                    e_mov_rr(19, I.rm_reg, sf2);
                    do_alu(7, -1, 19, RAX, w);
                    e_csel(I.rm_reg, I.reg, 19, 0, sf2); // rm = ZF? reg : rm_old
                    e_csel(RAX, RAX, 19, 0, sf2);        // rax = ZF? rax : rm_old
                } else {
                    report_unimpl(gpc, &I);
                    break;
                }
                gpc = next;
                continue;
            }
            // xadd (0F C0 byte / C1): tmp=r/m; r/m += reg; reg = tmp (+ flags)
            if (op == 0xC0 || op == 0xC1) {
                int w = op == 0xC0 ? 1 : I.opsize, sf2 = (w == 8);
                if (I.is_mem) {
                    emit_ea(&I, next);
                    e_lse(LSE_LDADD, w, I.reg, 19, 17); // x19 = old; [m] += reg
                    do_alu(0, -1, 19, I.reg, w);        // flags from old+reg
                    if (w >= 4)
                        e_mov_rr(I.reg, 19, sf2);
                    else
                        e_bfi(I.reg, 19, 0, 8 * w, 1); // reg = old
                } else if (w >= 4) {
                    e_mov_rr(19, I.rm_reg, sf2); // old
                    e_rrr(A_ADDS, I.rm_reg, I.rm_reg, I.reg, sf2, 0);
                    e_nzcv_save_ci();         // rm += reg (x86 add carry)
                    e_mov_rr(I.reg, 19, sf2); // reg = old
                } else {
                    report_unimpl(gpc, &I);
                    break;
                }
                gpc = next;
                continue;
            }
            // jcc rel32 (0F 80-8F)
            if ((op & 0xF0) == 0x80) {
                int lo = op & 0xF, parity = (lo == 0xA || lo == 0xB);
                int cc;
                if (parity) {
                    cc = emit_parity_jcc_cond(lo); // jp/jnp: PF lane -> live ARM Z, branch off it
                } else {
                    cc = x86cc_to_arm(lo);
                    if (cc < 0) {
                        if (g_fl_pending) flags_materialize(); // materialize before boundary
                        report_unimpl(gpc, &I);
                        break;
                    }
                }
                uint64_t taken = next + (uint64_t)I.imm;
                // W5B tier-2: single-block self-loop (taken back-edge == block start). See jcc rel8.
                if (!parity && taken == start && !notier2x() &&
                    !loop_has_rmw_hazard((uint64_t)body, (uint64_t)g_cp)) {
                    int slot = g_tier2_build ? 0 : t2_slot(start);
                    if (g_tier2_build || slot >= 0) {
                        emit_selfloop_x86(cc, start, next, body, slot);
                        break;
                    }
                }
                if (parity) {
                    // live ARM Z already holds (PF==0) from emit_parity_jcc_cond; flags spilled there.
                } else if (g_fl_pending) {
                    // Fast path (see jcc rel8): spill the deferred flags for successors AND leave
                    // the live NZCV canonical, then branch off it; drop the redundant e_nzcv_load.
                    flags_materialize();
                } else {
                    e_nzcv_load();
                }
                uint64_t fall = next;
                // STITCH (see jcc rel8): inline the fall-through, invert the cond, taken exit OOL.
                // cpu->nzcv is materialized above, so both arms see canonical flags.
                if (STITCH_OK && fall != start && !seen_has(seen, nseen, fall) && !map_body(fall) && !trap_head(fall)) {
                    int inv = (cc ^ 1) & 0xF;
                    uint32_t *patch = (uint32_t *)g_cp;
                    emit32(0); // b.inv -> fall (inline)
                    emit_chain_exit(taken);
                    int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
                    *patch = 0x54000000u | (((uint32_t)d & 0x7FFFF) << 5) | (uint32_t)inv;
                    seen[nseen++] = fall;
                    trace_blk++;
                    gpc = fall;
                    continue;
                }
                uint32_t *patch = (uint32_t *)g_cp;
                emit32(0);
                emit_chain_exit(next);
                int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
                *patch = 0x54000000u | (((uint32_t)d & 0x7FFFF) << 5) | (cc & 0xF);
                emit_chain_exit(taken);
                break;
            }
            // setcc (0F 90-9F) -> r/m8 (byte: preserve upper bits / hi-lo byte regs)
            if ((op & 0xF0) == 0x90) {
                int lo = op & 0xF;
                if (lo == 0xA || lo == 0xB) { // setp/setnp: real PF lane (integer parity or comisd unordered)
                    if (I.is_mem) emit_ea(&I, next);
                    e_pf_compute(19); // x19 = x86 PF (uses x16 as scratch; x17/EA preserved)
                    if (lo == 0xB) {
                        e_movconst(16, 1);
                        e_rrr(A_EOR, 19, 19, 16, 0, 0); // setnp = NOT PF
                    }
                    if (I.is_mem)
                        e_store(1, 19, 17);
                    else
                        byte_wb(&I, I.rm_reg, 19);
                    gpc = next;
                    continue;
                }
                int cc = x86cc_to_arm(op & 0xF);
                if (cc < 0) {
                    report_unimpl(gpc, &I);
                    break;
                }
                if (I.is_mem) {
                    emit_ea(&I, next); // EA -> x17 FIRST (emit_ea may clobber x16)
                    e_nzcv_load();
                    e_cset(16, cc, 0);
                    e_store(1, 16, 17);
                } else {
                    e_nzcv_load();
                    e_cset(16, cc, 0);
                    byte_wb(&I, I.rm_reg, 16);
                }
                gpc = next;
                continue;
            }
            // cmovcc (0F 40-4F), reg or mem source
            if ((op & 0xF0) == 0x40) {
                int lo = op & 0xF;
                if (lo == 0xA || lo == 0xB) { // cmovp / cmovnp: real PF lane
                    e_pf_compute(19);         // x19 = x86 PF (before rm_load, which reuses x16/x17)
                    int mem;
                    int rmv = rm_load(&I, next, I.opsize, &mem);
                    e_rrr(A_SUBS, 31, 19, 31, 0, 0);                     // Z = (PF == 0)
                    e_csel(I.reg, rmv, I.reg, (lo == 0xA) ? 1 : 0, sf); // cmovp: NE(PF==1); cmovnp: EQ(PF==0)
                    gpc = next;
                    continue;
                }
                int cc = x86cc_to_arm(op & 0xF);
                if (cc < 0) {
                    report_unimpl(gpc, &I);
                    break;
                }
                int mem;
                int rmv = rm_load(&I, next, I.opsize, &mem);
                e_nzcv_load();
                e_csel(I.reg, rmv, I.reg, cc, sf);
                gpc = next;
                continue;
            }
            // movzx/movsx (0F B6/B7 zero, BE/BF sign), movsxd handled as 0x63 one-byte (TODO)
            if (op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF) {
                int w = (op & 1) ? 2 : 1; // B6/BE byte, B7/BF word
                int signd = (op >= 0xBE);
                if (I.is_mem) {
                    emit_ea(&I, next);
                    if (signd)
                        e_ldrs(w, I.reg, 17);
                    else
                        e_load(w, I.reg, 17);
                } else {
                    int src = (w == 1) ? byte_val(&I, I.rm_reg, 16) : I.rm_reg; // byte source: ah/bh/ch/dh -> bits 8-15
                    if (signd)
                        e_sxt(I.reg, src, w);
                    else
                        e_uxt(I.reg, src, w);
                }
                gpc = next;
                continue;
            }
        }
        report_unimpl(gpc, &I);
        break;
    }
    // W5B tier-2: the promoter (g_tier2_build) recompiles in place and updates the EXISTING map entry
    // itself, so don't insert a duplicate and don't chain pending edges here (the promoter does both
    // AFTER icache-flushing the new code). Expose the body for it.
    g_last_body = body;
    if (!g_tier2_build) {
        map_put(start, host, body);
        if (!g_threaded) patch_links_to(start, body); // chaining mutates live blocks -> off when threaded
    }
    return host;
#undef STITCH_OK
}

// W5B tier-2: promote a hot self-loop (its in-cache counter hit threshold and exited R_TIER2 with
// rip == gpc). Recompile the block with the folded back-edge (+ dead-flag-save elision), then SWAP it in
// under live execution: emit+icache-flush the tier-2 code, redirect the old body, repoint the live map
// entry + still-pending chains, and drop a stale IBTC entry. The old tier-1 code is left as dead bytes.
// Single-threaded only (skipped once a guest thread exists -- promotion mutates the cache outside the
// threaded lock discipline; the loop keeps running tier-1, still correct). Caller is the dispatcher
// between block runs, so guest state is fully spilled. Reuses the shared jit/cache.c substrate
// (g_tier2_build/g_last_body/g_prof_t2/map_idx/patch_links_to/g_ibtc).
static void tier2_promote(uint64_t gpc) {
    if (g_threaded || notier2x()) return;
    int mi = map_idx(gpc);
    if (mi < 0) return;
    pthread_jit_write_protect_np(0);
    g_emit_start = g_cp;
    g_tier2_build = 1;
    void *nh = translate_block(gpc); // folded recompile; no counter, no map_put, no chain
    void *nb = g_last_body;
    g_tier2_build = 0;
    if (getenv("T2DUMP")) {
        fprintf(stderr, "[t2dump] gpc=%llx body+%ld:", (unsigned long long)gpc, (long)((uint8_t *)nb - (uint8_t *)nh));
        for (uint32_t *p = (uint32_t *)nb; (uint8_t *)p < g_cp; p++)
            fprintf(stderr, " %08x", *p);
        fprintf(stderr, "\n");
    }
    // make the tier-2 code coherent BEFORE anything can branch into it
    sys_icache_invalidate(g_emit_start, (size_t)(g_cp - g_emit_start));
    // redirect the OLD tier-1 body to tier-2 (predecessor chains were resolved to the old body when they
    // were translated; patch_links_to only fixes still-PENDING edges) -- overwrite its first insn with
    // `b nb`. Costs one branch per loop ENTRY (negligible vs the loop body).
    void *old_body = g_map[mi].body;
    int64_t bd = ((uint8_t *)nb - (uint8_t *)old_body) / 4;
    *(uint32_t *)old_body = 0x14000000u | ((uint32_t)bd & 0x3FFFFFFu);
    sys_icache_invalidate(old_body, 4);
    // swap the live map entry: future dispatcher lookups + IBTC fills resolve to tier-2 directly
    g_map[mi].host = nh;
    g_map[mi].body = nb;
    patch_links_to(gpc, nb); // repoint any still-unresolved chains to this gpc straight at tier-2
    uint32_t h = (uint32_t)((gpc >> 2) & (IBTC_N - 1)); // drop a stale IBTC entry (refills to tier-2)
    if (g_ibtc[h].target == gpc) {
        g_ibtc[h].target = 0;
        g_ibtc[h].body = NULL;
    }
    pthread_jit_write_protect_np(1);
    g_prof_t2++;
}

static void report_unimpl(uint64_t pc, struct insn *I) {
    const uint8_t *p = (const uint8_t *)pc;
    fprintf(stderr, "[jit86] UNIMPL %s opcode 0x%02x at rip=%llx  bytes:", I->two ? "0F" : "1B", I->op,
            (unsigned long long)pc);
    for (int i = 0; i < (I->len ? I->len : 8); i++)
        fprintf(stderr, " %02x", p[i]);
    fprintf(stderr, "\n");
    // emit a clean exit that terminates the guest (so we don't run off into garbage).
    emit_spill();
    e_movconst(16, 0xDEAD0000u | I->op);
    e_str(16, 28, OFF_RIP);
    e_movconst(16, 99);
    e_str(16, 28, OFF_RSN); // reason 99 -> dispatcher aborts
    emit_host_ptr(16, (uint64_t)block_return, PRELOC_BLOCKRET);
    e_br(16);
}

// ---------------- host entry trampolines (adapted from jit.c, x86 reg set) ----------------
__attribute__((naked)) static void run_block(struct cpu *cpu, void *code) {
    __asm__ volatile( // x0=cpu, x1=code
        "str x19,[x0,#176]\n str x20,[x0,#184]\n str x21,[x0,#192]\n str x22,[x0,#200]\n"
        "str x23,[x0,#208]\n str x24,[x0,#216]\n str x25,[x0,#224]\n str x26,[x0,#232]\n"
        "str x27,[x0,#240]\n str x28,[x0,#248]\n str x29,[x0,#256]\n str x30,[x0,#264]\n"
        "str q8,[x0,#272]\n str q9,[x0,#288]\n str q10,[x0,#304]\n str q11,[x0,#320]\n"
        "str q12,[x0,#336]\n str q13,[x0,#352]\n str q14,[x0,#368]\n str q15,[x0,#384]\n"
        "mov x9, sp\n str x9,[x0,#168]\n" // host_sp
        "br x1\n");                       // -> emitted prologue (sets x28=cpu)
}
__attribute__((naked)) static void block_return(void) {
    __asm__ volatile( // x28 == &cpu (pinned through the block)
        "ldr x19,[x28,#176]\n ldr x20,[x28,#184]\n ldr x21,[x28,#192]\n ldr x22,[x28,#200]\n"
        "ldr x23,[x28,#208]\n ldr x24,[x28,#216]\n ldr x25,[x28,#224]\n ldr x26,[x28,#232]\n"
        "ldr x27,[x28,#240]\n ldr x29,[x28,#256]\n ldr x30,[x28,#264]\n"
        "ldr q8,[x28,#272]\n ldr q9,[x28,#288]\n ldr q10,[x28,#304]\n ldr q11,[x28,#320]\n"
        "ldr q12,[x28,#336]\n ldr q13,[x28,#352]\n ldr q14,[x28,#368]\n ldr q15,[x28,#384]\n"
        "ldr x9,[x28,#168]\n mov sp, x9\n" // host sp
        "ldr x28,[x28,#248]\n"             // restore host x28 LAST (was using it as base)
        "ret\n");
}
