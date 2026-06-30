// dd/runtime/translate/x86_64 -- shift/rotate instruction class (group2: C0/C1 imm, D0/D1 by 1, D2/D3 by
// CL -> SHL/SHR/SAR/ROL/ROR/RCL/RCR), lifted VERBATIM out of translate_block's one-byte switch. Emits the
// exact x86 CF/SF/ZF/PF (incl. the count==0 'no flags change' rule and the by-CL masked-count case). A rare
// unhandled form (RCL/RCR by CL) defers to C via report_unimpl. Returns TX_FALL (not a shift), TX_NEXT
// (caller: gpc = next; continue) or TX_BREAK (deferred form -> end the block). #included after the other
// class files (uses emit_rcl_rcr/rm_load/rm_store/report_unimpl above + the e_* shift/flag emitters).
static int translate_shift(struct insn *I, uint64_t gpc, uint64_t next) {
    uint8_t op = I->op;
    int sf = I->opsize == 8;
            // ---- shifts: group2 (C0/C1 imm, D0/D1 by 1, D2/D3 by CL) ----
            if (op == 0xC0 || op == 0xC1 || op == 0xD0 || op == 0xD1 || op == 0xD2 || op == 0xD3) {
                int k = I->reg & 7;
                if (k == 6) k = 4; // SAL == SHL
                int w = (op & 1) ? I->opsize : 1, mem;
                int bycl = (op == 0xD2 || op == 0xD3), by1 = (op == 0xD0 || op == 0xD1);
                // RCL(/2)/RCR(/3) through carry: the constant-count forms (by-1, immediate) are emitted here;
                // the by-CL form still defers (caught by the unimpl check below).
                if ((k == 2 || k == 3) && !bycl) {
                    int cmask = (w == 8) ? 63 : 31;
                    emit_rcl_rcr(I, next, w, k == 3, by1 ? 1 : ((int)I->imm & cmask));
                    return TX_NEXT;
                }
                if (k != 0 && k != 1 && k != 4 && k != 5 && k != 7) {
                    return TX_BREAK;
                } // RCL/RCR by CL defer
                int raw = rm_load(I, next, w, &mem);
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
                        int ce = (((int)(I->imm) % width) + width) % width;
                        int rr = (k == 1) ? ce : (width - ce) % width;
                        if (rr) e_ror_i(16, 16, rr, 0); // 32-bit ROR; low `width` bits are the answer
                    }
                    rm_store(I, w, 16); // stores low w bytes; x86 rotates leave SF/ZF unchanged -> no flag save
                    return TX_NEXT;
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
                int cnt = by1 ? 1 : (bycl ? -1 : (int)(I->imm & (ssf ? 63 : 31)));
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
                        rm_store(I, w, 16);
                        return TX_NEXT;
                    }
                    uint32_t b = k == 4 ? S_LSLV : k == 5 ? S_LSRV : k == 7 ? S_ASRV : S_RORV;
                    e_shv(b, 16, src, RCX, ssf);
                } else {
                    if (cnt == 0) {
                        // x86: a 0 effective count changes NO flags and leaves the value unchanged --
                        // but a 32-bit register destination is still written, so bits 63:32 must be
                        // zeroed (a 32-bit op zero-extends). w==8/16/8 register dests keep their bits;
                        // a memory dest is rewritten unchanged.
                        if (mem)
                            e_store(w, raw, 17);
                        else if (w == 4)
                            e_mov_rr(raw, raw, 0);
                        return TX_NEXT;
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
                rm_store(I, w, 16);
                return TX_NEXT;
            }
    return TX_FALL;
}
