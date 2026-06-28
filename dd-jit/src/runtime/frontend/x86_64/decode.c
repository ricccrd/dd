// dd/runtime/frontend/x86_64 -- the x86-64 instruction decoder (prefixes, ModRM/SIB, the insn IR).

// ---------------- x86-64 decoder ----------------
struct insn {
    int len;
    int rexW, rexR, rexX, rexB, has_rex;
    int opsize; // operand size in bytes: 1/2/4/8
    int p66;    // 0x66 prefix seen (mandatory-prefix for SSE; distinct from opsize)
    int addr32; // 0x67
    int seg;    // 0 none, 1 fs, 2 gs
    int lock, rep, repne;
    int two;    // 0F escape seen
    uint8_t op; // opcode byte after escape
    int has_modrm;
    uint8_t modrm;
    int mod, reg, rm;
    int is_mem; // operand-in-memory (mod != 3)
    int m_base, m_index, m_scale;
    int64_t disp;
    int rip_rel;
    int m_hasbase, m_hasindex;
    int rm_reg; // when mod==3: the r/m register number
    int64_t imm;
    int imm_bytes;
};

static int op_has_modrm(int two, uint8_t op) {
    if (two) {
        if (op == 0x05) return 0;                             // syscall
        if (op == 0xA2 || op == 0x31 || op == 0x77) return 0; // cpuid / rdtsc / emms (no modrm)
        if (op >= 0xC8 && op <= 0xCF) return 0;               // bswap reg (encoded in opcode)
        if (op == 0x1E) return 1;                             // endbr (modrm follows)
        if ((op & 0xF0) == 0x80) return 0;                    // jcc rel32
        if ((op & 0xF0) == 0x90) return 1;                    // setcc
        if ((op & 0xF0) == 0x40) return 1;                    // cmovcc
        if (op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF || op == 0xAF) return 1; // movzx/sx/imul
        if (op == 0x1F) return 1;                                                         // nop r/m
        if (op == 0x10 || op == 0x11 || op == 0x28 || op == 0x29 || op == 0x6E || op == 0x7E || op == 0x6F ||
            op == 0x7F || op == 0xD6 || op == 0xEF || op == 0x57 || op == 0x54)
            return 1; // SSE
        return 1;
    }
    if (op >= 0x50 && op <= 0x5F) return 0;                             // push/pop r
    if (op >= 0x70 && op <= 0x7F) return 0;                             // jcc rel8
    if (op == 0xE8 || op == 0xE9 || op == 0xEB || op == 0xE3) return 0; // call/jmp rel, jrcxz
    if (op == 0xC3 || op == 0xC2 || op == 0xC9 || op == 0x90 || op == 0xF4 || op == 0x99 || op == 0x98) return 0;
    if (op >= 0x91 && op <= 0x97) return 0;                                           // xchg eax, rN
    if (op == 0x9B || op == 0x9C || op == 0x9D || op == 0x9E || op == 0x9F) return 0; // fwait/pushf/popf/sahf/lahf
    if (op == 0x9C || op == 0x9D || op == 0xFC || op == 0xFD || op == 0xCC || op == 0xF5)
        return 0;                                                // pushf/popf/cld/std/int3/cmc
    if (op >= 0xA4 && op <= 0xAF) return 0;                      // movs/cmps/stos/lods/scas + test al,imm(A8/A9)
    if (op >= 0xB0 && op <= 0xBF) return 0;                      // mov r8/r, imm
    if (op < 0x40 && ((op & 7) == 4 || (op & 7) == 5)) return 0; // ALU al/eAX, imm (04/05,0C/0D,...,3C/3D)
    if (op == 0xA8 || op == 0xA9) return 0;                      // test al/eax, imm
    if (op == 0x68 || op == 0x6A) return 0;                      // push imm
    if (op == 0xCC || op == 0xF1) return 0;
    // ALU group, mov, lea, test, group1/2/3, etc. all have modrm
    return 1;
}
// immediate size (bytes) for the opcodes we handle; 0 if none.
static int op_imm_bytes(struct insn *I) {
    int two = I->two;
    uint8_t op = I->op;
    int os = I->opsize;
    if (two) {
        if ((op & 0xF0) == 0x80) return 4;      // jcc rel32
        if (op == 0xBA) return 1;               // bt/bts/btr/btc r/m, imm8
        if (op == 0xA4 || op == 0xAC) return 1; // shld/shrd r/m, r, imm8
        if (op == 0x70 || op == 0x71 || op == 0x72 || op == 0x73 || op == 0xC2 || op == 0xC4 || op == 0xC5 ||
            op == 0xC6)
            return 1; // SSE imm
        return 0;
    }
    if (op == 0xC2) return 2;                                             // ret imm16
    if (op >= 0x70 && op <= 0x7F) return 1;                               // jcc rel8
    if (op == 0xEB || op == 0xE3) return 1;                               // jmp rel8 / jrcxz rel8
    if (op == 0xE9 || op == 0xE8) return 4;                               // jmp/call rel32
    if (op >= 0xB0 && op <= 0xB7) return 1;                               // mov r8, imm8
    if (op >= 0xB8 && op <= 0xBF) return os == 8 ? 8 : (os == 2 ? 2 : 4); // mov r,imm (movabs if W)
    if (op < 0x40 && (op & 7) == 4) return 1;                             // ALU al, imm8
    if (op < 0x40 && (op & 7) == 5) return os == 2 ? 2 : 4;               // ALU eAX, imm16/32
    if (op == 0xA8) return 1;
    if (op == 0xA9) return os == 2 ? 2 : 4; // test
    if (op == 0x6A) return 1;
    if (op == 0x68) return os == 2 ? 2 : 4;       // push imm
    if (op == 0x80) return 1;                     // group1 r/m8, ib
    if (op == 0x81) return os == 2 ? 2 : 4;       // group1 r/m, iz
    if (op == 0x83) return 1;                     // group1 r/m, ib (sign-ext)
    if (op == 0xC6) return 1;                     // mov r/m8, ib
    if (op == 0xC7) return os == 2 ? 2 : 4;       // mov r/m, iz
    if (op == 0xC0 || op == 0xC1) return 1;       // shift r/m, ib
    if (op == 0xF6) return (I->reg <= 1) ? 1 : 0; // test r/m8,ib only for /0,/1
    if (op == 0xF7) return (I->reg <= 1) ? (os == 2 ? 2 : 4) : 0;
    if (op == 0x69) return os == 2 ? 2 : 4; // imul r,r/m,iz
    if (op == 0x6B) return 1;               // imul r,r/m,ib
    return 0;
}

// returns instruction length, fills I. On a decode it can't handle for length, returns
// the bytes consumed so far so the reporter can show them.
static int decode(uint64_t pc, struct insn *I) {
    memset(I, 0, sizeof *I);
    const uint8_t *p = (const uint8_t *)pc;
    int n = 0;
    I->opsize = 4;
    I->m_scale = 0;
    // legacy prefixes
    for (;;) {
        uint8_t b = p[n];
        if (b == 0x66) {
            I->opsize = 2;
            I->p66 = 1;
            n++;
            continue;
        }
        if (b == 0x67) {
            I->addr32 = 1;
            n++;
            continue;
        }
        if (b == 0xF0) {
            I->lock = 1;
            n++;
            continue;
        }
        if (b == 0xF2) {
            I->repne = 1;
            n++;
            continue;
        }
        if (b == 0xF3) {
            I->rep = 1;
            n++;
            continue;
        }
        if (b == 0x64) {
            I->seg = 1;
            n++;
            continue;
        } // fs
        if (b == 0x65) {
            I->seg = 2;
            n++;
            continue;
        } // gs
        if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26) {
            n++;
            continue;
        }
        break;
    }
    // REX
    if ((p[n] & 0xF0) == 0x40) {
        uint8_t rex = p[n++];
        I->has_rex = 1;
        I->rexW = (rex >> 3) & 1;
        I->rexR = (rex >> 2) & 1;
        I->rexX = (rex >> 1) & 1;
        I->rexB = rex & 1;
        if (I->rexW) I->opsize = 8;
    }
    // opcode
    uint8_t op = p[n++];
    if (op == 0x0F) {
        I->two = 1;
        op = p[n++];
    }
    I->op = op;
    // modrm + sib + disp
    if (op_has_modrm(I->two, op)) {
        uint8_t m = p[n++];
        I->has_modrm = 1;
        I->modrm = m;
        I->mod = m >> 6;
        I->reg = ((m >> 3) & 7) | (I->rexR << 3);
        I->rm = m & 7;
        if (I->mod == 3) {
            I->rm_reg = I->rm | (I->rexB << 3);
        } else {
            I->is_mem = 1;
            int base = I->rm, idx = -1, scale = 0;
            if (I->rm == 4) { // SIB
                uint8_t s = p[n++];
                scale = s >> 6;
                idx = ((s >> 3) & 7) | (I->rexX << 3);
                base = (s & 7);
                if (((s >> 3) & 7) == 4 && !I->rexX) idx = -1; // no index
                if ((s & 7) == 5 && I->mod == 0) {
                    I->m_hasbase = 0;
                } else {
                    I->m_hasbase = 1;
                    I->m_base = base | (I->rexB << 3);
                }
            } else if (I->rm == 5 && I->mod == 0) { // RIP-relative
                I->rip_rel = 1;
            } else {
                I->m_hasbase = 1;
                I->m_base = I->rm | (I->rexB << 3);
            }
            if (idx >= 0) {
                I->m_hasindex = 1;
                I->m_index = idx;
                I->m_scale = scale;
            }
            // displacement
            if (I->rip_rel) {
                I->disp = (int32_t)(p[n] | (p[n + 1] << 8) | (p[n + 2] << 16) | ((uint32_t)p[n + 3] << 24));
                n += 4;
            } else if (I->mod == 1) {
                I->disp = (int8_t)p[n];
                n += 1;
            } else if (I->mod == 2 || (!I->m_hasbase && I->rm == 4)) {
                I->disp = (int32_t)(p[n] | (p[n + 1] << 8) | (p[n + 2] << 16) | ((uint32_t)p[n + 3] << 24));
                n += 4;
            }
        }
    }
    // immediate
    int ib = op_imm_bytes(I);
    I->imm_bytes = ib;
    if (ib) {
        uint64_t v = 0;
        for (int i = 0; i < ib; i++)
            v |= (uint64_t)p[n + i] << (8 * i);
        I->imm = (ib == 1) ? (int8_t)v : (ib == 2) ? (int16_t)v : (ib == 4) ? (int32_t)v : (int64_t)v;
        n += ib;
    }
    I->len = n;
    return n;
}

// x17 += d  (signed displacement), folded into add/sub-immediate(s) instead of a 64-bit
// movconst+add.  d is the sign-extension of an x86 disp8/disp32, so |d| <= 2^31.
// imm12 (1 insn) covers all disp8 and small disp32; 24-bit (2 insns via LSL#12) covers most
// disp32; only the rare >=2^24 case materializes the constant (1:1 mapping, no guest base).
static void ea_add_disp(int64_t d) {
    if (d == 0) return;
    int sub = d < 0;
    uint64_t a = sub ? (uint64_t)(-d) : (uint64_t)d;
    if (a <= 0xFFFu) {
        if (sub)
            e_subi(17, 17, (unsigned)a, 1);
        else
            e_addi(17, 17, (unsigned)a, 1);
    } else if (a <= 0xFFFFFFu) {
        unsigned lo = (unsigned)(a & 0xFFF), hi = (unsigned)((a >> 12) & 0xFFF);
        if (sub) {
            e_subi_sh(17, 17, hi, 1, 1);
            if (lo) e_subi(17, 17, lo, 1);
        } else {
            e_addi_sh(17, 17, hi, 1, 1);
            if (lo) e_addi(17, 17, lo, 1);
        }
    } else {
        e_movconst(16, (uint64_t)d);
        e_rrr(A_ADD, 17, 17, 16, 1, 0);
    }
}
// Compute the effective address of a memory operand into host scratch x17.
// (base + index*scale + disp, + fs/gs base; RIP-relative -> a constant.)
// Address-gen fast path: fold base, index<<scale and disp into the fewest host insns --
// base+index in one shifted add, disp in an add/sub-immediate (no per-access constant build).
// NOEAOPT=1 reverts to the exact baseline lowering (movconst-built disp + base+0 add).
static void emit_ea(struct insn *I, uint64_t next_rip) {
    if (noeaopt()) { // exact baseline lowering
        if (I->rip_rel) {
            e_movconst(17, next_rip + (uint64_t)I->disp);
        } else {
            if (I->m_hasbase)
                e_mov_rr(17, I->m_base, 1);
            else
                e_movz(17, 0, 0);
            if (I->m_hasindex) e_rrr(A_ADD, 17, 17, I->m_index, 1, I->m_scale); // add x17,x17,idx,lsl#scale
            if (I->disp) {
                e_movconst(16, (uint64_t)I->disp);
                e_rrr(A_ADD, 17, 17, 16, 1, 0);
            }
        }
    } else if (I->rip_rel) {
        e_movconst(17, next_rip + (uint64_t)I->disp);
    } else if (I->m_hasbase) {
        if (I->m_hasindex) {
            e_rrr(A_ADD, 17, I->m_base, I->m_index, 1, I->m_scale); // x17 = base + idx<<scale
            ea_add_disp(I->disp);
        } else if (I->disp >= 0 && I->disp <= 0xFFF) {
            e_addi(17, I->m_base, (unsigned)I->disp, 1); // x17 = base + #disp   (1 insn)
        } else if (I->disp < 0 && -I->disp <= 0xFFF) {
            e_subi(17, I->m_base, (unsigned)(-I->disp), 1); // x17 = base - #disp  (1 insn)
        } else {
            e_mov_rr(17, I->m_base, 1);
            ea_add_disp(I->disp);
        }
    } else if (I->m_hasindex) {
        e_rrr(A_ADD, 17, 31, I->m_index, 1, I->m_scale); // x17 = idx<<scale   (add from xzr)
        ea_add_disp(I->disp);
    } else {
        e_movconst(17, (uint64_t)I->disp); // absolute [disp]
    }
    if (I->seg) {
        e_ldr(16, 28, I->seg == 1 ? OFF_FS : OFF_GS);
        e_rrr(A_ADD, 17, 17, 16, 1, 0);
    }
}

// Decide whether a [base+disp] (no index/seg/rip) memory operand can be folded directly
// into one ldr/str addressing mode. Returns 0 = no fold; 1 = scaled unsigned imm; 2 = unscaled
// signed imm. On success sets *rn = host base reg, *off = displacement to pass to the encoder.
// NOEAOPT=1 forces 0 (no fold) -> the caller uses the baseline emit_ea + e_load/e_store.
static int ea_imm_fold(struct insn *I, int w, int *rn, int *off) {
    if (noeaopt()) return 0;
    if (!(I->m_hasbase && !I->m_hasindex && !I->seg && !I->rip_rel)) return 0;
    int64_t d = I->disp;
    *rn = I->m_base;
    *off = (int)d;
    if (d >= 0 && (d % w) == 0 && (uint64_t)(d / w) <= 0xFFFu) return 1; // scaled: ldr[rn,#d]
    if (d >= -256 && d <= 255) return 2;                                 // unscaled: ldur[rn,#d]
    return 0;
}
// Single-instruction folded zero-extending load of a memory operand into rt.
// Falls back to emit_ea + e_load for index/seg/rip/large-disp forms (identical to before).
static void emit_load_mem(struct insn *I, uint64_t next, int w, int rt) {
    int rn, off, f = ea_imm_fold(I, w, &rn, &off);
    if (f == 1)
        e_load_uoff(w, rt, rn, (unsigned)off);
    else if (f == 2)
        e_ldur(w, rt, rn, off);
    else {
        emit_ea(I, next);
        e_load(w, rt, 17);
    }
}
