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
