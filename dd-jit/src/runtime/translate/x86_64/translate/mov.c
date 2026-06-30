// dd/runtime/translate/x86_64 -- data-move instruction class, lifted VERBATIM out of translate_block's
// one-byte switch (behavior-preserving move): mov r,imm (B0-BF), mov r/m,imm (C6/C7), mov r/m<->r
// (88-8B), lea (8D), push/pop r (50-5F), movsxd/move (0x63). None of these touch the modeled EFLAGS, so
// there is no lazy-flag interaction. Every handler advances past the insn -> the helper returns TX_FALL
// (not a move -> caller falls through) or TX_NEXT (caller: gpc = next; continue). #included after the
// other class files; uses EA helpers from decode.c + e_* from emit.c + byte_val/byte_wb (all defined above).
static int translate_mov(struct insn *I, uint64_t next) {
    uint8_t op = I->op;
    int sf = I->opsize == 8;
            // ---- mov r8, imm8 (B0+r) ----
            if (op >= 0xB0 && op <= 0xB7) {
                int rnum = (op - 0xB0) | (I->rexB << 3);
                e_movz(16, (uint32_t)(I->imm & 0xff), 0);
                byte_wb(I, rnum, 16);
                return TX_NEXT;
            }
            // ---- mov r, imm (B8+r) ----
            if (op >= 0xB8 && op <= 0xBF) {
                int rd = (op - 0xB8) | (I->rexB << 3);
                e_movconst(rd, sf ? (uint64_t)I->imm : (uint64_t)(uint32_t)I->imm);
                return TX_NEXT;
            }
            // ---- mov r/m, imm (C7 /0, C6 /0) ----
            if (op == 0xC7 || op == 0xC6) {
                int w = op == 0xC6 ? 1 : I->opsize;
                if (I->is_mem) {
                    emit_ea(I, next);
                    e_movconst(16, (uint64_t)I->imm);
                    e_store(w, 16, 17);
                } else
                    e_movconst(I->rm_reg, sf ? (uint64_t)I->imm : (uint64_t)(uint32_t)I->imm);
                return TX_NEXT;
            }
            // ---- mov r/m,r (88/89) and r,r/m (8A/8B) ----
            if (op == 0x88 || op == 0x89 || op == 0x8A || op == 0x8B) {
                int w = (op & 1) ? I->opsize : 1;
                int to_reg = (op & 2); // 8A/8B: dest is reg
                if (I->is_mem) {
                    if (to_reg) {     // mov reg, [mem]  -- folded into one ldr when [base+disp]
                        if (w == 1) { // byte dest: ah/bh/ch/dh -> bits 8-15; lo8 preserves upper
                            emit_load_mem(I, next, 1, 16);
                            byte_wb(I, I->reg, 16);
                        } else
                            emit_load_mem(I, next, w, I->reg);
                    } else { // mov [mem], reg  -- folded into one str when [base+disp]
                        int rn, off, f = ea_imm_fold(I, w, &rn, &off);
                        if (f) {
                            int sv = (w == 1) ? byte_val(I, I->reg, 16) : I->reg;
                            if (f == 1)
                                e_store_uoff(w, sv, rn, (unsigned)off);
                            else
                                e_stur(w, sv, rn, off);
                        } else {
                            emit_ea(I, next);                                   // may clobber x16
                            int sv = (w == 1) ? byte_val(I, I->reg, 16) : I->reg; // byte src: ah/bh/ch/dh -> bits 8-15
                            e_store(w, sv, 17);
                        }
                    }
                } else if (w == 1) {
                    // byte reg-to-reg (88/8A, mod=3): copy ONE byte, hi8-aware, preserving the dest's other
                    // bits. The full-width e_mov_rr below was wrong here -- `mov bl,cl` copied all of ecx into
                    // ebx (and high-byte src/dst like `mov al,dh` were garbage), only masked when the upper
                    // bytes happened to be 0. That polluted icu's TinyStr niche math -> dropped 'n' in "en-US".
                    int srcreg = to_reg ? I->rm_reg : I->reg;
                    int dstreg = to_reg ? I->reg : I->rm_reg;
                    int sv = byte_val(I, srcreg, 16);
                    byte_wb(I, dstreg, sv);
                } else {
                    if (to_reg)
                        e_mov_rr(I->reg, I->rm_reg, sf);
                    else
                        e_mov_rr(I->rm_reg, I->reg, sf);
                }
                return TX_NEXT;
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
                if (sf && I->rip_rel && g_nonpie_types_lo) {
                    uint64_t lo = (next - g_nonpie_bias) + (uint64_t)I->disp; // low link target
                    if (lo >= g_nonpie_types_lo && lo < g_nonpie_types_hi) {
                        e_movconst(I->reg, lo);
                        return TX_NEXT;
                    }
                }
                emit_ea_core(I, next, 0); // lea returns the guest (low) effective ADDRESS -> no bias-fold
                e_mov_rr(I->reg, 17, sf);
                return TX_NEXT;
            }
            // ---- push/pop r (50-5F) ----
            if (op >= 0x50 && op <= 0x57) {
                int r = (op - 0x50) | (I->rexB << 3);
                e_subi(RSP, RSP, 8, 1);
                e_store(8, r, RSP);
                return TX_NEXT;
            } // push (64-bit)
            if (op >= 0x58 && op <= 0x5F) {
                int r = (op - 0x58) | (I->rexB << 3);
                e_load(8, r, RSP);
                e_addi(RSP, RSP, 8, 1);
                return TX_NEXT;
            } // pop
            // ---- movsxd (0x63): operand-size governed. REX.W -> sign-extend r/m32 to r64; no REX.W ->
            // a 32-bit move (zero-extend r/m32 to 64); 0x66 -> a 16-bit move (insert low 16, keep 63:16).
            if (op == 0x63) {
                if (I->opsize == 8) { // REX.W: sign-extend 32 -> 64
                    if (I->is_mem) {
                        emit_ea(I, next);
                        e_ldrs(4, I->reg, 17);
                    } else
                        e_sxt(I->reg, I->rm_reg, 4);
                } else if (I->opsize == 2) { // 0x66: 16-bit move, preserve bits 63:16
                    if (I->is_mem) {
                        emit_ea(I, next);
                        e_load(2, 16, 17);
                        e_bfi(I->reg, 16, 0, 16, 1);
                    } else
                        e_bfi(I->reg, I->rm_reg, 0, 16, 1);
                } else { // no REX.W: 32-bit move, zero-extend to 64
                    if (I->is_mem) {
                        emit_ea(I, next);
                        e_load(4, I->reg, 17);
                    } else
                        e_mov_rr(I->reg, I->rm_reg, 0);
                }
                return TX_NEXT;
            }
    return TX_FALL;
}
