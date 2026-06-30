// ---------------- opt5: rep movs/stos idiom upgrade ----------------
// Generalizes the LSE idiom-upgrade lever to the x86 string ops: a `rep movs`/`rep stos`
// (the idiomatic memcpy/memset of every musl/glibc x86 guest) is lowered to ONE optimized
// host libc call instead of the per-element `ldr;str;sub;cbnz` host loop. Bit-exact with
// that scalar loop for all lengths (incl. 0), alignments, and the forward-overlap smear.
// Kill-switch: NOREP=1 (or any non-"0" value) -> fall back to the original element loop.
static int norep_disabled(void) {
    static int v = -1; // env read once, then cached
    if (v < 0) {
        const char *s = getenv("NOREP");
        v = (s && *s && *s != '0') ? 1 : 0;
    }
    return v;
}

// Host helper for `rep movs`: copy `nbytes` forward, x86 element-by-element semantics.
// rep movs always copies LOW->HIGH; a plain memcpy/memmove is correct only when the
// regions are disjoint or dst precedes src. When src < dst < src+nbytes the forward copy
// SMEARS (each element is re-read after a previous element overwrote it) -- memmove would
// be WRONG here -- so we replay it element-by-element at the guest element width `w`,
// which reproduces the scalar loop's bytes exactly (byte-wise smear differs from a
// w>1 element smear at sub-element overlap offsets).
// W6A item 1 (non-PIE): a biased ET_EXEC's guest pointer may still carry its low link address (e.g. a
// rip-relative lea into the type/rodata section); rebase it to the real high mapping so these bulk C string
// helpers touch the mapped bytes (the single-access fault path nonpie_fixup cannot serve a libc memcpy).
// Inert for PIE/static-PIE (g_nonpie_lo == 0).
static inline uint64_t repstr_g2h(uint64_t a) {
    return (g_nonpie_lo && a >= g_nonpie_lo && a < g_nonpie_hi) ? a + g_nonpie_bias : a;
}
static void dd_rep_movs(uint8_t *dst, const uint8_t *src, uint64_t nbytes, int w) {
    dst = (uint8_t *)repstr_g2h((uint64_t)dst);
    src = (const uint8_t *)repstr_g2h((uint64_t)src);
    if (nbytes == 0) return;
    if (dst <= src || dst >= src + nbytes) { // disjoint, or forward-safe (dst before src)
        memcpy(dst, src, nbytes);
        return;
    }
    switch (w) { // forward-overlap smear, element-granular (matches per-element rep movs)
    case 2: {
        uint16_t *d = (uint16_t *)dst;
        const uint16_t *s = (const uint16_t *)src;
        for (uint64_t i = 0, n = nbytes >> 1; i < n; i++) d[i] = s[i];
        return;
    }
    case 4: {
        uint32_t *d = (uint32_t *)dst;
        const uint32_t *s = (const uint32_t *)src;
        for (uint64_t i = 0, n = nbytes >> 2; i < n; i++) d[i] = s[i];
        return;
    }
    case 8: {
        uint64_t *d = (uint64_t *)dst;
        const uint64_t *s = (const uint64_t *)src;
        for (uint64_t i = 0, n = nbytes >> 3; i < n; i++) d[i] = s[i];
        return;
    }
    default:
        for (uint64_t i = 0; i < nbytes; i++) dst[i] = src[i];
        return;
    }
}

// Host helper for `rep stos`: fill `n` elements of width `w` with `val` (AL/AX/EAX/RAX).
static void dd_rep_stos(uint8_t *dst, uint64_t val, uint64_t n, int w) {
    dst = (uint8_t *)repstr_g2h((uint64_t)dst);
    switch (w) {
    case 2: {
        uint16_t *p = (uint16_t *)dst, v = (uint16_t)val;
        for (uint64_t i = 0; i < n; i++) p[i] = v;
        return;
    }
    case 4: {
        uint32_t *p = (uint32_t *)dst, v = (uint32_t)val;
        for (uint64_t i = 0; i < n; i++) p[i] = v;
        return;
    }
    case 8: {
        uint64_t *p = (uint64_t *)dst, v = val;
        for (uint64_t i = 0; i < n; i++) p[i] = v;
        return;
    }
    default:
        memset(dst, (int)(val & 0xff), n);
        return;
    }
}

// Reload guest state (flags + xmm0..15 + r0..15) from the membank into host regs, WITHOUT
// re-pinning x28 (== &cpu, callee-saved across the host call). Mirrors emit_prologue minus
// the `mov x28,x0`.
static void emit_reload(void) {
    e_nzcv_load();
    for (int t = 0; t < 16; t += 2)
        e_ldp_q(t, t + 1, 28, OFF_V + t * 16);
    for (int r = 1; r <= 15; r++)
        e_ldr(r, 28, R_OFF(r));
    e_ldr(0, 28, 0); // rax last
}

// Codegen for the idiom: spill guest state, marshal args, blr the host helper, then fix up
// RDI/RSI (+= count*w) and RCX (->0) in the membank snapshot, and reload. Guest GPRs live in
// host x0..x15 (caller-saved) so the spill/reload around the call is mandatory; x28 (cpu) is
// callee-saved and survives; the host SP is untouched (guest RSP is x4), so ABI alignment holds.
static void emit_rep_string(int movs, int w, int shift) {
    emit_spill();                          // x0..x15 + xmm0..15 + flags -> cpu (membank)
    e_ldr(0, 28, R_OFF(RDI));              // x0 = dst (rdi)
    e_ldr(1, 28, R_OFF(movs ? RSI : RAX)); // x1 = src (rsi) / fill value (rax)
    e_ldr(2, 28, R_OFF(RCX));              // x2 = element count (rcx)
    e_movconst(3, (uint64_t)w);            // x3 = element width
    if (movs) {
        if (shift) e_lsl_i(2, 2, shift, 1); // x2 = nbytes = count << shift
        emit_host_ptr(16, (uint64_t)(uintptr_t)&dd_rep_movs, PRELOC_HOSTGLOBAL);
    } else {
        emit_host_ptr(16, (uint64_t)(uintptr_t)&dd_rep_stos, PRELOC_HOSTGLOBAL);
    }
    emit32(0xD63F0000u | (16 << 5)); // blr x16
    // membank still holds the pre-call rcx/rdi/rsi (the helper takes its args by value):
    e_ldr(17, 28, R_OFF(RCX)); // x17 = original element count
    if (shift)
        e_lsl_i(16, 17, shift, 1); // x16 = nbytes = count << shift
    else
        e_mov_rr(16, 17, 1);
    e_ldr(19, 28, R_OFF(RDI));
    e_rrr(A_ADD, 19, 19, 16, 1, 0); // rdi += nbytes
    e_str(19, 28, R_OFF(RDI));
    if (movs) {
        e_ldr(19, 28, R_OFF(RSI));
        e_rrr(A_ADD, 19, 19, 16, 1, 0); // rsi += nbytes
        e_str(19, 28, R_OFF(RSI));
    }
    e_str(31, 28, R_OFF(RCX)); // rcx = 0 (str xzr); EFLAGS unchanged by movs/stos
    emit_reload();
}

// translate_block control code: a per-class translate helper cannot continue/break the caller's for(;;)
// translate loop, so it returns how the caller should steer it. (Shared by the instruction-class splits.)
enum { TX_FALL = 0, TX_NEXT = 1, TX_BREAK = 2 };

// ---- string ops dispatch: stos/movs/lods (AA/AB/A4/A5/AC/AD), cmps/scas (A6/A7/AE/AF), cld/std (FC/FD).
// Lifted VERBATIM out of translate_block's one-byte switch (behavior-preserving move). DF assumed 0 (fwd)
// for stos/movs/lods unless std set g_df. Returns TX_FALL if `op` is not a string op (caller falls through
// to the next handler), TX_NEXT (caller: `gpc = next; continue;`), or TX_BREAK (block ends; caller: break).
static int translate_string(struct insn *I, uint64_t next) {
    uint8_t op = I->op;
    if (op == 0xAA || op == 0xAB || op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD) {
        int w = (op & 1) ? I->opsize : 1;
        int movs = (op == 0xA4 || op == 0xA5), lods = (op == 0xAC || op == 0xAD);
        // opt5: `rep movs`/`rep stos` -> one optimized host memcpy/memset call (bit-exact with the scalar
        // loop below). Fall back to that loop for NOREP=1, `lods` (result is RAX, not a bulk move), a segment
        // override / 32-bit address size (the scalar loop ignores both too), or DF=1 (host helper is forward-
        // only; the backward case takes the per-element scalar loop with a decrementing stride).
        if (I->rep && !lods && !I->seg && !I->addr32 && !g_df && (w == 1 || w == 2 || w == 4 || w == 8) &&
            !norep_disabled()) {
            int shift = w == 1 ? 0 : w == 2 ? 1 : w == 4 ? 2 : 3;
            emit_rep_string(movs, w, shift);
            return TX_NEXT;
        }
        uint32_t *cbz = NULL, *top = NULL;
        if (I->rep) {
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
        if (I->rep) {
            e_subi(RCX, RCX, 1, 1);
            int64_t back = (int64_t)(top - (uint32_t *)g_cp);
            emit32(0x14000000u | ((uint32_t)back & 0x3FFFFFFu)); // b top
            int64_t d = ((uint32_t *)g_cp - cbz);
            *cbz = 0xB4000000u | (((uint32_t)d & 0x7FFFF) << 5) | RCX; // cbz x_rcx,done
        }
        return TX_NEXT;
    }
    // cmps (A6/A7) / scas (AE/AF): the whole (possibly REP/REPE/REPNE) compare+scan is done in ONE C round-
    // trip (like cpuid/div): bit-exact RCX/RSI/RDI + ZF/SF/CF/OF end-state, fast host memcmp/memchr inside on
    // the forward path (gate NOREPCMP for the naive per-element oracle loop; DF=1 uses that loop with a
    // decrementing stride). Descriptor (width | isscas | isrepne | isrep | df) -> cpu->divop.
    if (op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF) {
        int w = (op & 1) ? I->opsize : 1;
        int isscas = (op == 0xAE || op == 0xAF);
        int isrep = (I->rep || I->repne);
        uint64_t desc = (uint64_t)w | ((uint64_t)isscas << 8) | ((uint64_t)(I->repne ? 1 : 0) << 9) |
                        ((uint64_t)isrep << 10) | ((uint64_t)(g_df ? 1 : 0) << 11);
        e_movconst(16, desc);
        e_str(16, 28, OFF_DIVOP);
        emit_exit_const(next, R_REPSTR); // spills regs+flags; do_repstr() resumes at `next`
        return TX_BREAK;                 // block ends here (helper runs, dispatcher continues)
    }
    if (op == 0xFC) {
        g_df = 0; // cld: forward string ops
        return TX_NEXT;
    }
    if (op == 0xFD) {
        g_df = 1; // std: backward string ops (consumed at translate time by the lowering below)
        return TX_NEXT;
    }
    return TX_FALL;
}
