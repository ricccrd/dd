// dd/runtime/frontend/x86_64 -- integer ALU instruction class, lifted VERBATIM out of translate_block's
// one-byte switch (behavior-preserving move): ADD/OR/ADC/SBB/AND/SUB/XOR/CMP primary /r forms (00..3D),
// the acc,imm forms (04/05..3C/3D), group1 r/m,imm (80/81/83), and TEST (84/85, A8/A9). Flag production is
// unchanged -- do_alu()/narrow_adcsbb() still drive the lazy-flag state (g_fl_pending) exactly as before;
// the top-of-loop flag classifier in translate_block runs before this dispatch, untouched. Every handler
// here advances past the insn, so the helper only ever returns TX_FALL (not an ALU op -> caller falls
// through) or TX_NEXT (caller: gpc = next; continue). #included after x87.c (uses do_alu/rm_load/rm_store/
// narrow_adcsbb/byte_val/byte_wb/lock_rmw/alu_kind_primary, all defined above) and before translate_block.
static int translate_alu(struct insn *I, uint64_t next) {
    uint8_t op = I->op;
            // ---- ALU primary (00..3D): /r reg,r/m forms ----
            // gate on op<0x40: bits[7:6]==00 is primary ALU. 0x80-0x83 (group1) handled below.
            if (op < 0x40 && (op & 7) <= 3 && alu_kind_primary(op) >= 0) {
                int k = alu_kind_primary(op), dir = op & 2; // dir 0: r/m,reg ; 2: reg,r/m
                int w = (op & 1) ? I->opsize : 1, mem;
                if ((k == 2 || k == 3) && w < 4) { // byte/word ADC/SBB -> narrow_adcsbb (do_alu is 32/64 only)
                    int adc = (k == 2), m2;
                    int rmv2 = rm_load(I, next, w, &m2);
                    int regv2 = (w == 1) ? byte_val(I, I->reg, 24) : I->reg;
                    if (dir) { // dst = reg
                        narrow_adcsbb(adc, 16, regv2, rmv2, w);
                        if (w == 1)
                            byte_wb(I, I->reg, 16);
                        else
                            e_bfi(I->reg, 16, 0, 8 * w, 1);
                    } else { // dst = r/m
                        narrow_adcsbb(adc, 16, rmv2, regv2, w);
                        rm_store(I, w, 16);
                    }
                    return TX_NEXT;
                }
                int rmv = rm_load(I, next, w, &mem);
                int regv = (w == 1) ? byte_val(I, I->reg, 24) : I->reg; // reg operand value (handle ah/ch)
                if (dir) {                                             // dst = reg
                    if (k == 7)
                        do_alu(7, -1, regv, rmv, w); // cmp: no write
                    else if (w == 1) {
                        do_alu(k, 16, regv, rmv, w);
                        byte_wb(I, I->reg, 16);
                    } else
                        do_alu(k, I->reg, I->reg, rmv, w);
                } else { // dst = r/m
                    if (!(I->lock && mem && k != 7 && lock_rmw(k, w, regv))) {
                        do_alu(k, (k == 7) ? -1 : 16, rmv, regv, w);
                        if (k != 7) rm_store(I, w, 16);
                    }
                }
                return TX_NEXT;
            }
            // ALU al/eax/rax, imm (04/05 ... 3C/3D)
            if (op < 0x40 && ((op & 7) == 4 || (op & 7) == 5) && alu_kind_primary(op) >= 0) {
                int k = alu_kind_primary(op), w = (op & 7) == 4 ? 1 : I->opsize;
                if (!((k == 2 || k == 3) && w < 4)) {
                    e_movconst(16, (uint64_t)I->imm);
                    do_alu(k, k == 7 ? -1 : RAX, RAX, 16, w);
                } else { // byte/word ADC/SBB al/ax, imm -- do_alu is 32/64-only, mirror the group1 narrow path
                    e_movconst(19, (uint64_t)I->imm);
                    narrow_adcsbb(k == 2, 16, RAX, 19, w);
                    e_bfi(RAX, 16, 0, 8 * w, 1); // write low w bytes into the accumulator, preserve upper
                }
                return TX_NEXT;
            }
            // ---- group1 (80/81/83): ALU r/m, imm ----
            if (op == 0x80 || op == 0x81 || op == 0x83) {
                int k = I->reg & 7, w = op == 0x80 ? 1 : I->opsize, mem;
                if (!((k == 2 || k == 3) && w < 4)) {     // ADC/SBB ok for 32/64-bit
                    int rmv = rm_load(I, next, w, &mem); // mem -> x16 (val), x17 (EA)
                    if (I->lock && mem && k != 7) {        // LOCK op [mem], imm -> atomic (e.g. lock add $1)
                        e_movconst(21, (uint64_t)I->imm);  // operand in x21 (x19/x20 are lock_rmw scratch)
                        if (lock_rmw(k, w, 21)) {
                            return TX_NEXT;
                        }
                    }
                    e_movconst(19, (uint64_t)I->imm); // imm in x19 (x16 holds the loaded value)
                    // compute into scratch x16, then rm_store -> correct dest (handles mem + hi/lo byte regs)
                    do_alu(k, (k == 7) ? -1 : 16, rmv, 19, w);
                    if (k != 7) rm_store(I, w, 16);
                    return TX_NEXT;
                } else { // byte/word ADC/SBB r/m, imm
                    int adc = (k == 2);
                    int rmv = rm_load(I, next, w, &mem); // value -> x16
                    e_movconst(19, (uint64_t)I->imm);      // imm in x19 (narrow_adcsbb reads b before clobbering x19)
                    narrow_adcsbb(adc, 16, rmv, 19, w);
                    rm_store(I, w, 16);
                    return TX_NEXT;
                }
            }
            // ---- test (84/85, A8/A9, F6/F7 /0) ----
            if (op == 0x84 || op == 0x85) {
                int w = (op & 1) ? I->opsize : 1, mem;
                int rmv = rm_load(I, next, w, &mem);
                int regv = (w == 1) ? byte_val(I, I->reg, 24) : I->reg; // reg operand: handle ah/bh/ch/dh
                do_alu(4, -1, rmv, regv, w);
                return TX_NEXT; // test = and(discard)
            }
            if (op == 0xA8 || op == 0xA9) {
                int w = op == 0xA8 ? 1 : I->opsize;
                e_movconst(16, (uint64_t)I->imm);
                do_alu(4, -1, RAX, 16, w);
                return TX_NEXT;
            }
    return TX_FALL;
}
