// dd/runtime/frontend/x86_64 -- the x86-64 -> arm64 translator (flag synthesis, SSE/x87 lowering, the
// big translate_block) + host entry trampolines.

// ---------------- the translator ----------------
static void report_unimpl(uint64_t pc, struct insn *I);

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
// Width-correct ALU: dst = a <kind> b, set cpu->nzcv.  dst<0 => cmp/test (no write).
// 4/8-byte: direct ARM op. 1/2-byte: operate in the HIGH bits (<<sh) so ARM NZCV matches
// x86 byte/word flags exactly, then merge the low w bytes back (preserving upper bits).
static void do_alu(int kind, int dst, int a, int b, int w) {
    int sf = w == 8, out = dst < 0 ? 31 : dst;
    int ak = kind == 7 ? 5 : kind; // cmp == sub(discard); test == and(discard)
    if (kind == 7) ak = 5;
    if (kind == 2) {                          // ADC: carry-in = x86 CF = NOT stored; result CF stored inverted
        e_nzcv_load_ci();                     // live ARM C = x86 CF
        e_rrr(0x3A000000u, out, a, b, sf, 0); // adcs
        e_nzcv_save_ci();
        return;
    }
    if (kind == 3) { // SBB: borrow convention matches stored C directly
        e_nzcv_load();
        e_rrr(0x7A000000u, out, a, b, sf, 0); // sbcs
        e_nzcv_save();
        return;
    }
    int logical = (kind == 1 || kind == 4 || kind == 6); // or/and/xor (and test): x86 clears CF
    if (w >= 4) {
        alu_core(ak, out, a, b, sf);
        if (kind == 0)
            e_nzcv_save_ci(); // x86 add -> invert ARM add-carry
        else if (logical)
            e_nzcv_save_c1(); // x86 CF=0 -> stored C=1
        else
            e_nzcv_save();
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
    case 5: e_rrr(A_SUB, 20, 31, rs, sf, 0); rsu = 20; lse = LSE_LDADD; break; // sub: atomic add(-v)
    case 1: lse = LSE_LDSET; break;                                            // or
    case 6: lse = LSE_LDEOR; break;                                            // xor
    case 4: e_rrr(A_ORN, 20, 31, rs, sf, 0); rsu = 20; lse = LSE_LDCLR; break; // and: clear ~v
    default: return 0;
    }
    e_lse(lse, w, rsu, 19, 17); // x19 = old; [x17] op= rsu  (acquire-release)
    do_alu(k, -1, 19, rs, w);   // x86 flags from (old OP original-operand)
    return 1;
}

// x86 condition (opcode low nibble) -> ARM cond, or -1 if unsupported (parity).
static int x86cc_to_arm(int cc) {
    // x86 PF (parity, idx 10/11) -> ARM V flag: our FP compares (comis*/fcomi) leave V=1 on
    // unordered (NaN), which is exactly what `jp`/`jnp` after an FP compare test. (Integer
    // parity is not modeled, but is essentially unused outside the FP-compare idiom.)
    static const int t[16] = {6, 7, 3, 2, 0, 1, 9, 8, 4, 5, 6, 7, 11, 10, 13, 12};
    return t[cc & 0xF];
}

// Translate the basic block at guest address gpc; returns host entry pointer.
static void *translate_block(uint64_t gpc) {
    uint64_t start = gpc;
    void *host = g_cp;
    emit_prologue();
    void *body = g_cp;
    for (;;) {
        if (g_itrace && gpc != start) {
            emit_chain_exit(gpc);
            break;
        } // 1 insn/block: per-instruction register dump
        struct insn I;
        decode(gpc, &I);
        uint64_t next = gpc + I.len;
        uint8_t op = I.op;
        int sf = I.opsize == 8;
        if (g_trace)
            fprintf(stderr, "[dec] %llx %s%02x len=%d mod%d rm%d reg%d mem%d base%d idx%d disp=%lld imm=%lld\n",
                    (unsigned long long)gpc, I.two ? "0F " : "", op, I.len, I.mod, I.rm_reg, I.reg, I.is_mem,
                    I.m_hasbase ? I.m_base : -1, I.m_hasindex ? I.m_index : -1, (long long)I.disp, (long long)I.imm);

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
                    emit_ea(&I, next);
                    if (to_reg) { // mov reg, [mem]
                        if (w == 1) {
                            e_load(1, 16, 17);
                            byte_wb(&I, I.reg, 16);
                        } // byte dest: ah/bh/ch/dh -> bits 8-15; lo8 preserves upper
                        else
                            e_load(w, I.reg, 17);
                    } else {                                                 // mov [mem], reg
                        int sv = (w == 1) ? byte_val(&I, I.reg, 16) : I.reg; // byte src: ah/bh/ch/dh -> bits 8-15
                        e_store(w, sv, 17);
                    }
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
                emit_ea(&I, next);
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
                if ((k == 2 || k == 3) && w < 4) {
                    report_unimpl(gpc, &I);
                    break;
                } // ADC/SBB 8/16: TODO
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
                    gpc = next;
                    continue;
                }
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
                if (k != 0 && k != 1 && k != 4 && k != 5 && k != 7) {
                    report_unimpl(gpc, &I);
                    break;
                } // RCL/RCR defer
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
                    if (k == 0) {
                        report_unimpl(gpc, &I);
                        break;
                    } // ROL by CL: defer
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
                if (want_cf) {
                    int width = ssf ? 64 : 32, bit = (k == 4) ? (width - cnt) : (cnt - 1);
                    if (bit > width - 1) bit = width - 1;
                    e_lsr_i(19, 19, bit, ssf);
                    e_movconst(23, 1);
                    e_rrr(A_AND, 19, 19, 23, ssf, 0); // x19 = x86 CF bit
                    e_nzcv_save_setcf(19);
                } else
                    e_nzcv_save();
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
                    int rmv = rm_load(&I, next, w, &mem); // neg -> x16
                    e_rrr(A_SUBS, 16, 31, rmv, w == 8, 0);
                    e_nzcv_save();
                    rm_store(&I, w, 16);
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
                            e_udiv(20, 19, 22, 1);
                        } // unsigned: zero-extend divisor
                        else {
                            e_sxt(22, rmv, 4);
                            e_sdiv(20, 19, 22, 1);
                        }                          // signed: sign-extend divisor (edx:eax already 64-bit signed)
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
                    int rmv = rm_load(&I, next, w, &mem);
                    int o = mem ? 16 : I.rm_reg;
                    if (w >= 4) {
                        if (k == 0)
                            e_addi_s(o, rmv, 1, sf);
                        else
                            e_subi_s(o, rmv, 1, sf);
                        e_nzcv_save_keepC();
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
                e_mul(I.reg, rmv, 19, sf);
                gpc = next;
                continue; // flags (CF/OF on overflow) approximate -> TODO
            }
            // ---- string ops: stos (AA/AB), movs (A4/A5), lods (AC/AD). DF assumed 0 (fwd). ----
            if (op == 0xAA || op == 0xAB || op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD) {
                int w = (op & 1) ? I.opsize : 1;
                int movs = (op == 0xA4 || op == 0xA5), lods = (op == 0xAC || op == 0xAD);
                uint32_t *cbz = NULL, *top = NULL;
                if (I.rep) {
                    top = (uint32_t *)g_cp;
                    cbz = (uint32_t *)g_cp;
                    emit32(0);
                } // cbz RCX,done
                if (movs) {
                    e_load(w, 16, RSI);
                    e_store(w, 16, RDI);
                    e_addi(RSI, RSI, w, 1);
                    e_addi(RDI, RDI, w, 1);
                } else if (lods) {
                    e_load(w, RAX, RSI);
                    e_addi(RSI, RSI, w, 1);
                } else {
                    e_store(w, RAX, RDI);
                    e_addi(RDI, RDI, w, 1);
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
            if (op == 0xFC) {
                gpc = next;
                continue;
            } // cld (DF=0): we assume forward already
            if (op == 0xFD) {
                report_unimpl(gpc, &I);
                break;
            } // std (DF=1): backward string ops -> TODO
            // ---- jmp rel (E9/EB) ----
            if (op == 0xE9 || op == 0xEB) {
                emit_chain_exit(next + (uint64_t)I.imm);
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
                int cc = x86cc_to_arm(op & 0xF);
                if (cc < 0) {
                    report_unimpl(gpc, &I);
                    break;
                }
                uint64_t taken = next + (uint64_t)I.imm;
                e_nzcv_load();
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
                gpc = next;
                continue;
            } // (F3 90 = pause -> also nop)
            if (op == 0x9B) {
                gpc = next;
                continue;
            } // fwait/wait -> nop (FPU sync)
            // sahf (9E): AH -> flags. We map SF=AH.7, ZF=AH.6, CF=AH.0 into cpu->nzcv (N/Z/C).
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
            // ===== x87 FPU (D8-DF): double-precision stack emulation =====
            if (op >= 0xD8 && op <= 0xDF) {
                int reg = I.reg & 7, rm = I.rm_reg & 7;
#define FAd(d, n, m) emit32(0x1E602800u | ((m) << 16) | ((n) << 5) | (d)) /* fadd d */
#define FSd(d, n, m) emit32(0x1E603800u | ((m) << 16) | ((n) << 5) | (d)) /* fsub d */
#define FMd(d, n, m) emit32(0x1E600800u | ((m) << 16) | ((n) << 5) | (d)) /* fmul d */
#define FDd(d, n, m) emit32(0x1E601800u | ((m) << 16) | ((n) << 5) | (d)) /* fdiv d */
#define FCMPd(n, m)                                                                                                    \
    do {                                                                                                               \
        emit32(0x1E602000u | ((m) << 16) | ((n) << 5));                                                                \
        e_nzcv_save();                                                                                                 \
    } while (0)
                if (I.is_mem) {
                    emit_ea(&I, next);
                    e_mov_rr(19, 17, 1); // x19 = EA (helpers clobber x17)
                    if (op == 0xD9) {    // f32 mem
                        if (reg == 0) {
                            e_ldr_s(16, 19);
                            emit32(0x1E22C000u | (16 << 5) | 16);
                            e_fp_push(16);
                        } // fld m32
                        else if (reg == 2 || reg == 3) {
                            e_fp_ld(16, 0);
                            emit32(0x1E624000u | (16 << 5) | 16);
                            e_str_s(16, 19);
                            if (reg == 3) e_fp_settop(1);
                        }                    // fst/fstp
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
                            e_fp_push(16);
                        } // fld m64
                        else if (reg == 2 || reg == 3) {
                            e_fp_ld(16, 0);
                            e_str_d(16, 19);
                            if (reg == 3) e_fp_settop(1);
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
                            e_fp_push(16);
                        } // fild m32
                        else if (reg == 2 || reg == 3) {
                            e_fp_ld(16, 0);
                            emit32(0x1E780000u | (16 << 5) | 16);
                            emit32(0xB9000000u | (19 << 5) | 16);
                            if (reg == 3) e_fp_settop(1);
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
                            e_fp_push(16);
                        } // fild m16 (ldrsh)
                        else if (reg == 3) {
                            e_fp_ld(16, 0);
                            emit32(0x1E780000u | (16 << 5) | 16);
                            emit32(0x79000000u | (19 << 5) | 16);
                            e_fp_settop(1);
                        } // fistp m16
                        else if (reg == 5) {
                            e_ldr(16, 19, 0);
                            emit32(0x9E620000u | (16 << 5) | 16);
                            e_fp_push(16);
                        } // fild m64
                        else if (reg == 7) {
                            e_fp_ld(16, 0);
                            emit32(0x9E780000u | (16 << 5) | 16);
                            e_str(16, 19, 0);
                            e_fp_settop(1);
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
                            e_fp_ld(18, 0);
                            e_fcom_setfpsw(18, 16);
                            if (reg == 3) e_fp_settop(1);
                            gpc = next;
                            continue;
                        } // fcom/fcomp
                        e_fp_ld(18, 0);
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
                        e_fp_st(18, 0);
                    }
                    gpc = next;
                    continue;
                }
                // ---- register forms (mod=3) ----
                if (op == 0xD9) {
                    if (reg == 0) {
                        e_fp_ld(16, rm);
                        e_fp_push(16);
                    } // fld ST(i)
                    else if (reg == 1) {
                        e_fp_ld(16, 0);
                        e_fp_ld(18, rm);
                        e_fp_st(18, 0);
                        e_fp_st(16, rm);
                    } // fxch
                    else if (reg == 4 && rm == 0) {
                        e_fp_ld(16, 0);
                        emit32(0x1E614000u | (16 << 5) | 16);
                        e_fp_st(16, 0);
                    } // fchs
                    else if (reg == 4 && rm == 1) {
                        e_fp_ld(16, 0);
                        emit32(0x1E60C000u | (16 << 5) | 16);
                        e_fp_st(16, 0);
                    }                    // fabs
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
                        e_fp_push(16);
                    } else if (reg == 7 && rm == 2) {
                        e_fp_ld(16, 0);
                        emit32(0x1E61C000u | (16 << 5) | 16);
                        e_fp_st(16, 0);
                    } // fsqrt
                    else {
                        report_unimpl(gpc, &I);
                        break;
                    }
                } else if (op == 0xD8 || op == 0xDC || op == 0xDE) { // arith ST0/ST(i) [+pop for DE]
                    e_fp_ld(18, 0);
                    e_fp_ld(16, rm);                   // v18=ST0, v16=ST(rm)
                    int dst_i = (op == 0xD8) ? 0 : rm; // D8 -> ST0; DC/DE -> ST(i)
                    if (reg == 2 || reg == 3) {
                        e_fcom_setfpsw(18, 16);
                        if (op == 0xDE && rm == 1) e_fp_settop(1);
                        if (reg == 3) e_fp_settop(1);
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
                    e_fp_st(a, dst_i);
                    if (op == 0xDE) e_fp_settop(1); // pop
                } else if (op == 0xDD) {
                    if (reg == 0) { /* ffree: no tag tracking -> nop */
                    } else if (reg == 2) {
                        e_fp_ld(16, 0);
                        e_fp_st(16, rm);
                    } // fst ST(i)
                    else if (reg == 3) {
                        e_fp_ld(16, 0);
                        e_fp_st(16, rm);
                        e_fp_settop(1);
                    } // fstp ST(i)
                    else if (reg == 4 || reg == 5) {
                        e_fp_ld(18, 0);
                        e_fp_ld(16, rm);
                        e_fcom_setfpsw(18, 16);
                        if (reg == 5) e_fp_settop(1);
                    } // fucom[p]
                    else {
                        report_unimpl(gpc, &I);
                        break;
                    }
                } else if (op == 0xDB) {
                    if (reg == 4 && rm == 3) {
                        e_movconst(16, 0);
                        e_str(16, 28, OFF_FPTOP);
                    }                    // finit -> top=0
                    else if (reg == 4) { /* fclex/etc */
                    } else if (reg == 5 || reg == 6) {
                        e_fp_ld(18, 0);
                        e_fp_ld(16, rm);
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
                        e_fp_ld(18, 0);
                        e_fp_ld(16, rm);
                        FCMPd(18, 16);
                        e_fp_settop(1);
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
                        e_fp_ld(18, 0);
                        e_fp_ld(16, rm); // v18=ST0, v16=ST(i)
                        emit32(0x1E600C00u | (18 << 16) | ((armc & 0xF) << 12) | (16 << 5) |
                               17); // fcsel d17, STi, ST0, cond
                        e_fp_st(17, 0);
                    } else if (reg == 5 && rm == 1) { // DA E9: fucompp (compare ST0,ST1; pop twice)
                        e_fp_ld(18, 0);
                        e_fp_ld(16, 1);
                        e_fcom_setfpsw(18, 16);
                        e_fp_settop(1);
                        e_fp_settop(1);
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
                emit_exit_const(next, R_SYSCALL);
                break;
            } // syscall
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
                            emit32(0x6E040420u | (vd << 5) | vm);
                        else
                            emit32(0x6E040420u | (vm << 5) | vd);
                    }                                               // ins .s[0]
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
                            emit32(0x6E080420u | (vd << 5) | vm);
                        else
                            emit32(0x6E080420u | (vm << 5) | vd);
                    }                                  // ins .d[0]
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
                    int hi = I.rep;     // F3 shuffles the HIGH 4 words, F2 the LOW 4
                    e_vmov(17, s);      // v17 = src (the un-shuffled half is preserved)
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
                    uint32_t b = op == 0xDE   ? 0x6E20A400u
                                 : op == 0xDA ? 0x6E20AC00u
                                 : op == 0xEE ? 0x4E60A400u
                                              : 0x4E60AC00u;
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
                } else if (op == 0x14 || op == 0x15) { // unpckl/hp{s,d}: interleave float lanes -> ZIP1/ZIP2
                    int s = I.is_mem ? 16 : vm;
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_ldr_q(16, 17, 0);
                    }
                    int hi = (op == 0x15);   // unpckh* -> ZIP2
                    int sz = I.p66 ? 3 : 2;  // 66=pd (64-bit lanes, .2d); none=ps (32-bit lanes, .4s)
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
                    if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    uint32_t sz = (op == 0x6B) ? 1u : 0u;                  // source element: 0x6B = 16-bit, else 8-bit dest
                    uint32_t lo = (op == 0x67) ? 0x2E212800u               // SQXTUN  (signed->unsigned narrow)
                                              : 0x0E214800u;               // SQXTN   (signed->signed narrow)
                    uint32_t hi = (op == 0x67) ? 0x6E212800u : 0x4E214800u; // ...2 (Q=1, high half)
                    emit32(lo | (sz << 22) | (vd << 5) | 17);             // narrow dst's lanes -> v17 low
                    emit32(hi | (sz << 22) | (s << 5) | 17);              // narrow src's lanes -> v17 high
                    e_vmov(vd, 17);
                } else if (op == 0xD7) {       // pmovmskb: byte MSBs -> GPR (reg)
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
                    uint32_t fop = (op == 0x2C) ? 0x1E380000u : 0x1E200000u; // FCVTZS (trunc) : FCVTNS (round)
                    emit32(fop | (I.rexW ? 0x80000000u : 0) | (I.repne ? 0x00400000u : 0) | (s << 5) | I.reg);
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
                    } else { // scalar FP: F2=double, F3=single
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
                } else if (op == 0x5A) {                             // cvtsd2ss(F2) / cvtss2sd(F3)
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
                    uint32_t EQ = (packed ? 0x4E20E400u : 0x5E20E400u) | szb;  // FCMEQ
                    uint32_t GE = (packed ? 0x6E20E400u : 0x7E20E400u) | szb;  // FCMGE
                    uint32_t GT = (packed ? 0x6EA0E400u : 0x7EA0E400u) | szb;  // FCMGT
                    uint32_t ANDb = packed ? 0x4E201C00u : 0x0E201C00u;        // AND Vd.16b/8b
                    uint32_t NOTb = packed ? 0x6E205800u : 0x2E205800u;        // NOT (MVN) Vd.16b/8b
                    if (pred == 3 || pred == 7) {                              // UNORD/ORD: ordered(a)&ordered(b)
                        emit32(EQ | (vd << 16) | (vd << 5) | 17);              // v17 = a==a (ordered a)
                        emit32(EQ | (s << 16) | (s << 5) | vd);                // vd  = b==b (ordered b)
                        emit32(ANDb | (17 << 16) | (vd << 5) | vd);            // vd  = ORD
                        if (pred == 3)
                            emit32(NOTb | (vd << 5) | vd);                     // UNORD = ~ORD
                    } else {
                        int swap = (pred == 1 || pred == 2);                   // LT/LE: a<b == b>a -> swap
                        int n = swap ? s : vd, m = swap ? vd : s;
                        uint32_t fc = (pred == 0 || pred == 4) ? EQ : (pred == 1 || pred == 6) ? GT : GE;
                        emit32(fc | (m << 16) | (n << 5) | vd);                // FCMxx vd, n, m
                        if (pred == 4)
                            emit32(NOTb | (vd << 5) | vd);                     // NEQ = ~EQ
                    }
                } else if (op == 0x2E || op == 0x2F) {       // ucomisd/comisd (66=double, none=single) -> FCMP + flags
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
                    e_nzcv_save(); // CF/ZF/PF substrate: ARM FCMP C/Z align with x86 unsigned cc
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
            }                             // cpuid -> dispatcher helper
            if (op == 0x31) {             // rdtsc: edx:eax = cntvct
                emit32(0xD53BE040u | 16); // mrs x16, cntvct_el0
                e_mov_rr(RAX, 16, 0);
                e_lsr_i(RDX, 16, 32, 1);
                gpc = next;
                continue;
            }
            if (op == 0x01 && I.has_modrm && I.modrm == 0xD0) { // xgetbv (ecx=0): XCR0 = x87+SSE (no AVX)
                e_movz(RAX, 3, 0);
                e_movz(RDX, 0, 0);
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
                e_mul(I.reg, I.reg, rmv, sf);
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
                if (sub == 2) {
                    gpc = next;
                    continue;
                } // ldmxcsr: ignore (no SSE rounding/excepts)
                if (sub == 3) {
                    if (I.is_mem) {
                        emit_ea(&I, next);
                        e_movconst(16, 0x1f80);
                        e_store(4, 16, 17);
                    }
                    gpc = next;
                    continue;
                }                                         // stmxcsr
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
            // bsf/tzcnt (0F BC), bsr/lzcnt (0F BD): bit scan -> RBIT+CLZ / CLZ
            if (op == 0xBC || op == 0xBD) {
                int mem;
                int rmv = rm_load(&I, next, I.opsize, &mem);
                if (op == 0xBC) {
                    e_rbit(I.reg, rmv, sf);
                    e_clz(I.reg, I.reg, sf);
                } // bsf = ctz
                else {
                    e_clz(16, rmv, sf);
                    e_movconst(19, sf ? 63 : 31);
                    e_rrr(A_SUB, I.reg, 19, 16, sf, 0);
                } // bsr = (w-1)-clz
                e_rrr(A_ANDS, 31, rmv, rmv, sf, 0);
                e_nzcv_save(); // ZF = (src == 0)
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
                int val = rm_load(&I, next, w, &mem);
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
                int cc = x86cc_to_arm(op & 0xF);
                if (cc < 0) {
                    report_unimpl(gpc, &I);
                    break;
                }
                uint64_t taken = next + (uint64_t)I.imm;
                e_nzcv_load();
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
    map_put(start, host, body);
    if (!g_threaded) patch_links_to(start, body); // chaining mutates live blocks -> off when threaded
    return host;
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
    e_movconst(16, (uint64_t)block_return);
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
