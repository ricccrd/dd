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
    if (I->seg == 1)
        a += c->fs_base;
    else if (I->seg == 2)
        a += c->gs_base;
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

// ---- scalar FP helpers used by the VEX arithmetic/FMA lowerings ----
// map-1 0x58..0x5F packed/scalar arithmetic, by opcode.
static float avx_fp_arith_f32(int op, float x, float y) {
    switch (op) {
    case 0x58: return x + y;
    case 0x59: return x * y;
    case 0x5C: return x - y;
    case 0x5E: return x / y;
    case 0x5D: return x < y ? x : y; // min (x86: NaN/equal -> second operand; tests don't probe that)
    default: return x > y ? x : y;   // 0x5F max
    }
}
static double avx_fp_arith_f64(int op, double x, double y) {
    switch (op) {
    case 0x58: return x + y;
    case 0x59: return x * y;
    case 0x5C: return x - y;
    case 0x5E: return x / y;
    case 0x5D: return x < y ? x : y;
    default: return x > y ? x : y;
    }
}

// F16C uses the host's native fp16 so the half<->single conversion (and round-to-nearest-even) matches x86.
static uint16_t avx_f32_to_f16(float f) {
    _Float16 h = (_Float16)f;
    uint16_t o;
    memcpy(&o, &h, 2);
    return o;
}
static float avx_f16_to_f32(uint16_t bits) {
    _Float16 h;
    memcpy(&h, &bits, 2);
    return (float)h;
}

static void do_avx(struct cpu *c) {
    struct insn I;
    decode(c->rip, &I);
    uint64_t next = c->rip + I.len;
    int L = I.vex_l;                            // 0=128,1=256,2=512
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
        case 0x7E: {       // F3: vmovq xmm<-xmm/mem (zext); 66: vmovd/q xmm->gpr/mem
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
        case 0x12: { // F2: vmovddup (dup low 64 per 128-lane); F3: vmovsldup (dup even dwords)
            if (pp == 3) {     // vmovddup
                uint8_t src[64]; // 128-bit reads m64; 256-bit reads m256
                if (I.is_mem) {
                    uint64_t ea = avx_ea(c, &I, next, L == 0 ? 8 : W);
                    memcpy(src, (void *)ea, L == 0 ? 8 : W);
                } else
                    avx_get(c, I.rm_reg, src);
                for (int lane = 0; lane < W; lane += 16) {
                    int so = (I.is_mem && L == 0) ? 0 : lane; // 128-bit mem source is a single m64
                    memcpy(d + lane, src + so, 8);
                    memcpy(d + lane + 8, src + so, 8);
                }
                avx_put(c, rd, d, W);
                goto done;
            } else if (pp == 2) { // vmovsldup
                avx_get_rm(c, &I, next, W, b);
                for (int i = 0; i < W; i += 8) {
                    memcpy(d + i, b + i, 4);
                    memcpy(d + i + 4, b + i, 4);
                }
                avx_put(c, rd, d, W);
                goto done;
            }
            goto avx_unimpl;
        }
        case 0x16: { // F3: vmovshdup (dup odd dwords)
            if (pp == 2) {
                avx_get_rm(c, &I, next, W, b);
                for (int i = 0; i < W; i += 8) {
                    memcpy(d + i, b + i + 4, 4);
                    memcpy(d + i + 4, b + i + 4, 4);
                }
                avx_put(c, rd, d, W);
                goto done;
            }
            goto avx_unimpl;
        }
        case 0x2A: { // vcvtsi2ss/sd: GPR/mem int -> scalar float; rest of low-128 from src1(vvvv)
            int dbl = (pp == 3), wi = I.vex_w ? 8 : 4;
            avx_get(c, vv, a);
            int64_t iv;
            if (I.is_mem) {
                uint64_t ea = avx_ea(c, &I, next, wi);
                iv = 0;
                memcpy(&iv, (void *)ea, wi);
                if (!I.vex_w) iv = (int32_t)iv;
            } else
                iv = I.vex_w ? (int64_t)c->r[I.rm_reg] : (int64_t)(int32_t)c->r[I.rm_reg];
            memcpy(d, a, 16);
            if (dbl) {
                double f = (double)iv;
                memcpy(d, &f, 8);
            } else {
                float f = (float)iv;
                memcpy(d, &f, 4);
            }
            avx_put(c, rd, d, 16);
            goto done;
        }
        case 0x2C: // vcvttss2si/sd2si (truncate) -> GPR
        case 0x2D: // vcvtss2si/sd2si (round)    -> GPR
        {
            int dbl = (pp == 3), es = dbl ? 8 : 4, trunc = (op == 0x2C);
            avx_get_rm(c, &I, next, es, b);
            int64_t res;
            if (dbl) {
                double x;
                memcpy(&x, b, 8);
                res = trunc ? (int64_t)x : (int64_t)__builtin_llrint(x);
            } else {
                float x;
                memcpy(&x, b, 4);
                res = trunc ? (int64_t)x : (int64_t)__builtin_llrintf(x);
            }
            c->r[rd] = I.vex_w ? (uint64_t)res : (uint32_t)res; // 32-bit dst zero-extends
            goto done;
        }
        case 0x5A: { // vcvtss2sd/sd2ss (scalar) or vcvtps2pd/pd2ps (packed) per pp
            if (pp == 2) { // F3: ss->sd scalar, rest of low-128 from src1
                avx_get(c, vv, a);
                avx_get_rm(c, &I, next, 4, b);
                memcpy(d, a, 16);
                float x;
                memcpy(&x, b, 4);
                double y = (double)x;
                memcpy(d, &y, 8);
                avx_put(c, rd, d, 16);
            } else if (pp == 3) { // F2: sd->ss scalar
                avx_get(c, vv, a);
                avx_get_rm(c, &I, next, 8, b);
                memcpy(d, a, 16);
                double x;
                memcpy(&x, b, 8);
                float y = (float)x;
                memcpy(d, &y, 4);
                avx_put(c, rd, d, 16);
            } else if (pp == 0) { // np: ps->pd, src is W/2 bytes of floats -> W bytes doubles
                avx_get_rm(c, &I, next, W / 2, b);
                int n = W / 8;
                for (int i = 0; i < n; i++) {
                    float x;
                    memcpy(&x, b + 4 * i, 4);
                    double y = (double)x;
                    memcpy(d + 8 * i, &y, 8);
                }
                avx_put(c, rd, d, W);
            } else { // 66: pd->ps, src W bytes doubles -> W/2 bytes floats
                avx_get_rm(c, &I, next, W, b);
                int n = W / 8;
                for (int i = 0; i < n; i++) {
                    double x;
                    memcpy(&x, b + 8 * i, 8);
                    float y = (float)x;
                    memcpy(d + 4 * i, &y, 4);
                }
                avx_put(c, rd, d, W / 2);
            }
            goto done;
        }
        // packed/scalar FP arithmetic: dst = src1 OP src2 (src1=vvvv, src2=rm). pp: 0=ps,1=pd,2=ss,3=sd.
        case 0x58: // vadd
        case 0x59: // vmul
        case 0x5C: // vsub
        case 0x5D: // vmin
        case 0x5E: // vdiv
        case 0x5F: // vmax
        {
            int dbl = (pp == 1 || pp == 3), scalar = (pp == 2 || pp == 3);
            int es = dbl ? 8 : 4;
            avx_get(c, vv, a);
            avx_get_rm(c, &I, next, scalar ? es : W, b);
            if (scalar) {       // low element computed, rest of low-128 from src1
                memcpy(d, a, 16);
                if (dbl) {
                    double x, y;
                    memcpy(&x, a, 8);
                    memcpy(&y, b, 8);
                    double z = avx_fp_arith_f64(op, x, y);
                    memcpy(d, &z, 8);
                } else {
                    float x, y;
                    memcpy(&x, a, 4);
                    memcpy(&y, b, 4);
                    float z = avx_fp_arith_f32(op, x, y);
                    memcpy(d, &z, 4);
                }
                avx_put(c, rd, d, 16);
            } else {
                for (int i = 0; i < W; i += es) {
                    if (dbl) {
                        double x, y;
                        memcpy(&x, a + i, 8);
                        memcpy(&y, b + i, 8);
                        double z = avx_fp_arith_f64(op, x, y);
                        memcpy(d + i, &z, 8);
                    } else {
                        float x, y;
                        memcpy(&x, a + i, 4);
                        memcpy(&y, b + i, 4);
                        float z = avx_fp_arith_f32(op, x, y);
                        memcpy(d + i, &z, 4);
                    }
                }
                avx_put(c, rd, d, W);
            }
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
                d[i] = (op == 0xEF || op == 0x57)   ? (x ^ y)
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
            int es = (op == 0xFC || op == 0xF8)   ? 1
                     : (op == 0xFD || op == 0xF9) ? 2
                     : (op == 0xFE || op == 0xFA) ? 4
                                                  : 8;
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
        case 0x71: // shift-by-imm8 group: dst=vvvv, src=rm(reg), ModRM.reg=opcode extension.
        case 0x72: // 0x71 word /2 psrlw /4 psraw /6 psllw; 0x72 dword /2 psrld /4 psrad /6 pslld;
        case 0x73: // 0x73 qword /2 psrlq /6 psllq; /3 psrldq /7 pslldq (per-128-lane byte shift).
        {
            int ext = rd, imm = (uint8_t)I.imm;
            int es = (op == 0x71) ? 2 : (op == 0x72) ? 4 : 8;
            avx_get(c, I.rm_reg, a); // source
            if (op == 0x73 && (ext == 3 || ext == 7)) {
                for (int lane = 0; lane < W; lane += 16)
                    for (int i = 0; i < 16; i++) {
                        if (ext == 3) // psrldq
                            d[lane + i] = (i + imm < 16) ? a[lane + i + imm] : 0;
                        else // pslldq
                            d[lane + i] = (i - imm >= 0) ? a[lane + i - imm] : 0;
                    }
            } else {
                int left = (ext == 6), arith = (ext == 4), bits = es * 8;
                for (int i = 0; i < W; i += es) {
                    uint64_t v = 0, z;
                    memcpy(&v, a + i, es);
                    if (left)
                        z = (imm >= bits) ? 0 : (v << imm);
                    else if (arith) {
                        int sh = 64 - bits;
                        int64_t sv = ((int64_t)v << sh) >> sh;
                        z = (uint64_t)(sv >> (imm >= bits ? bits - 1 : imm));
                    } else
                        z = (imm >= bits) ? 0 : (v >> imm);
                    memcpy(d + i, &z, es);
                }
            }
            avx_put(c, vv, d, W); // dst = VEX.vvvv
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
            for (int i = 0; i < W; i += es)
                memcpy(d + i, b, es);
            avx_put(c, rd, d, W);
            goto done;
        }
        case 0x18:
        case 0x19: { // vbroadcastss(4)/sd(8)
            int es = (op == 0x18) ? 4 : 8;
            avx_get_rm(c, &I, next, es, b);
            for (int i = 0; i < W; i += es)
                memcpy(d + i, b, es);
            avx_put(c, rd, d, W);
            goto done;
        }
        case 0x20: // vpmovsxbw   vpmov{s,z}x{b,w,d}{w,d,q}: widen a smaller source element with
        case 0x21: // vpmovsxbd   sign(2x)/zero(3x) extension. dst holds W/dst_es elements.
        case 0x22: // vpmovsxbq
        case 0x23: // vpmovsxwd
        case 0x24: // vpmovsxwq
        case 0x25: // vpmovsxdq
        case 0x30: // vpmovzxbw
        case 0x31: // vpmovzxbd
        case 0x32: // vpmovzxbq
        case 0x33: // vpmovzxwd
        case 0x34: // vpmovzxwq
        case 0x35: // vpmovzxdq
        {
            int sx = (op < 0x30), idx = op - (sx ? 0x20 : 0x30);
            static const int k_src_es[6] = {1, 1, 1, 2, 2, 4};
            static const int k_dst_es[6] = {2, 4, 8, 4, 8, 8};
            int src_es = k_src_es[idx], dst_es = k_dst_es[idx];
            int n = W / dst_es;
            avx_get_rm(c, &I, next, n * src_es, b);
            for (int i = 0; i < n; i++) {
                int64_t v = 0;
                memcpy(&v, b + i * src_es, src_es);
                if (sx) { // sign-extend from src_es bytes
                    int sh = 64 - src_es * 8;
                    v = (v << sh) >> sh;
                }
                memcpy(d + i * dst_es, &v, dst_es);
            }
            avx_put(c, rd, d, W);
            goto done;
        }
        case 0x13: { // vcvtph2ps: rm holds W/2 bytes of packed fp16 -> W/4 fp32 in dst
            int nf = W / 4;
            avx_get_rm(c, &I, next, W / 2, b);
            for (int i = 0; i < nf; i++) {
                uint16_t h;
                memcpy(&h, b + 2 * i, 2);
                float f = avx_f16_to_f32(h);
                memcpy(d + 4 * i, &f, 4);
            }
            avx_put(c, rd, d, W);
            goto done;
        }
        case 0x36: { // vpermd: dst.dword[i] = rm.dword[ vvvv.dword[i] & 7 ] (across full 256)
            avx_get(c, vv, a); // control indices
            avx_get_rm(c, &I, next, W, b);
            for (int i = 0; i < W; i += 4) {
                uint32_t idx;
                memcpy(&idx, a + i, 4);
                memcpy(d + i, b + 4 * (idx & 7), 4);
            }
            avx_put(c, rd, d, W);
            goto done;
        }
        case 0x40: { // vpmulld: 32-bit low product, dst = src1(vvvv) * rm
            avx_get(c, vv, a);
            avx_get_rm(c, &I, next, W, b);
            for (int i = 0; i < W; i += 4) {
                int32_t x, y;
                memcpy(&x, a + i, 4);
                memcpy(&y, b + i, 4);
                int32_t z = x * y;
                memcpy(d + i, &z, 4);
            }
            avx_put(c, rd, d, W);
            goto done;
        }
        case 0x45: // vpsrlvd/q: variable logical right shift
        case 0x46: // vpsravd:   variable arithmetic right shift (dword only)
        case 0x47: // vpsllvd/q: variable logical left shift
        {
            int es = I.vex_w ? 8 : 4; // W selects dword(0) / qword(1); 0x46 is dword-only
            avx_get(c, vv, a);        // values to shift
            avx_get_rm(c, &I, next, W, b);
            for (int i = 0; i < W; i += es) {
                uint64_t v = 0, cnt = 0;
                memcpy(&v, a + i, es);
                memcpy(&cnt, b + i, es);
                uint64_t z;
                int bits = es * 8;
                if (op == 0x46) { // arithmetic right (sign-extend), dword
                    int32_t sv;
                    memcpy(&sv, a + i, 4);
                    z = (uint32_t)((cnt >= 32) ? (sv >> 31) : (sv >> cnt));
                } else if (op == 0x45) { // logical right
                    z = (cnt >= (uint64_t)bits) ? 0 : (v >> cnt);
                } else { // 0x47 logical left
                    z = (cnt >= (uint64_t)bits) ? 0 : (v << cnt);
                }
                memcpy(d + i, &z, es);
            }
            avx_put(c, rd, d, W);
            goto done;
        }
        // ---- FMA (VEX 0F38 0x98..0xBF): fused multiply-add. reg=dst, vvvv, rm. W=0 ps, W=1 pd; odd op=scalar.
        case 0x98:
        case 0x99:
        case 0x9A:
        case 0x9B:
        case 0x9C:
        case 0x9D:
        case 0x9E:
        case 0x9F:
        case 0xA8:
        case 0xA9:
        case 0xAA:
        case 0xAB:
        case 0xAC:
        case 0xAD:
        case 0xAE:
        case 0xAF:
        case 0xB8:
        case 0xB9:
        case 0xBA:
        case 0xBB:
        case 0xBC:
        case 0xBD:
        case 0xBE:
        case 0xBF: {
            int form = (op >> 4) - 9;     // 0=132, 1=213, 2=231
            int base = op & 0x0E;         // 8=madd,A=msub,C=nmadd,E=nmsub
            int scalar = op & 1;
            int dbl = I.vex_w;
            int es = dbl ? 8 : 4;
            int sneg_mul = (base == 0x0C || base == 0x0E) ? -1 : 1;
            int sneg_add = (base == 0x0A || base == 0x0E) ? -1 : 1;
            uint8_t dst[64];
            avx_get(c, rd, dst);
            avx_get(c, vv, a);
            avx_get_rm(c, &I, next, W, b);
            // per form pick (mul1, mul2, add) from {dst, vvvv=a, rm=b}
            uint8_t *m1 = (form == 0) ? dst : a;
            uint8_t *m2 = (form == 0) ? b : (form == 1) ? dst : b;
            uint8_t *ad = (form == 0) ? a : (form == 1) ? b : dst;
            int n = scalar ? es : W;
            memcpy(d, dst, 64); // scalar keeps dst's upper bits; packed overwrites fully
            for (int i = 0; i < n; i += es) {
                if (dbl) {
                    double x, y, z;
                    memcpy(&x, m1 + i, 8);
                    memcpy(&y, m2 + i, 8);
                    memcpy(&z, ad + i, 8);
                    double res = __builtin_fma(sneg_mul * x, y, sneg_add * z);
                    memcpy(d + i, &res, 8);
                } else {
                    float x, y, z;
                    memcpy(&x, m1 + i, 4);
                    memcpy(&y, m2 + i, 4);
                    memcpy(&z, ad + i, 4);
                    float res = __builtin_fmaf(sneg_mul * x, y, sneg_add * z);
                    memcpy(d + i, &res, 4);
                }
            }
            avx_put(c, rd, d, scalar ? 16 : W);
            goto done;
        }
        }
    }
    // ---- map 3 (0F3A) ----
    if (map == 3) {
        switch (op) {
        case 0x18: // vinsertf128 (same as vinserti128)
        case 0x38: { // vinserti128: dst = src1; dst[imm&1 *16] = rm(128)
            avx_get(c, vv, d);
            avx_get_rm(c, &I, next, 16, b);
            memcpy(d + ((I.imm & 1) ? 16 : 0), b, 16);
            avx_put(c, rd, d, 32);
            goto done;
        }
        case 0x19: // vextractf128 (same as vextracti128)
        case 0x39: { // vextracti128: rm(128) = src.reg[imm&1]
            avx_get(c, rd, a);
            memcpy(d, a + ((I.imm & 1) ? 16 : 0), 16);
            avx_put_rm(c, &I, next, 16, d);
            goto done;
        }
        case 0x06: { // vperm2f128: select a 128-bit lane into each half (imm[3]/[7] zero the lane)
            avx_get(c, vv, a);
            avx_get_rm(c, &I, next, 32, b);
            uint8_t src[64];
            memcpy(src, a, 32);      // [0:16)=a.lo, [16:32)=a.hi
            memcpy(src + 32, b, 32); // [32:48)=b.lo, [48:64)=b.hi
            for (int half = 0; half < 2; half++) {
                int ctl = (I.imm >> (half * 4)) & 0xF;
                if (ctl & 0x8)
                    memset(d + half * 16, 0, 16);
                else
                    memcpy(d + half * 16, src + (ctl & 3) * 16, 16);
            }
            avx_put(c, rd, d, 32);
            goto done;
        }
        case 0x1D: { // vcvtps2ph: reg holds W/4 fp32 -> W/2 bytes of fp16 in rm (imm[1:0] rounding; RNE only)
            int nf = W / 4;
            avx_get(c, rd, a);
            for (int i = 0; i < nf; i++) {
                float f;
                memcpy(&f, a + 4 * i, 4);
                uint16_t h = avx_f32_to_f16(f);
                memcpy(d + 2 * i, &h, 2);
            }
            avx_put_rm(c, &I, next, W / 2, d);
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
                for (int i = 0; i < 16; i++)
                    d[lane + i] = (sh + i < 32) ? t[sh + i] : 0;
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
        fprintf(stderr, "[avx] UNIMPLEMENTED %s map=%d op=0x%02x pp=%d L=%d w=%d rip=%llx\n", I.evex ? "EVEX" : "VEX",
                map, op, pp, L, I.vex_w, (unsigned long long)c->rip);
    }
    c->exited = 1;
    c->exit_code = 70;
    return;

done:
    c->rip = next;
}

// ============================================================================================
// Legacy (non-VEX) 0F38 / 0F3A emulation (R_SSE3B): SSSE3, SSE4.1, SSE4.2, AES-NI, SHA, PCLMUL,
// CRC32 and MOVBE. Mirrors do_avx(): the translator exits the block at each such insn, this
// re-decodes it at cpu->rip, emulates against the xmm file (v[]) / GPRs (r[]) / memory and
// advances rip. Legacy SSE is destructive: ModRM.reg is both src1 and dst; ModRM.r/m is src2.
// ============================================================================================

static const uint8_t k_aes_sbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, 0xca, 0x82, 0xc9,
    0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f,
    0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07,
    0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3,
    0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58,
    0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3,
    0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f,
    0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
    0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac,
    0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a,
    0xae, 0x08, 0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70,
    0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1, 0xf8, 0x98, 0x11,
    0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, 0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42,
    0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};
static const uint8_t k_aes_isbox[256] = {
    0x52, 0x09, 0x6a, 0xd5, 0x30, 0x36, 0xa5, 0x38, 0xbf, 0x40, 0xa3, 0x9e, 0x81, 0xf3, 0xd7, 0xfb, 0x7c, 0xe3, 0x39,
    0x82, 0x9b, 0x2f, 0xff, 0x87, 0x34, 0x8e, 0x43, 0x44, 0xc4, 0xde, 0xe9, 0xcb, 0x54, 0x7b, 0x94, 0x32, 0xa6, 0xc2,
    0x23, 0x3d, 0xee, 0x4c, 0x95, 0x0b, 0x42, 0xfa, 0xc3, 0x4e, 0x08, 0x2e, 0xa1, 0x66, 0x28, 0xd9, 0x24, 0xb2, 0x76,
    0x5b, 0xa2, 0x49, 0x6d, 0x8b, 0xd1, 0x25, 0x72, 0xf8, 0xf6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xd4, 0xa4, 0x5c, 0xcc,
    0x5d, 0x65, 0xb6, 0x92, 0x6c, 0x70, 0x48, 0x50, 0xfd, 0xed, 0xb9, 0xda, 0x5e, 0x15, 0x46, 0x57, 0xa7, 0x8d, 0x9d,
    0x84, 0x90, 0xd8, 0xab, 0x00, 0x8c, 0xbc, 0xd3, 0x0a, 0xf7, 0xe4, 0x58, 0x05, 0xb8, 0xb3, 0x45, 0x06, 0xd0, 0x2c,
    0x1e, 0x8f, 0xca, 0x3f, 0x0f, 0x02, 0xc1, 0xaf, 0xbd, 0x03, 0x01, 0x13, 0x8a, 0x6b, 0x3a, 0x91, 0x11, 0x41, 0x4f,
    0x67, 0xdc, 0xea, 0x97, 0xf2, 0xcf, 0xce, 0xf0, 0xb4, 0xe6, 0x73, 0x96, 0xac, 0x74, 0x22, 0xe7, 0xad, 0x35, 0x85,
    0xe2, 0xf9, 0x37, 0xe8, 0x1c, 0x75, 0xdf, 0x6e, 0x47, 0xf1, 0x1a, 0x71, 0x1d, 0x29, 0xc5, 0x89, 0x6f, 0xb7, 0x62,
    0x0e, 0xaa, 0x18, 0xbe, 0x1b, 0xfc, 0x56, 0x3e, 0x4b, 0xc6, 0xd2, 0x79, 0x20, 0x9a, 0xdb, 0xc0, 0xfe, 0x78, 0xcd,
    0x5a, 0xf4, 0x1f, 0xdd, 0xa8, 0x33, 0x88, 0x07, 0xc7, 0x31, 0xb1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xec, 0x5f, 0x60,
    0x51, 0x7f, 0xa9, 0x19, 0xb5, 0x4a, 0x0d, 0x2d, 0xe5, 0x7a, 0x9f, 0x93, 0xc9, 0x9c, 0xef, 0xa0, 0xe0, 0x3b, 0x4d,
    0xae, 0x2a, 0xf5, 0xb0, 0xc8, 0xeb, 0xbb, 0x3c, 0x83, 0x53, 0x99, 0x61, 0x17, 0x2b, 0x04, 0x7e, 0xba, 0x77, 0xd6,
    0x26, 0xe1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0c, 0x7d};

static uint8_t aes_gfmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        uint8_t hi = a & 0x80;
        a <<= 1;
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}
static void aes_subbytes(uint8_t s[16], const uint8_t box[256]) {
    for (int i = 0; i < 16; i++) s[i] = box[s[i]];
}
// ShiftRows (inv=0) / InvShiftRows (inv=1). State is column-major: s[4*col+row].
static void aes_shiftrows(const uint8_t in[16], uint8_t out[16], int inv) {
    for (int col = 0; col < 4; col++)
        for (int row = 0; row < 4; row++) {
            int sc = inv ? ((col - row) & 3) : ((col + row) & 3);
            out[4 * col + row] = in[4 * sc + row];
        }
}
static void aes_mixcolumns(uint8_t s[16], int inv) {
    for (int col = 0; col < 4; col++) {
        uint8_t a0 = s[4 * col], a1 = s[4 * col + 1], a2 = s[4 * col + 2], a3 = s[4 * col + 3];
        if (!inv) {
            s[4 * col] = aes_gfmul(a0, 2) ^ aes_gfmul(a1, 3) ^ a2 ^ a3;
            s[4 * col + 1] = a0 ^ aes_gfmul(a1, 2) ^ aes_gfmul(a2, 3) ^ a3;
            s[4 * col + 2] = a0 ^ a1 ^ aes_gfmul(a2, 2) ^ aes_gfmul(a3, 3);
            s[4 * col + 3] = aes_gfmul(a0, 3) ^ a1 ^ a2 ^ aes_gfmul(a3, 2);
        } else {
            s[4 * col] = aes_gfmul(a0, 14) ^ aes_gfmul(a1, 11) ^ aes_gfmul(a2, 13) ^ aes_gfmul(a3, 9);
            s[4 * col + 1] = aes_gfmul(a0, 9) ^ aes_gfmul(a1, 14) ^ aes_gfmul(a2, 11) ^ aes_gfmul(a3, 13);
            s[4 * col + 2] = aes_gfmul(a0, 13) ^ aes_gfmul(a1, 9) ^ aes_gfmul(a2, 14) ^ aes_gfmul(a3, 11);
            s[4 * col + 3] = aes_gfmul(a0, 11) ^ aes_gfmul(a1, 13) ^ aes_gfmul(a2, 9) ^ aes_gfmul(a3, 14);
        }
    }
}

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static inline uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

// CRC-32C (Castagnoli, reflected poly 0x82F63B78) -- the polynomial used by the x86 CRC32 instruction.
static uint32_t crc32c_step(uint32_t crc, uint64_t v, int nbytes) {
    for (int b = 0; b < nbytes; b++) {
        crc ^= (uint8_t)(v >> (8 * b));
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0x82F63B78u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc;
}

static double sse_round_d(double x, int mode) {
    switch (mode & 3) {
    case 1: return __builtin_floor(x);
    case 2: return __builtin_ceil(x);
    case 3: return __builtin_trunc(x);
    default: return __builtin_rint(x); // round-to-nearest-even
    }
}
static float sse_round_f(float x, int mode) {
    switch (mode & 3) {
    case 1: return __builtin_floorf(x);
    case 2: return __builtin_ceilf(x);
    case 3: return __builtin_truncf(x);
    default: return __builtin_rintf(x);
    }
}

static inline int sat_s16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static inline int sat_u16(int v) { return v < 0 ? 0 : v > 65535 ? 65535 : v; }
static inline int sat_s8(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }

// Read the 16-byte r/m operand (xmm register or m128) of a legacy SSE insn.
static void sse_get_rm(struct cpu *c, struct insn *I, uint64_t next, uint8_t buf[16]) {
    if (I->is_mem)
        memcpy(buf, (void *)avx_ea(c, I, next, 16), 16);
    else
        memcpy(buf, &c->v[2 * I->rm_reg], 16);
}

static void do_sse3b(struct cpu *c) {
    struct insn I;
    decode(c->rip, &I);
    uint64_t next = c->rip + I.len;
    int map = I.map3, op = I.op;
    uint8_t *D = (uint8_t *)&c->v[2 * I.reg]; // dst xmm == src1 (destructive)
    uint8_t s[16], r[16];

    // ---- CRC32 (F2 0F38 F0/F1) and MOVBE (no-F2 0F38 F0/F1): GENERAL-register / memory ops -----------
    if (map == 2 && (op == 0xF0 || op == 0xF1)) {
        if (I.repne) { // CRC32 r, r/m  (F0=r/m8, F1=r/m16/32/64 per operand size)
            int nb = (op == 0xF0) ? 1 : I.opsize;
            uint64_t v;
            if (I.is_mem) {
                uint64_t a = avx_ea(c, &I, next, nb);
                v = 0;
                memcpy(&v, (void *)a, nb);
            } else {
                v = c->r[I.rm_reg];
            }
            uint32_t crc = (uint32_t)c->r[I.reg];
            crc = crc32c_step(crc, v, nb);
            c->r[I.reg] = crc; // zero-extends into the 64-bit GPR (incl. the REX.W form)
        } else {               // MOVBE: byte-swapping load (F0) / store (F1) of a memory operand
            int nb = I.opsize;
            uint64_t a = avx_ea(c, &I, next, nb);
            if (op == 0xF0) { // MOVBE r, m  -> reg = bswap(load)
                uint64_t v = 0;
                memcpy(&v, (void *)a, nb);
                uint64_t sw = 0;
                for (int i = 0; i < nb; i++)
                    sw |= ((v >> (8 * i)) & 0xff) << (8 * (nb - 1 - i));
                if (nb == 2)
                    c->r[I.reg] = (c->r[I.reg] & ~0xffffull) | (sw & 0xffff);
                else
                    c->r[I.reg] = sw; // 32-bit zero-extends, 64-bit full
            } else {              // MOVBE m, r  -> [m] = bswap(reg)
                uint64_t v = c->r[I.reg], sw = 0;
                for (int i = 0; i < nb; i++)
                    sw |= ((v >> (8 * i)) & 0xff) << (8 * (nb - 1 - i));
                memcpy((void *)a, &sw, nb);
            }
        }
        c->rip = next;
        return;
    }

    // ---- PEXTR* / EXTRACTPS (0F3A 14/15/16/17): xmm -> GPR/memory -----------------------------------
    if (map == 3 && (op == 0x14 || op == 0x15 || op == 0x16 || op == 0x17)) {
        int imm = (int)I.imm;
        uint64_t val;
        int nb;
        if (op == 0x14) {
            nb = 1;
            val = D[imm & 15];
        } else if (op == 0x15) {
            nb = 2;
            uint16_t w;
            memcpy(&w, D + 2 * (imm & 7), 2);
            val = w;
        } else if (op == 0x16) {
            nb = I.rexW ? 8 : 4;
            memcpy(&val, D + (I.rexW ? 8 * (imm & 1) : 4 * (imm & 3)), nb);
        } else { // 0x17 extractps -> 32-bit float lane as raw dword
            nb = 4;
            uint32_t w;
            memcpy(&w, D + 4 * (imm & 3), 4);
            val = w;
        }
        if (I.is_mem) {
            uint64_t a = avx_ea(c, &I, next, nb);
            memcpy((void *)a, &val, nb);
        } else if (nb == 8) {
            c->r[I.rm_reg] = val;
        } else {
            c->r[I.rm_reg] = (uint32_t)val; // pextrb/w/d/extractps zero-extend into the GPR
        }
        c->rip = next;
        return;
    }

    // ---- PINSR* / INSERTPS (0F3A 20/21/22): GPR/memory -> xmm ---------------------------------------
    if (map == 3 && (op == 0x20 || op == 0x21 || op == 0x22)) {
        int imm = (int)I.imm;
        if (op == 0x20) { // pinsrb: r/m8 -> byte lane imm[3:0]
            uint8_t v = I.is_mem ? *(uint8_t *)avx_ea(c, &I, next, 1) : (uint8_t)c->r[I.rm_reg];
            D[imm & 15] = v;
        } else if (op == 0x22) { // pinsrd/q: r/m32/64 -> dword/qword lane
            if (I.rexW) {
                uint64_t v = I.is_mem ? *(uint64_t *)avx_ea(c, &I, next, 8) : c->r[I.rm_reg];
                memcpy(D + 8 * (imm & 1), &v, 8);
            } else {
                uint32_t v = I.is_mem ? *(uint32_t *)avx_ea(c, &I, next, 4) : (uint32_t)c->r[I.rm_reg];
                memcpy(D + 4 * (imm & 3), &v, 4);
            }
        } else { // 0x21 insertps: select src dword, insert at dst lane, then zero per imm[3:0]
            uint32_t src;
            if (I.is_mem)
                src = *(uint32_t *)avx_ea(c, &I, next, 4); // memory source: element 0
            else
                memcpy(&src, (uint8_t *)&c->v[2 * I.rm_reg] + 4 * ((imm >> 6) & 3), 4); // src dword via imm[7:6]
            int dlane = (imm >> 4) & 3;
            memcpy(D + 4 * dlane, &src, 4);
            for (int i = 0; i < 4; i++)
                if (imm & (1 << i)) memset(D + 4 * i, 0, 4);
        }
        c->rip = next;
        return;
    }

    // ---- PCMPISTRI (0F3A 63): packed string compare, implicit-length -> index in ECX ---------------
    if (map == 3 && op == 0x63) {
        sse_get_rm(c, &I, next, s);
        int imm = (int)I.imm;
        int wordsz = (imm & 1) ? 2 : 1;       // element size: 0=byte,1=word
        int n = 16 / wordsz;                   // element count
        int agg = (imm >> 2) & 3;              // 0=equal-any,1=ranges,2=equal-each,3=equal-ordered
        int neg = (imm >> 4) & 1;              // polarity bit (negate)
        int neg_valid_only = (imm >> 5) & 1;   // masked negation
        int msb = (imm >> 6) & 1;              // index select: 0=lsb,1=msb
        int la = n, lb = n;                    // implicit lengths: first null element
        for (int i = 0; i < n; i++) {
            uint64_t e = 0;
            memcpy(&e, D + i * wordsz, wordsz);
            if (e == 0) { la = i; break; }
        }
        for (int i = 0; i < n; i++) {
            uint64_t e = 0;
            memcpy(&e, s + i * wordsz, wordsz);
            if (e == 0) { lb = i; break; }
        }
        int res = 0;
        for (int i = 0; i < n; i++) {
            int bit = 0, bvalid = (i < lb);
            uint64_t bi = 0;
            memcpy(&bi, s + i * wordsz, wordsz);
            if (agg == 0 || agg == 3) { // equal-any / equal-ordered: compare s[i] against a[]
                for (int j = 0; j < n; j++) {
                    int avalid = (j < la);
                    uint64_t aj = 0;
                    memcpy(&aj, D + j * wordsz, wordsz);
                    if (agg == 0) { // equal-any: any a[j]==s[i]
                        if (avalid && bvalid && aj == bi) { bit = 1; break; }
                    }
                }
            } else if (agg == 2) { // equal-each: a[i]==s[i] with validity override
                int avalid = (i < la);
                uint64_t ai = 0;
                memcpy(&ai, D + i * wordsz, wordsz);
                if (avalid && bvalid)
                    bit = (ai == bi);
                else if (!avalid && !bvalid)
                    bit = 1;
                else
                    bit = 0;
            }
            if (bit) res |= (1 << i);
        }
        if (neg) {
            int mask = (1 << n) - 1;
            if (neg_valid_only)
                res ^= ((1 << lb) - 1); // negate only valid-element positions
            else
                res ^= mask;
        }
        int idx;
        if (res == 0)
            idx = n;
        else if (msb)
            idx = 31 - __builtin_clz(res);
        else
            idx = __builtin_ctz(res);
        c->r[RCX] = idx;
        // PCMPISTRI flags (Intel SDM): CF=(IntRes2!=0), ZF=(operand2/rm reached its null, lb<n),
        // SF=(operand1/reg reached its null, la<n), OF=IntRes2[0], AF=PF=0. glibc's SSE4.2 strlen/
        // strchr/strstr branch on these (jbe/ja/jc/jz) right after the op, so they MUST be set; leaving
        // stale flags was the debian-glibc grep miscount. Substrate: x86 CF = NOT stored-C (borrow).
        {
            int cf = (res != 0), zf = (lb < n), sf = (la < n), of = (res & 1);
            c->nzcv = ((uint64_t)sf << 31) | ((uint64_t)zf << 30) | ((uint64_t)(!cf) << 29) | ((uint64_t)of << 28);
            c->pf = 1; // PF source byte: odd popcount -> x86 PF=0
        }
        c->rip = next;
        return;
    }

    // ---- the remaining ops are xmm-destructive: load the r/m source, compute into r, write to D -----
    sse_get_rm(c, &I, next, s);
    memcpy(r, D, 16);

    if (map == 2) {
        switch (op) {
        case 0x00: { // pshufb
            uint8_t t[16];
            memcpy(t, D, 16);
            for (int i = 0; i < 16; i++)
                r[i] = (s[i] & 0x80) ? 0 : t[s[i] & 0x0f];
            break;
        }
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x05:
        case 0x06:
        case 0x07: { // phadd/phsub w/d (saturated for 03/07)
            int sub = (op >= 0x05);
            int sat = (op == 0x03 || op == 0x07);
            if (op == 0x02 || op == 0x06) { // dword
                int32_t a[4], b[4], o[4];
                memcpy(a, D, 16);
                memcpy(b, s, 16);
                o[0] = sub ? a[0] - a[1] : a[0] + a[1];
                o[1] = sub ? a[2] - a[3] : a[2] + a[3];
                o[2] = sub ? b[0] - b[1] : b[0] + b[1];
                o[3] = sub ? b[2] - b[3] : b[2] + b[3];
                memcpy(r, o, 16);
            } else { // word
                int16_t a[8], b[8], o[8];
                memcpy(a, D, 16);
                memcpy(b, s, 16);
                for (int i = 0; i < 4; i++) {
                    int va = sub ? a[2 * i] - a[2 * i + 1] : a[2 * i] + a[2 * i + 1];
                    int vb = sub ? b[2 * i] - b[2 * i + 1] : b[2 * i] + b[2 * i + 1];
                    o[i] = sat ? (int16_t)sat_s16(va) : (int16_t)va;
                    o[i + 4] = sat ? (int16_t)sat_s16(vb) : (int16_t)vb;
                }
                memcpy(r, o, 16);
            }
            break;
        }
        case 0x08:
        case 0x09:
        case 0x0A: { // psign b/w/d
            if (op == 0x08) {
                int8_t a[16], b[16], o[16];
                memcpy(a, D, 16);
                memcpy(b, s, 16);
                for (int i = 0; i < 16; i++) o[i] = b[i] < 0 ? -a[i] : b[i] == 0 ? 0 : a[i];
                memcpy(r, o, 16);
            } else if (op == 0x09) {
                int16_t a[8], b[8], o[8];
                memcpy(a, D, 16);
                memcpy(b, s, 16);
                for (int i = 0; i < 8; i++) o[i] = b[i] < 0 ? -a[i] : b[i] == 0 ? 0 : a[i];
                memcpy(r, o, 16);
            } else {
                int32_t a[4], b[4], o[4];
                memcpy(a, D, 16);
                memcpy(b, s, 16);
                for (int i = 0; i < 4; i++) o[i] = b[i] < 0 ? -a[i] : b[i] == 0 ? 0 : a[i];
                memcpy(r, o, 16);
            }
            break;
        }
        case 0x0B: { // pmulhrsw
            int16_t a[8], b[8], o[8];
            memcpy(a, D, 16);
            memcpy(b, s, 16);
            for (int i = 0; i < 8; i++) o[i] = (int16_t)((((a[i] * b[i]) >> 14) + 1) >> 1);
            memcpy(r, o, 16);
            break;
        }
        case 0x1C:
        case 0x1D:
        case 0x1E: { // pabs b/w/d (single source: r/m)
            if (op == 0x1C) {
                int8_t a[16];
                memcpy(a, s, 16);
                for (int i = 0; i < 16; i++) r[i] = (uint8_t)(a[i] < 0 ? -a[i] : a[i]);
            } else if (op == 0x1D) {
                int16_t a[8], o[8];
                memcpy(a, s, 16);
                for (int i = 0; i < 8; i++) o[i] = a[i] < 0 ? -a[i] : a[i];
                memcpy(r, o, 16);
            } else {
                int32_t a[4], o[4];
                memcpy(a, s, 16);
                for (int i = 0; i < 4; i++) o[i] = a[i] < 0 ? -a[i] : a[i];
                memcpy(r, o, 16);
            }
            break;
        }
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x23:
        case 0x24:
        case 0x25: { // pmovsx (sign-extend)
            int8_t b8[16];
            int16_t w16[8];
            int32_t d32[4];
            memcpy(b8, s, 16);
            memcpy(w16, s, 16);
            memcpy(d32, s, 16);
            if (op == 0x20) {
                int16_t o[8];
                for (int i = 0; i < 8; i++) o[i] = b8[i];
                memcpy(r, o, 16);
            } else if (op == 0x21) {
                int32_t o[4];
                for (int i = 0; i < 4; i++) o[i] = b8[i];
                memcpy(r, o, 16);
            } else if (op == 0x22) {
                int64_t o[2];
                for (int i = 0; i < 2; i++) o[i] = b8[i];
                memcpy(r, o, 16);
            } else if (op == 0x23) {
                int32_t o[4];
                for (int i = 0; i < 4; i++) o[i] = w16[i];
                memcpy(r, o, 16);
            } else if (op == 0x24) {
                int64_t o[2];
                for (int i = 0; i < 2; i++) o[i] = w16[i];
                memcpy(r, o, 16);
            } else {
                int64_t o[2];
                for (int i = 0; i < 2; i++) o[i] = d32[i];
                memcpy(r, o, 16);
            }
            break;
        }
        case 0x30:
        case 0x31:
        case 0x32:
        case 0x33:
        case 0x34:
        case 0x35: { // pmovzx (zero-extend)
            uint8_t b8[16];
            uint16_t w16[8];
            uint32_t d32[4];
            memcpy(b8, s, 16);
            memcpy(w16, s, 16);
            memcpy(d32, s, 16);
            if (op == 0x30) {
                uint16_t o[8];
                for (int i = 0; i < 8; i++) o[i] = b8[i];
                memcpy(r, o, 16);
            } else if (op == 0x31) {
                uint32_t o[4];
                for (int i = 0; i < 4; i++) o[i] = b8[i];
                memcpy(r, o, 16);
            } else if (op == 0x32) {
                uint64_t o[2];
                for (int i = 0; i < 2; i++) o[i] = b8[i];
                memcpy(r, o, 16);
            } else if (op == 0x33) {
                uint32_t o[4];
                for (int i = 0; i < 4; i++) o[i] = w16[i];
                memcpy(r, o, 16);
            } else if (op == 0x34) {
                uint64_t o[2];
                for (int i = 0; i < 2; i++) o[i] = w16[i];
                memcpy(r, o, 16);
            } else {
                uint64_t o[2];
                for (int i = 0; i < 2; i++) o[i] = d32[i];
                memcpy(r, o, 16);
            }
            break;
        }
        case 0x28: { // pmuldq: signed (a.dword[0]*s.dword[0], a.dword[2]*s.dword[2]) -> 2 qwords
            int32_t a[4], b[4];
            int64_t o[2];
            memcpy(a, D, 16);
            memcpy(b, s, 16);
            o[0] = (int64_t)a[0] * (int64_t)b[0];
            o[1] = (int64_t)a[2] * (int64_t)b[2];
            memcpy(r, o, 16);
            break;
        }
        case 0x29: { // pcmpeqq
            uint64_t a[2], b[2], o[2];
            memcpy(a, D, 16);
            memcpy(b, s, 16);
            o[0] = (a[0] == b[0]) ? ~0ull : 0;
            o[1] = (a[1] == b[1]) ? ~0ull : 0;
            memcpy(r, o, 16);
            break;
        }
        case 0x2B: { // packusdw: pack signed dword -> unsigned word (saturate); dst low, src high
            int32_t a[4], b[4];
            uint16_t o[8];
            memcpy(a, D, 16);
            memcpy(b, s, 16);
            for (int i = 0; i < 4; i++) o[i] = (uint16_t)sat_u16(a[i]);
            for (int i = 0; i < 4; i++) o[i + 4] = (uint16_t)sat_u16(b[i]);
            memcpy(r, o, 16);
            break;
        }
        case 0x37: { // pcmpgtq (signed)
            int64_t a[2], b[2];
            uint64_t o[2];
            memcpy(a, D, 16);
            memcpy(b, s, 16);
            o[0] = (a[0] > b[0]) ? ~0ull : 0;
            o[1] = (a[1] > b[1]) ? ~0ull : 0;
            memcpy(r, o, 16);
            break;
        }
        case 0x38:
        case 0x39:
        case 0x3A:
        case 0x3B:
        case 0x3C:
        case 0x3D:
        case 0x3E:
        case 0x3F: { // pmin/pmax sb/sd/uw/ud/sb.../
            if (op == 0x38 || op == 0x3C) { // signed byte min/max
                int8_t a[16], b[16], o[16];
                memcpy(a, D, 16);
                memcpy(b, s, 16);
                for (int i = 0; i < 16; i++) o[i] = (op == 0x38) ? (a[i] < b[i] ? a[i] : b[i]) : (a[i] > b[i] ? a[i] : b[i]);
                memcpy(r, o, 16);
            } else if (op == 0x3A || op == 0x3E) { // unsigned word min/max
                uint16_t a[8], b[8], o[8];
                memcpy(a, D, 16);
                memcpy(b, s, 16);
                for (int i = 0; i < 8; i++) o[i] = (op == 0x3A) ? (a[i] < b[i] ? a[i] : b[i]) : (a[i] > b[i] ? a[i] : b[i]);
                memcpy(r, o, 16);
            } else if (op == 0x39 || op == 0x3D) { // signed dword min/max
                int32_t a[4], b[4], o[4];
                memcpy(a, D, 16);
                memcpy(b, s, 16);
                for (int i = 0; i < 4; i++) o[i] = (op == 0x39) ? (a[i] < b[i] ? a[i] : b[i]) : (a[i] > b[i] ? a[i] : b[i]);
                memcpy(r, o, 16);
            } else { // 0x3B/0x3F unsigned dword min/max
                uint32_t a[4], b[4], o[4];
                memcpy(a, D, 16);
                memcpy(b, s, 16);
                for (int i = 0; i < 4; i++) o[i] = (op == 0x3B) ? (a[i] < b[i] ? a[i] : b[i]) : (a[i] > b[i] ? a[i] : b[i]);
                memcpy(r, o, 16);
            }
            break;
        }
        case 0x40: { // pmulld: 32-bit low product
            int32_t a[4], b[4], o[4];
            memcpy(a, D, 16);
            memcpy(b, s, 16);
            for (int i = 0; i < 4; i++) o[i] = a[i] * b[i];
            memcpy(r, o, 16);
            break;
        }
        case 0x10:
        case 0x14:
        case 0x15: { // pblendvb / blendvps / blendvpd -- mask is implicit xmm0
            uint8_t *mask = (uint8_t *)&c->v[0];
            if (op == 0x10) { // pblendvb: per-byte, mask = top bit of each byte
                for (int i = 0; i < 16; i++) r[i] = (mask[i] & 0x80) ? s[i] : D[i];
            } else if (op == 0x14) { // blendvps: per dword, top bit
                for (int i = 0; i < 4; i++)
                    memcpy(r + 4 * i, (mask[4 * i + 3] & 0x80) ? s + 4 * i : D + 4 * i, 4);
            } else { // blendvpd: per qword
                for (int i = 0; i < 2; i++)
                    memcpy(r + 8 * i, (mask[8 * i + 7] & 0x80) ? s + 8 * i : D + 8 * i, 8);
            }
            break;
        }
        case 0xDB: // aesimc: dst = InvMixColumns(src)
            memcpy(r, s, 16);
            aes_mixcolumns(r, 1);
            break;
        case 0xDC: // aesenc
        case 0xDD: // aesenclast
        {
            uint8_t t[16];
            aes_shiftrows(D, t, 0);
            aes_subbytes(t, k_aes_sbox);
            if (op == 0xDC) aes_mixcolumns(t, 0);
            for (int i = 0; i < 16; i++) r[i] = t[i] ^ s[i];
            break;
        }
        case 0xDE: // aesdec
        case 0xDF: // aesdeclast
        {
            uint8_t t[16];
            aes_shiftrows(D, t, 1);
            aes_subbytes(t, k_aes_isbox);
            if (op == 0xDE) aes_mixcolumns(t, 1);
            for (int i = 0; i < 16; i++) r[i] = t[i] ^ s[i];
            break;
        }
        case 0xCB: { // sha256rnds2: dst,src, implicit xmm0 = WK0/WK1
            uint32_t st1[4], st2[4], wk[4];
            memcpy(st1, D, 16);  // C0=st1[3],D0=st1[2],G0=st1[1],H0=st1[0]
            memcpy(st2, s, 16);  // A0=st2[3],B0=st2[2],E0=st2[1],F0=st2[0]
            memcpy(wk, &c->v[0], 16);
            uint32_t A = st2[3], B = st2[2], Cc = st1[3], Dd = st1[2];
            uint32_t E = st2[1], F = st2[0], G = st1[1], H = st1[0];
            for (int i = 0; i < 2; i++) {
                uint32_t WK = wk[i];
                uint32_t s1 = rotr32(E, 6) ^ rotr32(E, 11) ^ rotr32(E, 25);
                uint32_t ch = (E & F) ^ (~E & G);
                uint32_t s0 = rotr32(A, 2) ^ rotr32(A, 13) ^ rotr32(A, 22);
                uint32_t maj = (A & B) ^ (A & Cc) ^ (B & Cc);
                uint32_t t1 = H + s1 + ch + WK;
                uint32_t An = t1 + s0 + maj;
                uint32_t En = t1 + Dd;
                H = G; G = F; F = E; E = En;
                Dd = Cc; Cc = B; B = A; A = An;
            }
            uint32_t o[4] = {F, E, B, A}; // DEST: [31:0]=F2,[63:32]=E2,[95:64]=B2,[127:96]=A2
            memcpy(r, o, 16);
            break;
        }
        case 0xCC: { // sha256msg1
            uint32_t w[4], w4;
            memcpy(w, D, 16);    // W0=w[0]..W3=w[3]
            memcpy(&w4, s, 4);   // W4 = src[31:0]
            uint32_t in[5] = {w[0], w[1], w[2], w[3], w4};
            uint32_t o[4];
            for (int i = 0; i < 4; i++) {
                uint32_t x = in[i + 1];
                uint32_t s0 = rotr32(x, 7) ^ rotr32(x, 18) ^ (x >> 3);
                o[i] = in[i] + s0;
            }
            memcpy(r, o, 16);
            break;
        }
        default:
            goto unimpl;
        }
        memcpy(D, r, 16);
        c->rip = next;
        return;
    }

    // ---- map == 3 (0F3A), the xmm-destructive imm8 forms ------------------------------------------
    {
        int imm = (int)I.imm;
        switch (op) {
        case 0x08:
        case 0x09:
        case 0x0A:
        case 0x0B: { // roundps/pd/ss/sd, mode in imm[3:0] (bit2 set = use MXCSR, we treat as nearest)
            int mode = (imm & 4) ? 0 : (imm & 3);
            if (op == 0x08) { // roundps
                float a[4], o[4];
                memcpy(a, s, 16);
                for (int i = 0; i < 4; i++) o[i] = sse_round_f(a[i], mode);
                memcpy(r, o, 16);
            } else if (op == 0x09) { // roundpd
                double a[2], o[2];
                memcpy(a, s, 16);
                for (int i = 0; i < 2; i++) o[i] = sse_round_d(a[i], mode);
                memcpy(r, o, 16);
            } else if (op == 0x0A) { // roundss: low lane from src, rest from dst
                float a;
                memcpy(&a, s, 4);
                a = sse_round_f(a, mode);
                memcpy(r, &a, 4);
            } else { // roundsd
                double a;
                memcpy(&a, s, 8);
                a = sse_round_d(a, mode);
                memcpy(r, &a, 8);
            }
            break;
        }
        case 0x0C: { // blendps (4 dwords)
            for (int i = 0; i < 4; i++)
                if (imm & (1 << i)) memcpy(r + 4 * i, s + 4 * i, 4);
            break;
        }
        case 0x0D: { // blendpd (2 qwords)
            for (int i = 0; i < 2; i++)
                if (imm & (1 << i)) memcpy(r + 8 * i, s + 8 * i, 8);
            break;
        }
        case 0x0E: { // pblendw (8 words)
            for (int i = 0; i < 8; i++)
                if (imm & (1 << i)) memcpy(r + 2 * i, s + 2 * i, 2);
            break;
        }
        case 0x0F: { // palignr: (dst:src) >> imm8 bytes
            uint8_t comb[32];
            memcpy(comb, s, 16);
            memcpy(comb + 16, D, 16);
            for (int i = 0; i < 16; i++) r[i] = (imm + i < 32) ? comb[imm + i] : 0;
            break;
        }
        case 0x40: { // dpps: packed-single dot product
            float a[4], b[4];
            memcpy(a, D, 16);
            memcpy(b, s, 16);
            float sum = 0;
            for (int i = 0; i < 4; i++)
                if (imm & (0x10 << i)) sum += a[i] * b[i];
            float o[4];
            for (int i = 0; i < 4; i++) o[i] = (imm & (1 << i)) ? sum : 0.0f;
            memcpy(r, o, 16);
            break;
        }
        case 0x41: { // dppd: packed-double dot product
            double a[2], b[2];
            memcpy(a, D, 16);
            memcpy(b, s, 16);
            double sum = 0;
            for (int i = 0; i < 2; i++)
                if (imm & (0x10 << i)) sum += a[i] * b[i];
            double o[2];
            for (int i = 0; i < 2; i++) o[i] = (imm & (1 << i)) ? sum : 0.0;
            memcpy(r, o, 16);
            break;
        }
        case 0x44: { // pclmulqdq: carryless multiply of selected 64-bit halves
            uint64_t a64, b64;
            memcpy(&a64, D + 8 * (imm & 1), 8);
            memcpy(&b64, s + 8 * ((imm >> 4) & 1), 8);
            unsigned __int128 prod = 0;
            for (int i = 0; i < 64; i++)
                if ((b64 >> i) & 1) prod ^= (unsigned __int128)a64 << i;
            memcpy(r, &prod, 16);
            break;
        }
        case 0xCC: { // sha1rnds4: 4 SHA-1 rounds, function/constant from imm[1:0]
            uint32_t f_sel = imm & 3;
            uint32_t K = (f_sel == 0)   ? 0x5A827999u
                         : (f_sel == 1) ? 0x6ED9EBA1u
                         : (f_sel == 2) ? 0x8F1BBCDCu
                                        : 0xCA62C1D6u;
            uint32_t st[4], w[4];
            memcpy(st, D, 16); // D0=st[0],C0=st[1],B0=st[2],A0=st[3]
            memcpy(w, s, 16);  // W3=w[0],W2=w[1],W1=w[2],W0=w[3]
            uint32_t A = st[3], B = st[2], Cc = st[1], Dd = st[0];
            uint32_t W[4] = {w[3], w[2], w[1], w[0]};
            uint32_t E = 0;
            for (int i = 0; i < 4; i++) {
                uint32_t f = (f_sel == 0)   ? ((B & Cc) | (~B & Dd))
                             : (f_sel == 2) ? ((B & Cc) | (B & Dd) | (Cc & Dd))
                                            : (B ^ Cc ^ Dd);
                uint32_t t = f + rotl32(A, 5) + W[i] + K + E;
                E = Dd;
                Dd = Cc;
                Cc = rotl32(B, 30);
                B = A;
                A = t;
            }
            uint32_t o[4] = {Dd, Cc, B, A}; // DEST: [31:0]=D4,[63:32]=C4,[95:64]=B4,[127:96]=A4
            memcpy(r, o, 16);
            break;
        }
        case 0xDF: { // aeskeygenassist: SubWord+RotWord+RCON on dwords 1 and 3
            uint32_t x[4];
            memcpy(x, s, 16);
            uint32_t rcon = (uint32_t)(imm & 0xff);
            uint32_t X1 = x[1], X3 = x[3];
            uint32_t sub1 = (uint32_t)k_aes_sbox[X1 & 0xff] | ((uint32_t)k_aes_sbox[(X1 >> 8) & 0xff] << 8) |
                            ((uint32_t)k_aes_sbox[(X1 >> 16) & 0xff] << 16) | ((uint32_t)k_aes_sbox[(X1 >> 24) & 0xff] << 24);
            uint32_t sub3 = (uint32_t)k_aes_sbox[X3 & 0xff] | ((uint32_t)k_aes_sbox[(X3 >> 8) & 0xff] << 8) |
                            ((uint32_t)k_aes_sbox[(X3 >> 16) & 0xff] << 16) | ((uint32_t)k_aes_sbox[(X3 >> 24) & 0xff] << 24);
            uint32_t o[4];
            o[0] = sub1;
            o[1] = rotr32(sub1, 8) ^ rcon;
            o[2] = sub3;
            o[3] = rotr32(sub3, 8) ^ rcon;
            memcpy(r, o, 16);
            break;
        }
        default:
            goto unimpl;
        }
        memcpy(D, r, 16);
        c->rip = next;
        return;
    }

unimpl:
    if (!g_avx_warned || getenv("CRASHDBG")) {
        g_avx_warned = 1;
        fprintf(stderr, "[sse3b] UNIMPLEMENTED map=%d op=0x%02x p66=%d rep=%d repne=%d rip=%llx\n", map, op, I.p66,
                I.rep, I.repne, (unsigned long long)c->rip);
    }
    c->exited = 1;
    c->exit_code = 70;
}
