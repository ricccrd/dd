// frontend/x86_64/avx.c -- AVX/AVX2/AVX-512 (VEX/EVEX) emulation.
//
// The decoder (decode.c) recognises C5/C4/62-prefixed instructions and the translator exits the block to C
// (R_AVX) at each one rather than lowering it to NEON. do_avx() re-decodes the single instruction at
// cpu->rip, emulates it against the guest register file (v[]=xmm low128, vhi[], vz[], vx[], kreg[]) and
// memory, then advances rip past it. Correctness-first: one block exit per AVX insn. Unknown ops report
// the (map, opcode, pp) and exit 70 so coverage can be grown test-driven.
//
// Register model: a logical zmm is 64 bytes. For regs 0..15: [0:16)=v[2r..], [16:32)=vhi, [32:64)=vz.
// For 16..31: vx[]. VEX/EVEX writes ZERO all bits above the operation width (128/256/512) -- avx_put does
// this, so a 128-bit VEX op clears bits[128:512) (the AVX upper-zeroing rule).

static int g_avx_warned;

static void avx_get(struct cpu *c, int r, uint8_t out[64]) {
    if (r < 16) {
        memcpy(out + 0, &c->v[2 * r], 16);
        memcpy(out + 16, &c->vhi[2 * r], 16);
        memcpy(out + 32, &c->vz[4 * r], 32);
    } else {
        memcpy(out, &c->vx[8 * (r - 16)], 64);
    }
}
static void avx_put(struct cpu *c, int r, const uint8_t in[64], int wbytes) {
    uint8_t b[64];
    memset(b, 0, 64);
    memcpy(b, in, wbytes); // zero-extend above the op width (VEX upper-zeroing)
    if (r < 16) {
        memcpy(&c->v[2 * r], b + 0, 16);
        memcpy(&c->vhi[2 * r], b + 16, 16);
        memcpy(&c->vz[4 * r], b + 32, 32);
    } else {
        memcpy(&c->vx[8 * (r - 16)], b, 64);
    }
}
// Effective address of a memory operand. Guest pointers are host pointers in the in-process model (PIE
// images load 1:1). EVEX disp8 is compressed (disp8*N); for the common full-vector tuple N = vector bytes.
static uint64_t avx_ea(struct cpu *c, struct insn *I, uint64_t rip_after, int wbytes) {
    if (I->rip_rel) return rip_after + (uint64_t)I->disp;
    uint64_t a = 0;
    if (I->m_hasbase) a += c->r[I->m_base];
    if (I->m_hasindex) a += c->r[I->m_index] << I->m_scale;
    int64_t disp = I->disp;
    if (I->evex && I->mod == 1) disp *= wbytes ? wbytes : 1; // disp8*N, full-vector tuple
    a += (uint64_t)disp;
    if (I->seg == 1) a += c->fs_base;
    else if (I->seg == 2) a += c->gs_base;
    return a;
}
// Read the r/m operand (register or memory) into buf as `wbytes` bytes.
static void avx_get_rm(struct cpu *c, struct insn *I, uint64_t rip_after, int wbytes, uint8_t buf[64]) {
    memset(buf, 0, 64);
    if (I->is_mem) {
        uint64_t a = avx_ea(c, I, rip_after, wbytes);
        memcpy(buf, (void *)a, wbytes);
    } else {
        uint8_t t[64];
        avx_get(c, I->rm_reg, t);
        memcpy(buf, t, wbytes);
    }
}
static void avx_put_rm(struct cpu *c, struct insn *I, uint64_t rip_after, int wbytes, const uint8_t buf[64]) {
    if (I->is_mem) {
        uint64_t a = avx_ea(c, I, rip_after, wbytes);
        memcpy((void *)a, buf, wbytes);
    } else {
        avx_put(c, I->rm_reg, buf, wbytes);
    }
}

static void do_avx(struct cpu *c) {
    struct insn I;
    decode(c->rip, &I);
    uint64_t next = c->rip + I.len;
    int L = I.vex_l;            // 0=128,1=256,2=512
    int W = (L == 0) ? 16 : (L == 1) ? 32 : 64; // operation width in bytes
    int map = I.vex_map, op = I.op, pp = I.vex_pp;
    int rd = I.reg, vv = I.vvvv;
    uint8_t a[64], b[64], d[64];

    // ---- BMI2 / BMI1: VEX-encoded but operate on GENERAL registers (not vector). Routed here by the VEX
    // decoder; handle on cpu->r[]. wbits per VEX.W. rm = ModRM.r/m (reg or mem); the 2nd source is VEX.vvvv.
    if ((map == 2 && (op == 0xf2 || op == 0xf3 || op == 0xf5 || op == 0xf6 || op == 0xf7)) ||
        (map == 3 && op == 0xf0)) {
        int wb = I.vex_w ? 64 : 32;
        uint64_t M = I.vex_w ? ~0ull : 0xffffffffull;
        uint64_t rm;
        if (I.is_mem) {
            uint64_t ea = avx_ea(c, &I, next, I.vex_w ? 8 : 4);
            rm = 0;
            memcpy(&rm, (void *)ea, I.vex_w ? 8 : 4);
        } else
            rm = c->r[I.rm_reg] & M;
        uint64_t v2 = c->r[vv] & M, res = 0;
        int setfl = 0, cf = 0, zf, sf;
        if (map == 2 && op == 0xf5 && pp == 0) { // BZHI rd, rm, vvvv: zero bits >= index(vvvv&0xff)
            int idx = (int)(v2 & 0xff);
            res = (idx >= wb) ? rm : (rm & ((idx == 0) ? 0 : ((1ull << idx) - 1)));
            cf = (idx > wb - 1);
            setfl = 1;
        } else if (map == 2 && op == 0xf7 && pp == 0) { // BEXTR rd, rm, vvvv(start:len in al:ah of vvvv)
            int start = (int)(v2 & 0xff), len = (int)((v2 >> 8) & 0xff);
            uint64_t t = (start >= wb) ? 0 : (rm >> start);
            res = (len >= wb) ? t : (t & ((len == 0) ? 0 : ((1ull << len) - 1)));
            setfl = 1;
        } else if (map == 2 && op == 0xf7 && pp == 1) { // SHLX rd, rm, vvvv
            res = rm << (v2 & (wb - 1));
        } else if (map == 2 && op == 0xf7 && pp == 2) { // SARX rd, rm, vvvv (arithmetic)
            int sh = v2 & (wb - 1);
            res = (uint64_t)(I.vex_w ? ((int64_t)rm >> sh) : ((int32_t)rm >> sh));
        } else if (map == 2 && op == 0xf7 && pp == 3) { // SHRX rd, rm, vvvv
            res = rm >> (v2 & (wb - 1));
        } else if (map == 2 && op == 0xf6 && pp == 3) { // MULX rd(hi):vvvv(lo) = rdx * rm
            unsigned __int128 p = (unsigned __int128)(c->r[RDX] & M) * (unsigned __int128)rm;
            c->r[vv] = (uint64_t)p & M;
            res = (uint64_t)(I.vex_w ? (p >> 64) : ((p >> 32) & 0xffffffff));
        } else if (map == 3 && op == 0xf0 && pp == 3) { // RORX rd, rm, imm8 (no flags)
            int sh = (int)(I.imm & (wb - 1));
            res = sh ? ((rm >> sh) | (rm << (wb - sh))) : rm;
            if (!I.vex_w) res &= M;
        } else if (map == 2 && op == 0xf5 && pp == 3) { // PEXT rd, vvvv(src), rm(mask)
            uint64_t src = v2, msk = rm, bit = 1;
            for (uint64_t m = msk; m; m &= m - 1) {
                if (src & (m & (~m + 1))) res |= bit;
                bit <<= 1;
            }
        } else if (map == 2 && op == 0xf5 && pp == 2) { // PDEP rd, vvvv(src), rm(mask)
            uint64_t src = v2, msk = rm, bit = 1;
            for (uint64_t m = msk; m; m &= m - 1) {
                if (src & bit) res |= (m & (~m + 1));
                bit <<= 1;
            }
        } else {
            goto avx_unimpl;
        }
        c->r[rd] = res & M; // 32-bit dest zero-extends to 64
        if (setfl) {        // BZHI/BEXTR set ZF/SF, CF as computed, OF=0
            zf = ((res & M) == 0);
            sf = (int)((res >> (wb - 1)) & 1);
            c->nzcv = ((uint64_t)sf << 31) | ((uint64_t)zf << 30) | ((uint64_t)(!cf) << 29);
        }
        c->rip = next;
        return;
    }
    if (getenv("AVXTRACE"))
        fprintf(stderr, "[avx] %s map=%d op=0x%02x pp=%d L=%d W=%d len=%d rd=%d vv=%d mem=%d rip=%llx\n",
                I.evex ? "EVEX" : "VEX", map, op, pp, L, I.vex_w, I.len, rd, vv, I.is_mem, (unsigned long long)c->rip);

    // ---- map 1 (0F) ----
    if (map == 1) {
        switch (op) {
        // moves: vmovups/aps (np), vmovupd/apd (66), vmovss (F3), vmovsd (F2). 10/28 load, 11/29 store.
        case 0x10:
        case 0x28: { // dst.reg <- rm
            avx_get_rm(c, &I, next, (op == 0x10 && (pp == 2 || pp == 3)) ? (pp == 2 ? 4 : 8) : W, d);
            // scalar ss/sd merge: for 0x10 F3/F2 only the low element loads, but VEX zeroes the rest.
            avx_put(c, rd, d, W);
            goto done;
        }
        case 0x11:
        case 0x29: { // rm <- dst.reg
            avx_get(c, rd, d);
            avx_put_rm(c, &I, next, (op == 0x11 && (pp == 2 || pp == 3)) ? (pp == 2 ? 4 : 8) : W, d);
            goto done;
        }
        case 0x6F: { // vmovdqa(66)/vmovdqu(F3) reg <- rm
            avx_get_rm(c, &I, next, W, d);
            avx_put(c, rd, d, W);
            goto done;
        }
        case 0x7F: { // vmovdqa/u rm <- reg
            avx_get(c, rd, d);
            avx_put_rm(c, &I, next, W, d);
            goto done;
        }
        case 0x6E: { // vmovd/vmovq gpr/mem -> xmm (zero-extend)
            int wb = I.vex_w ? 8 : 4;
            memset(d, 0, 64);
            if (I.is_mem) {
                uint64_t addr = avx_ea(c, &I, next, wb);
                memcpy(d, (void *)addr, wb);
            } else
                memcpy(d, &c->r[I.rm_reg], wb);
            avx_put(c, rd, d, 16);
            goto done;
        }
        case 0x7E: { // F3: vmovq xmm<-xmm/mem (zext); 66: vmovd/q xmm->gpr/mem
            if (pp == 2) { // F3 vmovq: reg <- rm (low 64), zero-extend
                avx_get_rm(c, &I, next, 8, d);
                avx_put(c, rd, d, 16);
            } else { // 66 vmovd/q: rm <- reg low
                int wb = I.vex_w ? 8 : 4;
                avx_get(c, rd, d);
                if (I.is_mem) {
                    uint64_t addr = avx_ea(c, &I, next, wb);
                    memcpy((void *)addr, d, wb);
                } else {
                    uint64_t v = 0;
                    memcpy(&v, d, wb);
                    c->r[I.rm_reg] = v; // 32-bit dst zero-extends to 64
                }
            }
            goto done;
        }
        case 0xD6: { // vmovq rm <- reg (low 64)
            avx_get(c, rd, d);
            avx_put_rm(c, &I, next, 8, d);
            goto done;
        }
        // logical: dst = src1 OP src2  (src1=vvvv, src2=rm). byte-wise over W.
        case 0xEF: // vpxor
        case 0xEB: // vpor
        case 0xDB: // vpand
        case 0xDF: // vpandn
        case 0x57: // vxorps/pd
        case 0x56: // vorps/pd
        case 0x54: // vandps/pd
        case 0x55: // vandnps/pd
        {
            avx_get(c, vv, a);
            avx_get_rm(c, &I, next, W, b);
            for (int i = 0; i < W; i++) {
                uint8_t x = a[i], y = b[i];
                d[i] = (op == 0xEF || op == 0x57) ? (x ^ y)
                       : (op == 0xEB || op == 0x56) ? (x | y)
                       : (op == 0xDB || op == 0x54) ? (x & y)
                                                    : (uint8_t)(~x & y); // andn
            }
            avx_put(c, rd, d, W);
            goto done;
        }
        // integer add/sub by element width
        case 0xFC:
        case 0xFD:
        case 0xFE:
        case 0xD4: // vpaddb/w/d/q
        case 0xF8:
        case 0xF9:
        case 0xFA:
        case 0xFB: // vpsubb/w/d/q
        {
            int es = (op == 0xFC || op == 0xF8) ? 1 : (op == 0xFD || op == 0xF9) ? 2 : (op == 0xFE || op == 0xFA) ? 4 : 8;
            int sub = (op >= 0xF8);
            avx_get(c, vv, a);
            avx_get_rm(c, &I, next, W, b);
            for (int i = 0; i < W; i += es) {
                uint64_t x = 0, y = 0;
                memcpy(&x, a + i, es);
                memcpy(&y, b + i, es);
                uint64_t r = sub ? (x - y) : (x + y);
                memcpy(d + i, &r, es);
            }
            avx_put(c, rd, d, W);
            goto done;
        }
        // compare-equal / greater (signed) by element width -> all-ones/zero mask
        case 0x74:
        case 0x75:
        case 0x76: // vpcmpeqb/w/d
        case 0x64:
        case 0x65:
        case 0x66: // vpcmpgtb/w/d (signed)
        {
            int es = (op == 0x74 || op == 0x64) ? 1 : (op == 0x75 || op == 0x65) ? 2 : 4;
            int gt = (op >= 0x64 && op <= 0x66);
            avx_get(c, vv, a);
            avx_get_rm(c, &I, next, W, b);
            for (int i = 0; i < W; i += es) {
                int64_t x = 0, y = 0;
                memcpy(&x, a + i, es);
                memcpy(&y, b + i, es);
                if (es < 8) { // sign-extend for gt
                    int sh = 64 - es * 8;
                    x = (x << sh) >> sh;
                    y = (y << sh) >> sh;
                }
                int t = gt ? (x > y) : (x == y);
                uint64_t m = t ? ~0ull : 0ull;
                memcpy(d + i, &m, es);
            }
            avx_put(c, rd, d, W);
            goto done;
        }
        case 0xD7: { // vpmovmskb: gpr <- sign bits of each byte of rm (per 128-bit lane, packed)
            avx_get_rm(c, &I, next, W, b);
            uint64_t m = 0;
            for (int i = 0; i < W; i++)
                if (b[i] & 0x80) m |= (1ull << i);
            c->r[rd] = m;
            goto done;
        }
        case 0x70: { // vpshufd(66)/vpshuflw(F2)/vpshufhw(F3) reg <- rm, imm8 (per 128-bit lane)
            avx_get_rm(c, &I, next, W, b);
            uint8_t imm = (uint8_t)I.imm;
            for (int lane = 0; lane < W; lane += 16) {
                if (pp == 1) { // vpshufd: 4 dwords
                    for (int j = 0; j < 4; j++) {
                        int sel = (imm >> (2 * j)) & 3;
                        memcpy(d + lane + 4 * j, b + lane + 4 * sel, 4);
                    }
                } else { // pshuflw(F2)/pshufhw(F3): shuffle low/high 4 words, copy the other half
                    memcpy(d + lane, b + lane, 16);
                    int base = (pp == 3) ? 8 : 0; // F3=high half, F2=low half
                    for (int j = 0; j < 4; j++) {
                        int sel = (imm >> (2 * j)) & 3;
                        memcpy(d + lane + base + 2 * j, b + lane + base + 2 * sel, 2);
                    }
                }
            }
            avx_put(c, rd, d, W);
            goto done;
        }
        case 0x77: { // vzeroupper (L=0): zero bits[128:256) of ymm0..15. vzeroall (L=1): zero all of 0..15.
            uint8_t z[64];
            memset(z, 0, 64);
            for (int r = 0; r < 16; r++) {
                if (L == 1) { // vzeroall
                    memset(&c->v[2 * r], 0, 16);
                }
                memset(&c->vhi[2 * r], 0, 16);
                memset(&c->vz[4 * r], 0, 32);
            }
            goto done;
        }
        }
    }
    // ---- map 2 (0F38) ----
    if (map == 2) {
        switch (op) {
        case 0x00: { // vpshufb: per-128-lane byte shuffle (src1=vvvv, control=rm)
            avx_get(c, vv, a);
            avx_get_rm(c, &I, next, W, b);
            for (int lane = 0; lane < W; lane += 16)
                for (int i = 0; i < 16; i++) {
                    uint8_t ctl = b[lane + i];
                    d[lane + i] = (ctl & 0x80) ? 0 : a[lane + (ctl & 0x0F)];
                }
            avx_put(c, rd, d, W);
            goto done;
        }
        case 0x78:
        case 0x79:
        case 0x58:
        case 0x59: { // vpbroadcastb/w/d/q: broadcast low element of rm across W
            int es = (op == 0x78) ? 1 : (op == 0x79) ? 2 : (op == 0x58) ? 4 : 8;
            avx_get_rm(c, &I, next, es, b);
            for (int i = 0; i < W; i += es) memcpy(d + i, b, es);
            avx_put(c, rd, d, W);
            goto done;
        }
        case 0x18:
        case 0x19: { // vbroadcastss(4)/sd(8)
            int es = (op == 0x18) ? 4 : 8;
            avx_get_rm(c, &I, next, es, b);
            for (int i = 0; i < W; i += es) memcpy(d + i, b, es);
            avx_put(c, rd, d, W);
            goto done;
        }
        }
    }
    // ---- map 3 (0F3A) ----
    if (map == 3) {
        switch (op) {
        case 0x38: { // vinserti128: dst = src1; dst[imm&1 *16] = rm(128)
            avx_get(c, vv, d);
            avx_get_rm(c, &I, next, 16, b);
            memcpy(d + ((I.imm & 1) ? 16 : 0), b, 16);
            avx_put(c, rd, d, 32);
            goto done;
        }
        case 0x39: { // vextracti128: rm(128) = src.reg[imm&1]
            avx_get(c, rd, a);
            memcpy(d, a + ((I.imm & 1) ? 16 : 0), 16);
            avx_put_rm(c, &I, next, 16, d);
            goto done;
        }
        case 0x0F: { // vpalignr imm8: per-128-lane byte concat(src1:src2) >> imm
            avx_get(c, vv, a);
            avx_get_rm(c, &I, next, W, b);
            int sh = (uint8_t)I.imm;
            for (int lane = 0; lane < W; lane += 16) {
                uint8_t t[32];
                memcpy(t, b + lane, 16);
                memcpy(t + 16, a + lane, 16);
                for (int i = 0; i < 16; i++) d[lane + i] = (sh + i < 32) ? t[sh + i] : 0;
            }
            avx_put(c, rd, d, W);
            goto done;
        }
        }
    }

    // ---- unimplemented: report precisely + exit 70 so coverage is grown test-driven ----
avx_unimpl:
    if (!g_avx_warned || getenv("CRASHDBG")) {
        g_avx_warned = 1;
        fprintf(stderr, "[avx] UNIMPLEMENTED %s map=%d op=0x%02x pp=%d L=%d w=%d rip=%llx\n",
                I.evex ? "EVEX" : "VEX", map, op, pp, L, I.vex_w, (unsigned long long)c->rip);
    }
    c->exited = 1;
    c->exit_code = 70;
    return;

done:
    c->rip = next;
}
