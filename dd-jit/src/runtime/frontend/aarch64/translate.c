// dd/runtime/frontend/aarch64 -- the aarch64-Linux -> arm64-host transliterator. Same-ISA: copy
// most instructions verbatim; MANGLE only stolen-register (x18/x28/x30) users. Optimizations: LSE
// atomic upgrade, §B shadow-return prediction (depth-gated), tier-2 purity gate. See OPTIMIZATIONS.md.

// Non-PIE ET_EXEC link span + high-map bias. Really defined (and set by load_elf) in os/linux/container/
// vfs.c and os/linux/elf.c, both compiled LATER in the same unity TU; forward-declared here (static, so
// it merges into the single later definition) so adr/adrp can un-bias the PC. 0 for PIE/static-PIE.
static uint64_t g_nonpie_lo, g_nonpie_hi, g_nonpie_bias;

// PC-relative base for adr/adrp materialization. A non-PIE ET_EXEC maps HIGH (the low 4GB is reserved),
// so the dispatcher biases the guest PC to the high mapping before translate_block -> gpc here is HIGH.
// But the image's baked absolute data pointers are LOW (non-PIE => no dynamic relocations), and Go/gcc
// compare an adr/adrp-computed pointer against such a stored pointer for identity; a HIGH result then
// mismatches the LOW baked pointer (gcc ICEs in set_static_spec; cc1 hits an invalid free()). Materialize
// adr/adrp against the LOW (un-biased) PC so the produced value matches the baked pointers; the
// nonpie_fixup SIGSEGV handler transparently serves the resulting LOW data access from the real high
// mapping (+bias). Branch/stitch/dispatch logic keeps the HIGH gpc -- only the *address value* adr/adrp
// produces becomes LOW. Inert for PIE/static-PIE (g_nonpie_lo == 0, the only state the test matrix sees).
static uint64_t pcrel_base(uint64_t gpc) {
    if (g_nonpie_lo && gpc >= g_nonpie_lo + g_nonpie_bias && gpc < g_nonpie_hi + g_nonpie_bias)
        return gpc - g_nonpie_bias;
    return gpc;
}

// ---- x18 stealing ----
// macOS asynchronously zeroes the real x18 (it is reserved on Apple platforms), but a
// Linux guest uses x18 as a normal GP register. So guest x18 must NEVER live in the real
// x18: it lives in cpu->x[18], and any guest instruction that names x18 is rewritten to
// use a scratch loaded from / stored back to cpu->x[18].
//
// gpr_field_mask: which of the 4 register fields are GP registers for this instruction.
//   bit0 = [4:0] (Rd/Rt)   bit1 = [9:5] (Rn)   bit2 = [20:16] (Rm/Rs)   bit3 = [14:10] (Rt2/Ra)
static int gpr_field_mask(uint32_t in) {
    uint32_t op = (in >> 25) & 0xF;
    // Data-processing immediate
    if (op == 8 || op == 9) {
        //   adr/adrp: Rd
        if ((in & 0x1F000000u) == 0x10000000u) return 1;
        //   move wide: Rd (imm in [20:5])
        if ((in & 0x1F800000u) == 0x12800000u) return 1;
        //   extr: Rd,Rn,Rm
        if ((in & 0x1F800000u) == 0x13800000u) return 1 | 2 | 4;
        //   add/sub-imm, logical-imm, bitfield: Rd,Rn
        return 1 | 2;
    }
    // Branches/Exception/System
    if (op == 0xA || op == 0xB) {
        //   mrs/msr <-> Rt
        if ((in & 0xFFD00000u) == 0xD5100000u) return 1;
        //   branches: handled as block enders
        return 0;
    }
    // Loads and Stores
    if ((in & 0x0A000000u) == 0x08000000u) {
        //   Rn[9:5] base is GP
        int v = (in >> 26) & 1, m = 2;
        //   Rt[4:0] GP unless SIMD/FP
        if (!v) m |= 1;
        //   register offset: Rm[20:16]
        if ((in & 0x3B200C00u) == 0x38200800u) m |= 4;
        //   load/store pair: Rt2[14:10] (GP only)
        if ((in & 0x3A000000u) == 0x28000000u && !v) m |= 8;
        //   exclusive: Rs[20:16], Rt2[14:10]
        if ((in & 0x3F000000u) == 0x08000000u) m |= 4 | 8;
        return m;
    }
    // Data-processing register
    if ((in & 0x0E000000u) == 0x0A000000u) {
        //   3-source: Rd,Rn,Rm,Ra
        if ((in & 0x1F000000u) == 0x1B000000u) return 1 | 2 | 4 | 8;
        //   1-source: Rd,Rn (Rm field is opcode)
        if ((in & 0x5FE00000u) == 0x5AC00000u) return 1 | 2;
        if ((in & 0x1FE00000u) == 0x1A400000u)
            // ccmp/ccmn: [4:0]=nzcv; imm -> Rn only
            return (in & 0x800u) ? 2 : (2 | 4);
        //   logical/addsub-reg/cond-sel/2-source
        return 1 | 2 | 4;
    }
    // SIMD/FP data: V registers only
    return 0;
}
static int field_is(uint32_t in, int bit, int shift) { return is_stolen((in >> shift) & 0x1F) && bit; }
// "uses a STOLEN reg" (x18 / x28 [/ x30 in Stage B])
static int uses_x18(uint32_t in, int mask) {
    return field_is(in, mask & 1, 0) || field_is(in, mask & 2, 5) || field_is(in, mask & 4, 16) ||
           field_is(in, mask & 8, 10);
}
// Emit a guest insn that references stolen reg(s): for each, a scratch S = cpu->x[stolen]; run the
// insn with the stolen field(s) replaced by scratch(es); store back. Real x28 = cpu is the base;
// scratch originals are spilled to cpu->mscratch (NOT the stack -- that would collide with the
// guest's own stp/ldp frame stores + writeback). At most two distinct stolen regs in one insn.
static void emit_mangled_x18(uint32_t in, int mask) {
    static const int shifts[4] = {0, 5, 16, 10}, mbits[4] = {1, 2, 4, 8};
    int stolen[2], ns = 0, used = 0;
    for (int k = 0; k < 4; k++)
        if (mask & mbits[k]) {
            int rf = (in >> shifts[k]) & 0x1F;
            used |= 1 << rf;
            if (is_stolen(rf)) {
                int seen = 0;
                for (int j = 0; j < ns; j++)
                    if (stolen[j] == rf) seen = 1;
                if (!seen) stolen[ns++] = rf;
            }
        }
    int sc[2], nsc = 0;
    for (int r = 0; r <= 27 && nsc < ns; r++)
        if (!(used & (1 << r)) && !is_stolen(r)) sc[nsc++] = r;
    for (int i = 0; i < ns; i++)
        // spill scratch -> cpu->mscratch
        e_str(sc[i], CPUREG, (int)OFF_MSCRATCH + 8 * i);
    for (int i = 0; i < ns; i++)
        // scratch = cpu->x[stolen]
        e_ldr(sc[i], CPUREG, stolen[i] * 8);
    uint32_t m = in;
    for (int k = 0; k < 4; k++)
        if (mask & mbits[k]) {
            int rf = (m >> shifts[k]) & 0x1F;
            if (is_stolen(rf)) {
                int s = sc[0];
                for (int i = 0; i < ns; i++)
                    if (stolen[i] == rf) s = sc[i];
                m = (m & ~(0x1Fu << shifts[k])) | ((unsigned)s << shifts[k]);
            }
        }
    emit32(m);
    for (int i = 0; i < ns; i++)
        // cpu->x[stolen] = scratch
        e_str(sc[i], CPUREG, stolen[i] * 8);
    for (int i = 0; i < ns; i++)
        // restore scratch
        e_ldr(sc[i], CPUREG, (int)OFF_MSCRATCH + 8 * i);
}
// For instructions that WRITE x18 via a special path (adr/ldr-literal/mrs): save x0,x1 to
// the red zone, x1 := cpu. The case then computes a value into x0 and stores it to
// cpu->x[18]; x18_epilog restores x0,x1.
static void x18_prolog(void) {
    e_stur(0, 31, -16);
    e_stur(1, 31, -24);
    e_load_cpu(1);
}
static void x18_epilog(void) {
    e_ldur(1, 31, -24);
    e_ldur(0, 31, -16);
}
// A3 §B instrumentation (PROF=1 only): bump a 64-bit global counter from emitted code. Self-contained:
// spills x9/x10 to the [sp,#-16/-24] red zone (same convention as emit_t2_counter / the IBTC), so it is
// independent of whatever scratch the surrounding shadow push/ret is juggling. Gated on g_prof, so the
// non-PROF codegen is byte-identical to baseline (zero steady-state cost).
static void emit_prof_bump(void *ctr) {
    e_stur(9, 31, -16);
    e_stur(10, 31, -24);
    e_adrp_add(9, (uint64_t)ctr); // x9 = &counter (plain RW data; adrp+add reaches it)
    e_ldr(10, 9, 0);
    e_addi(10, 10, 1);
    e_str(10, 9, 0);
    e_ldur(9, 31, -16);
    e_ldur(10, 31, -24);
}
// §B: store a constant to cpu->x[30] (the stolen guest link reg). x28=cpu; x0 scratched via mscratch.
static void emit_set_x30(uint64_t val) {
    e_str(0, CPUREG, (int)OFF_MSCRATCH);
    e_movconst(0, val);
    e_str(0, CPUREG, 30 * 8);
    e_ldr(0, CPUREG, (int)OFF_MSCRATCH);
}
// §B shadow push: cpu->x[30] = gpc+4; sstk[ssp&1023] = (gpc+4, &Lcont); ssp++. x0..x2 spilled to
// cpu->mscratch (all guest regs are live across the call). Returns the `adr x1,Lcont` to backpatch.
static uint32_t *emit_shadow_push(uint64_t gpc) {
    int M = (int)OFF_MSCRATCH;
    if (g_prof) emit_prof_bump(&g_prof_shpush); // A3: count §B shadow pushes executed (PROF only)
    e_stp(0, 1, CPUREG, M);
    // spill x0..x3 -> mscratch (paired: 2 stp not 4 str)
    e_stp(2, 3, CPUREG, M + 16);
    e_movconst(0, gpc + 4);
    // x0 = guest_ret; cpu->x[30] = guest_ret (ALWAYS)
    e_str(0, CPUREG, 30 * 8);
    // x1 = ssp (capped at 1024)
    e_ldr(1, CPUREG, (int)OFF_SSP);
    uint32_t *p_full = (uint32_t *)g_cp;
    // tbnz x1, #10, Lskip (ssp==1024 -> overflow; no flags)
    emit32(0);
    e_addlsl4(2, CPUREG, 1);
    // x2 = C + idx*16 + OFF_SSTK = &sstk[2*ssp]
    e_addi(2, 2, (unsigned)OFF_SSTK);
    uint32_t *p_adr = (uint32_t *)g_cp;
    // adr x3, Lcont (host_ret; backpatched)
    emit32(0);
    // sstk[2*ssp] = (guest_ret, host_ret=&Lcont)
    e_stp(0, 3, 2, 0);
    e_mov_from_sp(3);
    e_addlsl3(2, CPUREG, 1);
    // gsp[ssp] = current guest SP (frame disambiguator)
    e_str(3, 2, (int)OFF_GSP);
    e_addi(1, 1, 1);
    // ssp++
    e_str(1, CPUREG, (int)OFF_SSP);
    uint8_t *Lskip = g_cp;
    *p_full = 0x37000000u | (10u << 19) | (((uint32_t)(((uint8_t *)Lskip - (uint8_t *)p_full) / 4) & 0x3FFF) << 5) |
              // tbnz x1,#10
              1;
    e_ldp(0, 1, CPUREG, M);
    // restore x0..x3 (paired: 2 ldp not 4 ldr)
    e_ldp(2, 3, CPUREG, M + 16);
    return p_adr;
}
// §B profile gate: scan the target's entry block. A LEAF function (reaches `ret` with no bl/blr
// first) gains nothing from the shadow-RAS -- its monomorphic return is predicted by the per-site IC
// -- so paying the per-bl shadow push is pure overhead (floatk: sqrt/sin/pow). Only non-leaf targets
// (depth -> the hardware RAS predicts nested returns: stringk/recursion) get §B. Static, no profiling
// overhead; the ret auto-adapts (no frame pushed -> classify falls to the IC return).
// A3 §B depth-gate tuning. The baseline gate (scan_calls + target_is_leaf) is depth-2 static and
// MISCLASSIFIES two important cases as "leaf" (-> withholds §B -> the return falls to the IBTC):
//   (1) a function LARGER than the 64-insn scan window whose calls live past it (fib at -O2, most of
//       sqlite's VDBE helpers) -- the scan exhausts having seen no bl and reports "leaf";
//   (2) a "shallow" helper that calls only leaves but is itself called from MANY sites -- its single
//       return site is polymorphic, so the per-site IC thrashes (exactly what the RAS fixes).
// §B is self-validating (emit_shadow_ret checks guest_ret AND guest_sp; a wrong guess -> IBTC, never a
// misland), so the gate only ever trades cycles, never correctness. MEASUREMENT (see arm-a3.md) shows
// §B is NET-NEGATIVE on every return-heavy workload tested -- sqlite, qsort, AND the ideal polymorphic
// deep-recursion cases (longfib 2x, deepcall 1.4x) -- because the shadow push (~19 insn + sstk stores)
// plus the shadow-ret validate (~22 insn: guest_ret AND guest_sp compares) cost FAR more than the IBTC
// return path they replace (a monomorphic per-site IC hit, or even a thrashing shared-hash probe). The
// host RAS's 1-cycle `ret` is buried under ~40 insn of software bookkeeping. So the right tune is the
// OPPOSITE of "widen": DISABLE §B and return every ret through the proven IBTC. Levels (env, once):
//   -1 (DEFAULT)      -> §B OFF: no shadow push; every ret -> bare IBTC (IC + shared hash). The win.
//   -2 SHADOWGATE=-2  -> §B OFF on the push side, but ret keeps the shadow-ret stub (empty -> IBTC).
//    0 NOSHADOWTUNE=1 -> EXACT original §B-on gate (byte-identical baseline codegen). A/B kill switch.
//    1 SHADOWGATE=1   -> widen-fix: window-exhaustion = large/complex fn -> DEEP not leaf (measured: worse).
//    2 SHADOWGATE=2   -> widen more: ANY direct call -> §B (measured: worse / no better).
static int g_shadowgate = -2;
static int shadowgate(void) {
    if (g_shadowgate == -2) {
        if (getenv("NOSHADOWTUNE")) g_shadowgate = 0;
        else { const char *e = getenv("SHADOWGATE"); g_shadowgate = e ? atoi(e) : -1; }
    }
    return g_shadowgate;
}
// Scan target's straight-line extent (bounded by forward-branch reach). Returns -1 if a blr (unknown
// callee) -- or, when tuned, if the scan window is exhausted with no clean terminal (large/complex fn,
// treat as deep) -- else the count of direct-call (bl) targets, writing up to `max` of them to calls[].
static int scan_calls(uint64_t target, uint64_t calls[], int max) {
    int64_t reach = 0;
    int n = 0;
    for (int i = 0; i < 64; i++) {
        uint32_t in = *(uint32_t *)(target + (uint64_t)i * 4);
        // blr -> unknown callee
        if ((in & 0xFFFFFC1Fu) == 0xD63F0000u) return -1;
        if ((in & 0xFC000000u) == 0x94000000u) {
            if (n < max) calls[n] = target + (uint64_t)i * 4 + ((uint64_t)sext(in & 0x3FFFFFF, 26) << 2);
            n++;
        // bl
        }
        int64_t off = 0;
        int isb = 0;
        if ((in & 0xFF000010u) == 0x54000000u) {
            off = sext((in >> 5) & 0x7FFFF, 19);
            isb = 1;
        // b.cond
        }
        else if ((in & 0x7E000000u) == 0x34000000u) {
            off = sext((in >> 5) & 0x7FFFF, 19);
            isb = 1;
        // cbz/cbnz
        }
        else if ((in & 0x7E000000u) == 0x36000000u) {
            off = sext((in >> 5) & 0x3FFF, 14);
            isb = 1;
        // tbz/tbnz
        }
        else if ((in & 0xFC000000u) == 0x14000000u) {
            off = sext(in & 0x3FFFFFF, 26);
            isb = 1;
        // b
        }
        if (isb && off > 0 && i + off < 64 && i + off > reach) reach = i + off;
        if ((in & 0xFFFFFC1Fu) == 0xD65F0000u || (in & 0xFC000000u) == 0x14000000u || (in & 0xFFFFFC1Fu) == 0xD61F0000u)
            // terminal past all branches
            if (i >= reach) return n;
    }
    // window exhausted with no clean terminal: a function larger than the scan window. Baseline reported
    // whatever bl count it happened to see (usually 0 -> "leaf"); tuned treats it as deep/unknown.
    return shadowgate() ? -1 : n;
}
static int is_leaf0(uint64_t t) {
    uint64_t c[1];
    return scan_calls(t, c, 0) == 0;
// no calls at all
}
// §B helps only on DEPTH (the RAS predicts nested returns). A leaf or a depth-2 "shallow" function
// (all its calls go to leaves: sqrt/sin/pow's helpers) gains nothing -> keep cheap Stage-B. Only a
// function that calls a NON-leaf (or recurses, or calls indirectly) is deep enough to pay the push.
static int target_is_leaf(uint64_t target) {
    uint64_t calls[16];
    int n = scan_calls(target, calls, 16);
    // SHADOWGATE=-1: never §B (every bl -> leaf path, every ret -> IBTC). Floor experiment.
    if (shadowgate() < 0) return 1;
    // indirect callee / large-complex fn (tuned) -> assume deep -> §B
    if (n < 0) return 0;
    // true leaf (no calls at all: sqrt/sin/pow) -> Stage-B, regardless of level
    if (n == 0) return 1;
    // L2/L3: ANY direct call -> §B (covers multiply-called shallow helpers whose ret site is polymorphic)
    if (shadowgate() >= 2) return 0;
    for (int i = 0; i < n && i < 16; i++)
        // calls a non-leaf -> deep -> §B
        if (!is_leaf0(calls[i])) return 0;
    // all-leaf-calls (shallow) -> Stage-B
    return 1;
}
// §B guest bl: push shadow, host `bl body(target)` (RAS pushes &Lcont), Lcont continues at gpc+4.
static void emit_bl_ras(uint64_t gpc, uint64_t target) {
    if (target_is_leaf(target)) {
        if (g_prof) g_prof_bl_leaf++; // A3: depth-gate steered this bl to the cheap leaf Stage-B path
        emit_set_x30(gpc + 4);
        emit_chain_exit(target);
        return;
    // leaf -> cheap Stage-B (IC return)
    }
    if (g_prof) g_prof_bl_shadow++; // A3: depth-gate steered this bl to §B (shadow push + RAS ret)
    uint32_t *p_adr = emit_shadow_push(gpc);
    void *body = map_body(target);
    uint32_t *slot = (uint32_t *)g_cp;
    if (body) {
        int64_t d = ((uint8_t *)body - (uint8_t *)slot) / 4;
        emit32(0x94000000u | ((uint32_t)d & 0x3FFFFFFu));
    // host bl body (RAS pushes &Lcont)
    }
    else {
        add_pend2(slot, target, 1);
        emit_exit_const(target, R_BRANCH);
    // not translated yet: spill-exit (slot patched to `bl body`)
    }
    // host ret lands here
    uint8_t *Lcont = g_cp;
    int64_t ao = Lcont - (uint8_t *)p_adr;
    // adr x3, Lcont
    *p_adr = 0x10000000u | ((uint32_t)(ao & 3) << 29) | (((uint32_t)((ao >> 2) & 0x7FFFF)) << 5) | 3;
    // after the call returns -> gpc+4
    emit_chain_exit(gpc + 4);
}
// §B guest ret: if cpu->x[30] == shadow-top guest_ret, pop + real x30=host_ret + host `ret`
// (hardware-RAS predicted). Else fall back to the dispatcher reading cpu->x[30]. Never lands wrong.
static void emit_shadow_ret(void) {
    if (g_ibprof) { // ARM-B1 IBPROF: log the return as an indirect transfer (target = cpu->x[30])
        emit_iblog(30);
        return;
    }
    int M = (int)OFF_MSCRATCH;
    e_stp(0, 1, CPUREG, M);
    // spill x0..x3 (paired)
    e_stp(2, 3, CPUREG, M + 16);
    // x0 = ssp
    e_ldr(0, CPUREG, (int)OFF_SSP);
    uint32_t *p_cbz = (uint32_t *)g_cp;
    // cbz x0, Lfb (empty shadow)
    emit32(0);
    // x0 = ssp-1 = idx (ssp<=1024 -> no wrap)
    e_subi(0, 0, 1);
    e_addlsl4(1, CPUREG, 0);
    // x1 = &sstk[2*idx]
    e_addi(1, 1, (unsigned)OFF_SSTK);
    // x2 = guest_ret, x3 = host_ret
    e_ldp(2, 3, 1, 0);
    // x1 = cpu->x[30] (guest return target)
    e_ldr(1, CPUREG, 30 * 8);
    // sub x1, x2, x1 (guest_ret - x30; no flags)
    emit32(0xCB000000u | (1 << 16) | (2 << 5) | 1);
    uint32_t *p_cb1 = (uint32_t *)g_cp;
    // cbnz x1, Lfb (foreign/longjmp)
    emit32(0);
    e_addlsl3(1, CPUREG, 0);
    // x2 = gsp[idx] (guest SP captured at the bl)
    e_ldr(2, 1, (int)OFF_GSP);
    // x1 = current guest SP
    e_mov_from_sp(1);
    // sub x1, x1, x2 (sp - gsp; no flags)
    emit32(0xCB000000u | (2 << 16) | (1 << 5) | 1);
    uint32_t *p_cb2 = (uint32_t *)g_cp;
    // cbnz x1, Lfb (guest_ret matched but wrong frame -> slow)
    emit32(0);
    // FAST: ssp-- (pop)
    e_str(0, CPUREG, (int)OFF_SSP);
    // real x30 = host_ret
    e_movr(30, 3);
    e_ldp(0, 1, CPUREG, M);
    // restore x0..x3 (paired)
    e_ldp(2, 3, CPUREG, M + 16);
    if (g_prof) emit_prof_bump(&g_prof_shret_hit); // A3: §B predicted-return FAST hit (PROF only)
    // host ret -> &Lcont (hardware-RAS predicted)
    e_hret();
    uint8_t *Lfb = g_cp;
    *p_cbz = 0xB4000000u | (((uint32_t)(((uint8_t *)Lfb - (uint8_t *)p_cbz) / 4) & 0x7FFFF) << 5) | 0;
    // cbnz x1
    *p_cb1 = 0xB5000000u | (((uint32_t)(((uint8_t *)Lfb - (uint8_t *)p_cb1) / 4) & 0x7FFFF) << 5) | 1;
    // cbnz x1
    *p_cb2 = 0xB5000000u | (((uint32_t)(((uint8_t *)Lfb - (uint8_t *)p_cb2) / 4) & 0x7FFFF) << 5) | 1;
    e_ldp(0, 1, CPUREG, M);
    // restore x0..x3 (paired)
    e_ldp(2, 3, CPUREG, M + 16);
    if (g_prof) emit_prof_bump(&g_prof_shret_fb); // A3: §B return fell to the IBTC fallback (PROF only)
    // UNWIND/FOREIGN -> IBTC (per-site IC + hash), NOT the dispatcher
    emit_ibranch(30);
}
// Fast correct ret on the stolen x30: per-site monomorphic cache on cpu->x[30] (a `br`, not a host
// ret -> no RAS, but no stale-host_ret corruption either). Mirrors the IBTC per-site IC; the
// dispatcher fills Lsite_tgt/Lsite_body via ic_site. Reads cpu->x[30] (x30 is stolen).
static void emit_ret_ic(void) {
    e_stur(16, 31, -16);
    // stash scratch (target body_ind restores it)
    e_stur(17, 31, -24);
    // x16 = cpu->x[30]
    e_ldr(16, CPUREG, 30 * 8);
    uint32_t *p_ldrt = (uint32_t *)g_cp;
    // ldr x17, Lsite_tgt
    emit32(0);
    // sub x17, x17, x16
    emit32(0xCB000000u | (16 << 16) | (17 << 5) | 17);
    uint32_t *p_cbnz = (uint32_t *)g_cp;
    // cbnz x17, Lmiss
    emit32(0);
    uint32_t *p_ldrb = (uint32_t *)g_cp;
    // ldr x16, Lsite_body
    emit32(0);
    // HIT -> body_ind
    e_br(16);
    uint32_t *miss = (uint32_t *)g_cp;
    e_ldur(16, 31, -16);
    e_ldur(17, 31, -24);
    emit_spill();
    e_ldr(9, 0, 30 * 8);
    // cpu->pc = cpu->x[30]
    e_str(9, 0, OFF_PC);
    e_movconst(9, R_BRANCH);
    e_str(9, 0, OFF_RSN);
    uint32_t *p_adr = (uint32_t *)g_cp;
    // adr x9, Lsite_tgt -> dispatcher fills the site
    emit32(0);
    e_str(9, 0, OFF_ICSITE);
    e_movconst(9, (uint64_t)block_return);
    e_br(9);
    if ((uint64_t)g_cp & 7) emit32(0);
    uint8_t *Lt = g_cp;
    *(uint64_t *)g_cp = 0;
    g_cp += 8;
    uint8_t *Lb = g_cp;
    *(uint64_t *)g_cp = 0;
    g_cp += 8;
    *p_ldrt = 0x58000000u | (((uint32_t)((Lt - (uint8_t *)p_ldrt) / 4) & 0x7FFFF) << 5) | 17;
    *p_cbnz = 0xB5000000u | (((uint32_t)(((uint8_t *)miss - (uint8_t *)p_cbnz) / 4) & 0x7FFFF) << 5) | 17;
    *p_ldrb = 0x58000000u | (((uint32_t)((Lb - (uint8_t *)p_ldrb) / 4) & 0x7FFFF) << 5) | 16;
    int64_t ao = Lt - (uint8_t *)p_adr;
    *p_adr = 0x10000000u | ((uint32_t)(ao & 3) << 29) | (((uint32_t)((ao >> 2) & 0x7FFFF)) << 5) | 9;
}

// ---------------- the translator ----------------
// Translate the basic block at guest address gpc; returns host entry pointer.
// re-target a cond branch to offset d (instrs)
static uint32_t recode_cond(uint32_t in, int64_t d) {
    // cbz/cbnz
    if ((in & 0x7E000000u) == 0x34000000u) return (in & 0xFF00001Fu) | ((uint32_t)(d & 0x7FFFF) << 5);
    // b.cond
    if ((in & 0xFF000010u) == 0x54000000u) return (in & 0xFF00000Fu) | ((uint32_t)(d & 0x7FFFF) << 5);
    // tbz/tbnz
    return (in & 0xFFF8001Fu) | ((uint32_t)(d & 0x3FFF) << 5);
}

// W4E tier-2: emit the in-cache back-edge hotness counter for a hot-candidate self-loop. Runs on the
// TAKEN (loop) edge in tier-1. Flag-free (sub-imm + cbnz never touch NZCV, so the guest's condition
// flags are preserved across the back-edge -- mandatory for bit-exactness when the loop body does not
// itself re-set the tested flags). x9/x10 are spilled to the [sp,#-16/-24] red zone (never live across a
// block boundary, same convention as emit_spill/IBTC). Counts DOWN from g_t2thresh; on reaching zero it
// exits R_TIER2 so the dispatcher promotes the block, after which this stub is dead.
static void emit_t2_counter(int slot, uint64_t start, void *body) {
    e_stur(9, 31, -16);
    e_stur(10, 31, -24);
    // x9 = &g_t2cnt[slot] (plain RW data; adrp+add reaches it)
    e_adrp_add(9, (uint64_t)&g_t2cnt[slot]);
    e_ldr(10, 9, 0);
    // --count (sub immediate: flag-free)
    e_subi(10, 10, 1);
    e_str(10, 9, 0);
    uint32_t *p_cbnz = (uint32_t *)g_cp;
    // cbnz x10, Lcont (still counting -> keep looping; flag-free)
    emit32(0);
    // reached 0 -> restore scratch + exit to the dispatcher to promote (pc = loop start)
    e_ldur(9, 31, -16);
    e_ldur(10, 31, -24);
    emit_exit_const(start, R_TIER2);
    uint8_t *Lcont = g_cp;
    *p_cbnz = 0xB5000000u | (((uint32_t)(((uint8_t *)Lcont - (uint8_t *)p_cbnz) / 4) & 0x7FFFF) << 5) | 10;
    e_ldur(9, 31, -16);
    e_ldur(10, 31, -24);
    // b body  (the loop back-edge, in-cache)
    int64_t d = ((uint8_t *)body - (uint8_t *)g_cp) / 4;
    emit32(0x14000000u | ((uint32_t)d & 0x3FFFFFFu));
}
// W4E tier-2: store-to-load-forwarding hazard guard. Folding the back-edge tightens the loop enough that
// a store immediately followed by a load of the SAME address (e.g. a volatile / aliased RMW of one stack
// slot every iteration) starts hitting an Apple-Silicon store-forwarding replay -- measured as a ~3.7x
// slowdown on a `volatile` counter loop, while the extra tier-1 trampoline branch happened to mask it. So
// if the loop body contains a store whose (size,base,offset) a later load reuses, leave the loop on tier-1
// (no counter, no fold). Pure-store, load-only, and distinct-address load+store loops are NOT flagged and
// still tier up (measured wins). Scans the guest body [start, endpc).
static int loop_has_rmw_hazard(uint64_t start, uint64_t endpc) {
    uint64_t stores[32];
    int ns = 0;
    for (uint64_t p = start; p < endpc; p += 4) {
        uint32_t in = *(uint32_t *)p;
        uint64_t key = 0;
        int opc = -1;
        // load/store unsigned imm12
        if ((in & 0x3B000000u) == 0x39000000u) {
            opc = (in >> 22) & 3;
            key = ((uint64_t)((in >> 30) & 3) << 24) | (((in >> 5) & 31) << 12) | ((in >> 10) & 0xFFF);
        }
        // STUR/LDUR unscaled imm9
        else if ((in & 0x3B200C00u) == 0x38000000u) {
            opc = (in >> 22) & 3;
            key = (1ull << 40) | ((uint64_t)((in >> 30) & 3) << 24) | (((in >> 5) & 31) << 12) | ((in >> 12) & 0x1FF);
        }
        if (opc == 0) {
            if (ns < 32) stores[ns++] = key; // a store
        } else if (opc > 0) {
            for (int i = 0; i < ns; i++)
                if (stores[i] == key) return 1; // a load reusing a stored address -> hazard
        }
    }
    return 0;
}

// W4E tier-2: emit a single-block self-loop's terminating conditional (taken target == block start).
//   tier-1 build: cond -> Lcnt (counter) ; fall-through = loop exit. The counter promotes when hot.
//   tier-2 build: cond -> body DIRECTLY (the fold) ; fall-through = loop exit. One taken branch/iter
//                 instead of tier-1's `b.cond Ltaken; b body` -- native-equivalent. Bit-identical control
//                 flow (same condition, same taken target = loop top). `body` is always a few insns above,
//                 well inside the conditional's imm19/imm14 reach.
static void emit_selfloop(uint32_t in, uint64_t start, uint64_t fall, void *body, int slot) {
    uint32_t *patch = (uint32_t *)g_cp;
    emit32(0);
    emit_chain_exit(fall);
    if (g_tier2_build) {
        int64_t d = ((uint8_t *)body - (uint8_t *)patch) / 4;
        *patch = recode_cond(in, d);
        return;
    }
    int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
    *patch = recode_cond(in, d);
    emit_t2_counter(slot, start, body);
}

// ---- LSE atomics idiom upgrade ----
// Distro binaries are built ARMv8.0-baseline, so every atomic is an ldxr/stxr retry
// loop. Apple Silicon has FEAT_LSE: recognize the loop and emit a single atomic op
// (2.29x faster, and it removes the load/store-exclusive monitor region that
// complicates the translator). AL ordering is always safe.
static void e_lse(uint32_t base, int sz, int rs, int rt, int rn) {
    emit32(base | (sz == 3 ? 0x40000000u : 0) | (rs << 16) | (rn << 5) | rt);
}
// add/orr/eor/subs (shifted, no shift)
static void e_op_reg(uint32_t base, int sz, int rd, int rn, int rm) {
    emit32(base | (sz == 3 ? 0x80000000u : 0) | (rm << 16) | (rn << 5) | rd);
}
// casal Rs(compare/old), Rt(new), [Rn]
static void e_cas(int sz, int rs, int rt, int rn) {
    emit32((sz == 3 ? 0xC8E0FC00u : 0x88E0FC00u) | (rs << 16) | (rn << 5) | rt);
}
// Returns bytes consumed (12 or 16) if a known atomic loop at gpc was rewritten, else 0.
static int try_lse_atomic(uint64_t gpc) {
    uint32_t i0 = *(uint32_t *)gpc;
    // load-exclusive?
    if ((i0 & 0x3F400000u) != 0x08400000u) return 0;
    int sz = (i0 >> 30) & 3;
    // word/dword only
    if (sz < 2) return 0;
    // non-pair
    if (((i0 >> 16) & 0x1F) != 0x1F || ((i0 >> 10) & 0x1F) != 0x1F) return 0;
    int Wt = i0 & 31, Xn = (i0 >> 5) & 31;
    uint32_t i1 = *(uint32_t *)(gpc + 4);

    // SWP:  ldxr Wt,[Xn]; stxr Ws,Wv,[Xn]; cbnz Ws,loop
    if ((i1 & 0x3F400000u) == 0x08000000u && ((i1 >> 30) & 3) == sz && ((i1 >> 10) & 0x1F) == 0x1F &&
        ((i1 >> 5) & 31) == Xn) {
        int Ws = (i1 >> 16) & 31, Wv = i1 & 31;
        uint32_t i2 = *(uint32_t *)(gpc + 8);
        if ((i2 & 0xFF000000u) == 0x35000000u && (i2 & 31) == Ws &&
            (gpc + 8 + (uint64_t)(sext((i2 >> 5) & 0x7FFFF, 19) << 2)) == gpc) {
            if (is_stolen(Wt) || is_stolen(Xn) || is_stolen(Wv)) return 0;
            e_lse(0xB8E08000u, sz, Wv, Wt, Xn);
            // swpal Wv, Wt, [Xn]
            g_lse_n++;
            return 12;
        }
    }
    // LDADD/LDSET/LDEOR/LDCLR/LDADD-neg:  ldxr Wt,[Xn]; <op> Ws2,Wt,Wm; stxr Ws,Ws2,[Xn]; cbnz Ws,loop
    // 0 add 1 orr 2 eor 3 and 4 sub
    int op = -1;
    if ((i1 & 0xFFE0FC00u) == (sz == 3 ? 0x8B000000u : 0x0B000000u))
        op = 0;
    else if ((i1 & 0xFFE0FC00u) == (sz == 3 ? 0xAA000000u : 0x2A000000u))
        op = 1;
    else if ((i1 & 0xFFE0FC00u) == (sz == 3 ? 0xCA000000u : 0x4A000000u))
        op = 2;
    else if ((i1 & 0xFFE0FC00u) == (sz == 3 ? 0x8A000000u : 0x0A000000u))
        op = 3;
    else if ((i1 & 0xFFE0FC00u) == (sz == 3 ? 0xCB000000u : 0x4B000000u))
        op = 4;
    if (op >= 0) {
        int Ws2 = i1 & 31, n = (i1 >> 5) & 31, m = (i1 >> 16) & 31, Wm = -1;
        if (op == 4) {
            if (n == Wt) Wm = m;
        // sub: not commutative, Rn must be Wt
        }
        else {
            if (n == Wt)
                Wm = m;
            else if (m == Wt)
                Wm = n;
        }
        uint32_t i2 = *(uint32_t *)(gpc + 8), i3 = *(uint32_t *)(gpc + 12);
        if (Wm >= 0 && (i2 & 0x3F400000u) == 0x08000000u && ((i2 >> 30) & 3) == sz && (i2 & 31) == Ws2 &&
            ((i2 >> 5) & 31) == Xn && ((i2 >> 10) & 0x1F) == 0x1F) {
            int Ws = (i2 >> 16) & 31;
            if ((i3 & 0xFF000000u) == 0x35000000u && (i3 & 31) == Ws &&
                (gpc + 12 + (uint64_t)(sext((i3 >> 5) & 0x7FFFF, 19) << 2)) == gpc) {
                if (is_stolen(Wt) || is_stolen(Xn) || is_stolen(Wm) || is_stolen(Ws2) || is_stolen(Ws)) return 0;
                // need Ws as scratch, can't be Wm
                if (op >= 3 && Wm == Ws) return 0;
                // reconstruct = the original op (re-emit)
                uint32_t rec = i1;
                if (op <= 2) {
                    uint32_t lse = op == 0 ? 0xB8E00000u : op == 1 ? 0xB8E03000u : 0xB8E02000u;
                    // ldaddal/ldsetal/ldeoral
                    e_lse(lse, sz, Wm, Wt, Xn);
                // fetch_and: *Xn &= Wm  ==  ldclr ~Wm
                } else if (op == 3) {
                    // mvn Ws, Wm  (orn Ws, wzr, Wm)
                    e_op_reg(0x2A200000u, sz, Ws, 31, Wm);
                    // ldclral Ws, Wt, [Xn]
                    e_lse(0xB8E01000u, sz, Ws, Wt, Xn);
                // fetch_sub: *Xn -= Wm  ==  ldadd -Wm
                } else {
                    // neg Ws, Wm  (sub Ws, wzr, Wm)
                    e_op_reg(0x4B000000u, sz, Ws, 31, Wm);
                    // ldaddal Ws, Wt, [Xn]
                    e_lse(0xB8E00000u, sz, Ws, Wt, Xn);
                }
                emit32(rec);
                // reconstruct new value (re-emit original op)
                g_lse_n++;
                return 16;
            }
        }
    }
    // LDADD immediate (fetch_add of a constant -- the headline refcount/counter case):
    //   ldxr Wt,[Xn]; add Ws2,Wt,#imm (sh=0); stxr Ws,Ws2,[Xn]; cbnz Ws,loop
    uint32_t addib = sz == 3 ? 0x91000000u : 0x11000000u;
    if ((i1 & 0xFFC00000u) == addib && ((i1 >> 5) & 31) == Wt) {
        int Ws2 = i1 & 31;
        unsigned imm = (i1 >> 10) & 0xFFF;
        uint32_t i2 = *(uint32_t *)(gpc + 8), i3 = *(uint32_t *)(gpc + 12);
        if ((i2 & 0x3F400000u) == 0x08000000u && ((i2 >> 30) & 3) == sz && (i2 & 31) == Ws2 && ((i2 >> 5) & 31) == Xn &&
            ((i2 >> 10) & 0x1F) == 0x1F) {
            int Ws = (i2 >> 16) & 31;
            if ((i3 & 0xFF000000u) == 0x35000000u && (i3 & 31) == Ws &&
                (gpc + 12 + (uint64_t)(sext((i3 >> 5) & 0x7FFFF, 19) << 2)) == gpc) {
                if (is_stolen(Wt) || is_stolen(Xn) || is_stolen(Ws2) || is_stolen(Ws)) return 0;
                // Ws (dead status reg) = imm
                e_movz(Ws, imm, 0);
                // ldaddal Ws, Wt, [Xn]
                e_lse(0xB8E00000u, sz, Ws, Wt, Xn);
                emit32(i1);
                // re-emit add Ws2, Wt, #imm (reconstruct)
                g_lse_n++;
                return 16;
            }
        }
    }
    // CAS:  ldxr Wt,[Xn]; cmp Wt,Wexp; b.ne out; stxr Ws,Wnew,[Xn]; cbnz Ws,loop; out:
    // subs wzr, Wt, Wexp (cmp)
    uint32_t subsb = sz == 3 ? 0xEB00001Fu : 0x6B00001Fu;
    if ((i1 & 0xFFE0FC1Fu) == subsb && ((i1 >> 5) & 31) == Wt) {
        int Wexp = (i1 >> 16) & 31;
        uint32_t i2 = *(uint32_t *)(gpc + 8), i3 = *(uint32_t *)(gpc + 12), i4 = *(uint32_t *)(gpc + 16);
        // b.ne
        if ((i2 & 0xFF00001Fu) == 0x54000001u
            && (i3 & 0x3F400000u) == 0x08000000u && ((i3 >> 30) & 3) == sz && ((i3 >> 10) & 0x1F) == 0x1F &&
            ((i3 >> 5) & 31) == Xn && (i4 & 0xFF000000u) == 0x35000000u && (i4 & 31) == ((i3 >> 16) & 31) &&
            // cbnz -> loop
            (gpc + 16 + (uint64_t)(sext((i4 >> 5) & 0x7FFFF, 19) << 2)) == gpc
            // b.ne -> out
            && (gpc + 8 + (uint64_t)(sext((i2 >> 5) & 0x7FFFF, 19) << 2)) == gpc + 20) {
            int Wnew = i3 & 31;
            if (is_stolen(Wt) || is_stolen(Xn) || is_stolen(Wexp) || is_stolen(Wnew) || Wt == Wexp) return 0;
            // mov Wt, Wexp (orr Wt, wzr, Wexp)
            e_op_reg(0x2A000000u, sz, Wt, 31, Wexp);
            // casal Wt, Wnew, [Xn]; Wt = old
            e_cas(sz, Wt, Wnew, Xn);
            e_op_reg(0x6B000000u, sz, 31, Wt, Wexp);
            // cmp Wt, Wexp (reproduce NZCV)
            g_lse_n++;
            return 20;
        }
    }
    return 0;
}

// ---- tier-2 substrate: the purity gate (the analyze() of trace_pipeline.c) ----
// Given a formed trace's instructions, return 1 only if it is safe to MEMOIZE:
// no syscall (svc) and no memory access at all -- so the result is fully determined
// by the input registers and there are no side effects. Conservative by construction:
// any load/store or syscall -> impure -> emit unoptimized (side effects must run).
// This is the gate that refuses the impure region in the pipeline (a wrong gate here
// is a miscompile). Linear in trace length, run once on promotion. Verified by
// TIER2_SELFTEST; wired into specialization when trace formation (the "form trace"
// step) lands -- the remaining substrate brick.
static int region_pure(const uint32_t *code, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t in = code[i];
        // svc -> side effect
        if (in == 0xD4000001u) return 0;
        // any load/store -> not register-determined
        if ((in & 0x0A000000u) == 0x08000000u) return 0;
    }
    // pure: register-to-register computation only
    return 1;
}

// ---- §B shadow-stack return prediction: the validated mechanism (PoC: shadow_stack.c) ----
// At a guest `bl`, record the guest return address. At a guest `ret`, classify the guest's x30:
//   FAST    -> matches the top of the shadow stack: the normal return; take a host `ret` (the
//              hardware RAS predicts it in ~1 insn instead of the ~14-insn ret-IBTC).
//   UNWIND  -> matches a deeper frame (longjmp / multi-frame return): pop to it, still correct.
//   FOREIGN -> not on the shadow (computed/tail return): fall back to the IBTC.
// Conservative: ONLY the FAST path takes the host ret; UNWIND/FOREIGN fall back, so a return can
// never land at the wrong target. The codegen that emits host bl/ret + the x30 steal wires onto
// this (the one subtlety past the PoC is x30's dual role: host return address vs guest-visible
// link value -- handled by keeping guest x30 in cpu->x[30] and validating here).
enum { SS_FAST, SS_UNWIND, SS_FOREIGN };
static inline void shadow_push(struct cpu *c, uint64_t guest_ret, uint64_t host_ret) {
    if (c->ssp < 1024) {
        c->sstk[2 * c->ssp] = guest_ret;
        c->sstk[2 * c->ssp + 1] = host_ret;
        c->ssp++;
    }
}
// matches on guest_ret (even index)
static int shadow_classify(struct cpu *c, uint64_t guest_x30) {
    if (c->ssp > 0 && c->sstk[2 * (c->ssp - 1)] == guest_x30) {
        c->ssp--;
        return SS_FAST;
    }
    for (uint64_t d = 2; d <= c->ssp && d <= 64; d++)
        if (c->sstk[2 * (c->ssp - d)] == guest_x30) {
            c->ssp -= d;
            return SS_UNWIND;
        }
    return SS_FOREIGN;
}

// ---- opt4: greedy superblock / trace formation ----
// Follow unconditional `b` edges INLINE, and lay conditional fall-through successors INLINE
// (inverting the guest condition so the TAKEN side becomes a tiny out-of-line chain-exit).
// A region is bounded to TRACE_MAX_BYTES / TRACE_MAX_BLK; intermediate guest block-starts are
// deliberately NOT registered in g_map -- any edge that later enters mid-region self-heals by
// re-translating a fresh (always-correct) duplicate, wired up through the existing
// add_pend/patch_links_to back-patch path. NOSTITCH=1 -> g_stitch=0 -> exact single-block
// baseline (env read once; set-once + idempotent under the JIT lock).
#define TRACE_MAX_BLK 16
#define TRACE_MAX_BYTES (16 * 1024)
static int g_stitch = -1;
static int seen_has(const uint64_t *seen, int n, uint64_t v) {
    for (int i = 0; i < n; i++)
        if (seen[i] == v) return 1;
    return 0;
}
// Lay a conditional's fall-through inline: `inv` is the branch insn with its condition/op
// already inverted, so when the guest would NOT take it we keep falling through. Emit the
// inverted branch (skips the taken-side exit), the taken chain-exit, then patch the branch to
// jump just past it. The patched offset is always tiny (the taken exit is ~1 insn if chained,
// ~30 if it spills) -> in range even for tbz/tbnz's 14-bit field.
static void stitch_cond(uint32_t inv, uint64_t taken) {
    uint32_t *patch = (uint32_t *)g_cp;
    emit32(0);
    emit_chain_exit(taken);
    *patch = recode_cond(inv, ((uint8_t *)g_cp - (uint8_t *)patch) / 4);
}

// ---- SMC: self-modifying-code support (gate NOSMC=1) ----
// NOSMC=1 reverts to the legacy behavior (guest `ic ivau` emitted verbatim, no translation-cache drop) so
// a stale-translation A/B is still possible. Read once (idempotent static guard).
static int g_smc_off = -1;
static int smc_disabled(void) {
    if (g_smc_off < 0) g_smc_off = (getenv("NOSMC") != NULL);
    return g_smc_off;
}
// A guest `ic ivau` reached the dispatcher (R_ICFLUSH): the guest is about to execute code it just rewrote,
// so every gpc->host translation may be stale. Drop the whole block map + IBTC + pending chains (mirrors the
// x86 smc_on_write flush). We deliberately do NOT reset g_cp: the just-exited block's host code stays intact
// and is reclaimed by the normal wholesale flush; stale entries are simply re-emitted on demand. The §B
// shadow stack is left alone -- its host_rets point at old code that is still present in g_cp (valid targets).
// g_smc_seen latches so indirect branches stop populating the per-site IC (see G_IBTC_FILL): that literal
// lives in the unmodified CALLER block, which this flush cannot reach.
static void smc_icflush(void) {
    g_smc_seen = 1;
    memset(g_map, 0, sizeof g_map);
    memset(g_ibtc, 0, sizeof g_ibtc);
    g_npend = 0;
    g_smc_flushes++;
}

static void *translate_block(uint64_t gpc) {
    // W4E tier-2: read NOTIER2 / TIER2_THRESHOLD once (idempotent) before any self-loop detection.
    tier2_env_init();
    // gpc is mutated by the decode loop; key the cache by START
    uint64_t start = gpc;
    void *host = g_cp;
    emit_prologue();
    // chained jumps land here (regs already live)
    void *body = g_cp;
    // ldxr/ldaxr..stxr/stlxr exclusive regions must stay in ONE block with no injected
    // memory ops between them, else the monitor clears and stxr retries forever. While
    // inside such a region, conditional branches are emitted inline and their exits are
    // deferred to stubs after the store-exclusive.
    int in_excl = 0;
    struct {
        uint32_t *patch;
        uint64_t target;
        uint32_t in;
    } defer[64];
    int ndefer = 0;
    // opt4 region state: guest block-starts inlined into this region + a block budget. The
    // region STOPS (falls to the baseline single-block exit) at any dispatcher-mediated edge
    // (indirect br/blr, bl/call, ret, svc/syscall), inside an exclusive monitor region, or on
    // hitting the 16-block / 16 KB bound -- "when unsure, end the region".
    if (g_stitch < 0) g_stitch = getenv("NOSTITCH") ? 0 : 1;
    uint64_t seen[TRACE_MAX_BLK];
    int nseen = 0, trace_blk = 0;
#define STITCH_OK \
    (g_stitch && !in_excl && trace_blk < TRACE_MAX_BLK - 1 && (g_cp - (uint8_t *)host) < TRACE_MAX_BYTES)
    for (;;) {
        uint32_t in = *(uint32_t *)gpc;
        g_emit_gpc = gpc; // ARM-B1 IBPROF: tag indirect-branch sites with their guest PC

        if (!in_excl && !getenv("NOLSE")) {
            int n = try_lse_atomic(gpc);
            if (n) {
                gpc += n;
                continue;
            }
        // ldxr/stxr loop -> LSE
        }
        if ((in & 0x3F400000u) == 0x08400000u)
            // load-exclusive
            in_excl = 1;
        else if (in_excl && (in & 0x3F400000u) == 0x08000000u)
            // store-exclusive
            in_excl = 0;

        // svc #0
        if (in == 0xD4000001u) {
            emit_exit_const(gpc, R_SYSCALL);
            break;
        }
        // b
        if ((in & 0xFC000000u) == 0x14000000u) {
            int64_t off = sext(in & 0x3FFFFFF, 26) << 2;
            uint64_t tgt = gpc + off;
            // opt4: follow the unconditional edge INLINE if its target is a fresh block (not the
            // region head, not already inlined, not already translated) -> the inter-block `b`
            // disappears. Otherwise chain normally (existing block / loop back-edge).
            if (STITCH_OK && tgt != start && !seen_has(seen, nseen, tgt) && !map_body(tgt)) {
                seen[nseen++] = tgt;
                trace_blk++;
                gpc = tgt;
                continue;
            }
            // ARM-B1 VDBETRACE: path-specialize the VDBE dispatch. The shared loop-top (jump-table)
            // block is already translated, so opt4's `!map_body` guard would chain to the ONE shared
            // dispatch (-> polymorphic `br`, SDC hit rate = order-0 ~6%). Force-inline a PRIVATE copy of
            // the dispatch into THIS predecessor so its `br` sees only this handler's successor
            // (order-1+, ~75-98% stable). Re-translation of a mid-region entry self-heals as usual.
            if (g_vdbetrace && g_stitch && !in_excl && (g_cp - (uint8_t *)host) < TRACE_MAX_BYTES &&
                tgt != start && !seen_has(seen, nseen, tgt) && nseen < TRACE_MAX_BLK - 1 &&
                is_jt_dispatch_block(tgt)) {
                g_vt_force_inl++;
                seen[nseen++] = tgt;
                trace_blk++;
                gpc = tgt;
                continue;
            }
            emit_chain_exit(tgt);
            break;
        }
        // bl
        if ((in & 0xFC000000u) == 0x94000000u) {
            int64_t off = sext(in & 0x3FFFFFF, 26) << 2;
            emit_bl_ras(gpc, gpc + off);
            // §B: shadow push + host bl (RAS) + Lcont continuation
            break;
        }
        // ret xN
        if ((in & 0xFFFFFC1Fu) == 0xD65F0000u) {
            int rrn = (in >> 5) & 31;
            if (rrn == 30)
                // A3: §B OFF (default) -> bare IBTC return (no shadow-ret preamble); §B ON -> shadow ret
                // (FAST host-ret on guest_ret+guest_sp match, else IBTC fallback).
                shadowgate() == -1 ? emit_ibranch(30) : emit_shadow_ret();
            else
                // ret xN via another reg -> ordinary indirect branch
                emit_ibranch(rrn);
            break;
        }
        // br
        if ((in & 0xFFFFFC1Fu) == 0xD61F0000u) {
            int brn = (in >> 5) & 31;
            // ARM-B1 VDBETRACE: a clang jump-table dispatch `br` -> speculative direct-chain (SDC)
            // instead of the polymorphic IBTC probe. (brn is a normal reg here -- never 16/17/18/28/30
            // for a compiler switch dispatch.) Bit-exact: SDC guard falls back to the shared-hash IBTC.
            if (g_vdbetrace && brn < 16 && is_jt_dispatch_br(gpc))
                emit_vdbe_sdc(brn);
            else
                emit_ibranch(brn);
            break;
        }
        // blr
        if ((in & 0xFFFFFC1Fu) == 0xD63F0000u) {
            // guest x30 lives in cpu->x[30] (stolen); RAS push needs a host blr
            emit_set_x30(gpc + 4);
            emit_ibranch((in >> 5) & 31);
            //   (Section 3) -- deferred; Stage-B IBTC for the function-ptr return
            break;
        }
        // b.cond
        if ((in & 0xFF000010u) == 0x54000000u) {
            int cond = in & 0xF;
            int64_t off = sext((in >> 5) & 0x7FFFF, 19) << 2;
            uint64_t taken = gpc + off, fall = gpc + 4;
            if (in_excl) {
                defer[ndefer].patch = (uint32_t *)g_cp;
                defer[ndefer].target = taken;
                defer[ndefer].in = in;
                ndefer++;
                emit32(0);
                gpc += 4;
                continue;
            }
            // W4E tier-2: single-block self-loop (taken back-edge == block start). Intercept BEFORE the
            // opt4 stitch so the redundant back-edge trampoline can be folded; non-self-loops (taken !=
            // start) fall through to opt4 unchanged. NOTIER2 -> skipped (exact committed-opt4 baseline).
            if (taken == start && !g_notier2 && !loop_has_rmw_hazard(start, gpc)) {
                int slot = g_tier2_build ? 0 : t2_slot(start);
                if (g_tier2_build || slot >= 0) {
                    emit_selfloop(in, start, fall, body, slot);
                    break;
                }
            }
            // opt4: lay the fall-through inline; invert the condition so TAKEN is the exit
            if (STITCH_OK && fall != start && !seen_has(seen, nseen, fall) && !map_body(fall)) {
                stitch_cond(in ^ 1u, taken);
                seen[nseen++] = fall;
                trace_blk++;
                gpc = fall;
                continue;
            }
            uint32_t *patch = (uint32_t *)g_cp;
            // b.cond -> taken (backpatched)
            emit32(0);
            emit_chain_exit(fall);
            int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
            *patch = 0x54000000u | ((uint32_t)(d & 0x7FFFF) << 5) | cond;
            emit_chain_exit(taken);
            break;
        }
        // cbz / cbnz
        if ((in & 0x7E000000u) == 0x34000000u) {
            int64_t off = sext((in >> 5) & 0x7FFFF, 19) << 2;
            uint64_t taken = gpc + off, fall = gpc + 4;
            int sf = in >> 31, op = (in >> 24) & 1, rt = in & 31;
            if (in_excl) {
                defer[ndefer].patch = (uint32_t *)g_cp;
                defer[ndefer].target = taken;
                defer[ndefer].in = in;
                ndefer++;
                emit32(0);
                gpc += 4;
                continue;
            }
            // W4E tier-2: single-block self-loop (non-stolen tested reg). Before opt4; NOTIER2 -> skipped.
            if (taken == start && !g_notier2 && !is_stolen(rt) && !loop_has_rmw_hazard(start, gpc)) {
                int slot = g_tier2_build ? 0 : t2_slot(start);
                if (g_tier2_build || slot >= 0) {
                    emit_selfloop(in, start, fall, body, slot);
                    break;
                }
            }
            // opt4: lay the fall-through inline (non-stolen test reg only); invert op (cbz<->cbnz)
            if (STITCH_OK && !is_stolen(rt) && fall != start && !seen_has(seen, nseen, fall) && !map_body(fall)) {
                stitch_cond(in ^ (1u << 24), taken);
                seen[nseen++] = fall;
                trace_blk++;
                gpc = fall;
                continue;
            }
            // tested reg stolen -> test cpu->x[rt] via a saved scratch
            if (is_stolen(rt)) {
                int S = 0;
                e_str(S, CPUREG, (int)OFF_MSCRATCH);
                e_ldr(S, CPUREG, rt * 8);
                uint32_t *patch = (uint32_t *)g_cp;
                // cbz/cbnz S -> taken
                emit32(0);
                e_ldr(S, CPUREG, (int)OFF_MSCRATCH);
                // fall: restore S
                emit_chain_exit(fall);
                int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
                *patch = 0x34000000u | ((unsigned)sf << 31) | ((unsigned)op << 24) | ((uint32_t)(d & 0x7FFFF) << 5) | S;
                e_ldr(S, CPUREG, (int)OFF_MSCRATCH);
                emit_chain_exit(taken);
                // taken: restore S
                break;
            }
            uint32_t *patch = (uint32_t *)g_cp;
            // cbz/cbnz rt -> taken
            emit32(0);
            emit_chain_exit(fall);
            int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
            *patch = 0x34000000u | ((unsigned)sf << 31) | ((unsigned)op << 24) | ((uint32_t)(d & 0x7FFFF) << 5) | rt;
            emit_chain_exit(taken);
            break;
        }
        // tbz / tbnz
        if ((in & 0x7E000000u) == 0x36000000u) {
            int b40 = (in >> 19) & 0x1F, bit5 = (in >> 31) & 1;
            int64_t off = sext((in >> 5) & 0x3FFF, 14) << 2;
            uint64_t taken = gpc + off, fall = gpc + 4;
            int op = (in >> 24) & 1, rt = in & 31;
            if (in_excl) {
                defer[ndefer].patch = (uint32_t *)g_cp;
                defer[ndefer].target = taken;
                defer[ndefer].in = in;
                ndefer++;
                emit32(0);
                gpc += 4;
                continue;
            }
            // W4E tier-2: single-block self-loop (non-stolen tested reg). Before opt4; NOTIER2 -> skipped.
            if (taken == start && !g_notier2 && !is_stolen(rt) && !loop_has_rmw_hazard(start, gpc)) {
                int slot = g_tier2_build ? 0 : t2_slot(start);
                if (g_tier2_build || slot >= 0) {
                    emit_selfloop(in, start, fall, body, slot);
                    break;
                }
            }
            // opt4: lay the fall-through inline (non-stolen test reg only); invert op (tbz<->tbnz)
            if (STITCH_OK && !is_stolen(rt) && fall != start && !seen_has(seen, nseen, fall) && !map_body(fall)) {
                stitch_cond(in ^ (1u << 24), taken);
                seen[nseen++] = fall;
                trace_blk++;
                gpc = fall;
                continue;
            }
            // tested reg stolen -> test cpu->x[rt] via a saved scratch
            if (is_stolen(rt)) {
                int S = 0;
                e_str(S, CPUREG, (int)OFF_MSCRATCH);
                e_ldr(S, CPUREG, rt * 8);
                uint32_t *patch = (uint32_t *)g_cp;
                // tbz/tbnz S,#bit -> taken
                emit32(0);
                e_ldr(S, CPUREG, (int)OFF_MSCRATCH);
                // fall: restore S
                emit_chain_exit(fall);
                int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
                *patch = 0x36000000u | ((unsigned)bit5 << 31) | ((unsigned)op << 24) | ((unsigned)b40 << 19) |
                         ((uint32_t)(d & 0x3FFF) << 5) | S;
                e_ldr(S, CPUREG, (int)OFF_MSCRATCH);
                emit_chain_exit(taken);
                // taken: restore S
                break;
            }
            uint32_t *patch = (uint32_t *)g_cp;
            // tbz/tbnz rt,#bit -> taken
            emit32(0);
            emit_chain_exit(fall);
            int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
            *patch = 0x36000000u | ((unsigned)bit5 << 31) | ((unsigned)op << 24) | ((unsigned)b40 << 19) |
                     ((uint32_t)(d & 0x3FFF) << 5) | rt;
            emit_chain_exit(taken);
            break;
        }

        // --- TLS: the whole point. mrs/msr tpidr_el0 become a single NATIVE
        //     load/store from cpu->tls. No trap, no Mach round-trip. ---
        // mrs xN, tpidr_el0  (TLS read, hot)
        if ((in & 0xFFFFFFE0u) == 0xD53BD040u) {
            int n = in & 31;
            if (is_stolen(n)) {
                x18_prolog();
                e_ldr(0, 1, OFF_TLS);
                e_str(0, 1, n * 8);
                x18_epilog();
            } else {
                e_load_cpu(n);
                e_ldr(n, n, OFF_TLS);
            }
            gpc += 4;
            continue;
        }
        // msr tpidr_el0, xN  (TLS write, rare)
        if ((in & 0xFFFFFFE0u) == 0xD51BD040u) {
            int n = in & 31, t = (n == 16) ? 15 : 16;
            if (is_stolen(n)) {
                x18_prolog();
                e_ldr(0, 1, n * 8);
                e_str(0, 1, OFF_TLS);
                x18_epilog();
            } else {
                e_stur(t, 31, -16);
                e_load_cpu(t);
                e_str(n, t, OFF_TLS);
                e_ldur(t, 31, -16);
            }
            gpc += 4;
            continue;
        }

        // --- SMC prerequisite: mrs Xt, ctr_el0 (cache-type register) ---
        // __clear_cache reads CTR_EL0 to size its dc/ic strides. Reading it from EL0 FAULTS for the JIT'd
        // guest on macOS (SCTLR_EL1.UCT is not enabled for it), so the verbatim mrs crashed every guest that
        // flushes its icache. Materialize a synthetic value describing the DBT's coherence model instead:
        //   IminLine/DminLine = 4 -> 64-byte I/D lines        L1Ip = PIPT
        //   IDC (bit28) = 1 -> "DC clean to PoU not required": TRUE here, the host re-translates the page by
        //                      reading the SAME coherent memory the guest wrote, so __clear_cache skips DC.
        //   DIC (bit29) = 0 -> "IC invalidate IS required": keeps the guest issuing `ic ivau`, our SMC hook.
        if ((in & 0xFFFFFFE0u) == 0xD53B0020u) {
            int rd = in & 31;
            uint64_t ctr = 0x9444C004ull;
            if (is_stolen(rd)) {
                x18_prolog();
                e_movconst(0, ctr);
                e_str(0, 1, rd * 8);
                x18_epilog();
            } else
                e_movconst(rd, ctr);
            gpc += 4;
            continue;
        }
        // --- SMC: dc cvau, Xt (data-cache clean to PoU) -> nop ---
        // A pure no-op for a DBT: the host never instruction-fetches guest pages, so the guest's data writes
        // need no clean for our re-translation (which is a normal coherent data read). Standard __clear_cache
        // already skips DC via IDC=1 above; this also covers callers that issue it unconditionally and avoids
        // any EL0 trap on the instruction. (NOSMC keeps it -- it is unrelated to the stale-translation A/B.)
        if ((in & 0xFFFFFFE0u) == 0xD50B7B20u) {
            emit32(0xD503201Fu); // nop
            gpc += 4;
            continue;
        }
        // --- SMC: ic ivau, Xt (instruction-cache invalidate by VA to PoU) ---
        // A code-generating guest issues this (the __clear_cache / dc;dsb;ic;dsb;isb dance) before running
        // freshly-written bytes. The host never instruction-fetches guest pages (we execute the TRANSLATED
        // copy), so emitting `ic ivau` verbatim is a no-op for our cache -> the guest would re-run the STALE
        // translation. Instead end the block here and exit R_ICFLUSH: the dispatcher drops the stale gpc->host
        // map + IBTC (smc_icflush) and the modified bytes re-translate. pc resumes PAST the ic ivau. Gated by
        // NOSMC; the dc cvau / isb in the same dance run verbatim (harmless: they touch real data memory).
        if ((in & 0xFFFFFFE0u) == 0xD50B7520u && !smc_disabled()) {
            emit_exit_const(gpc + 4, R_ICFLUSH);
            break;
        }

        // --- non-branch, PC-relative: rewrite to materialize the (relocated) addr ---
        // adr
        if ((in & 0x9F000000u) == 0x10000000u) {
            int rd = in & 31;
            int64_t imm = sext((((in >> 5) & 0x7FFFF) << 2) | ((in >> 29) & 3), 21);
            uint64_t v = pcrel_base(gpc) + imm;
            if (is_stolen(rd)) {
                x18_prolog();
                e_movconst(0, v);
                e_str(0, 1, rd * 8);
                x18_epilog();
            } else
                e_movconst(rd, v);
            gpc += 4;
            continue;
        }
        // adrp
        if ((in & 0x9F000000u) == 0x90000000u) {
            int rd = in & 31;
            int64_t imm = sext((((in >> 5) & 0x7FFFF) << 2) | ((in >> 29) & 3), 21) << 12;
            uint64_t v = (pcrel_base(gpc) & ~0xFFFull) + imm;
            if (is_stolen(rd)) {
                x18_prolog();
                e_movconst(0, v);
                e_str(0, 1, rd * 8);
                x18_epilog();
            } else
                e_movconst(rd, v);
            gpc += 4;
            continue;
        }
        // ldr (literal) 32/64
        if ((in & 0xBF000000u) == 0x18000000u) {
            int rt = in & 31, is64 = (in >> 30) & 1;
            int64_t off = sext((in >> 5) & 0x7FFFF, 19) << 2;
            if (is_stolen(rt)) {
                x18_prolog();
                e_movconst(0, gpc + off);
                if (is64)
                    e_ldr(0, 0, 0);
                else
                    emit32(0xB9400000u | (0 << 5) | 0);
                e_str(0, 1, rt * 8);
                x18_epilog();
            } else {
                e_movconst(rt, gpc + off);
                if (is64)
                    e_ldr(rt, rt, 0);
                else
                    emit32(0xB9400000u | (rt << 5) | rt);
            }
            gpc += 4;
            continue;
        }

        // pointer authentication (ubuntu 24.04 -mbranch-protection): we don't enforce PAC, and signing
        // x30 on the PAC-capable host would corrupt the §B shadow-stack return match (it expects an
        // UNSIGNED guest x30) -> wild branch to a signed address. Neutralize PAC (hardening, not
        // semantics): paci*/auti* hints -> nop (x30 stays unsigned); retaa/retab -> a plain x30 ret.
        // paciasp/autiasp/paci?z/... -> nop
        if ((in & 0xFFFFFF1Fu) == 0xD503231Fu) { emit32(0xD503201Fu); gpc += 4; continue; }
        // retaa/retab -> shadow ret (x30)
        if ((in & 0xFFFFFBFFu) == 0xD65F0BFFu) { emit_shadow_ret(); break; }

        // everything else: verbatim,
        int mask = gpr_field_mask(in);
        if (uses_x18(in, mask))
            // unless it names x18 -> mangle
            emit_mangled_x18(in, mask);
        else
            emit32(in);
        gpc += 4;
    }
    // emit the deferred exit stubs for branches taken inside an exclusive region
    for (int i = 0; i < ndefer; i++) {
        int64_t d = ((uint8_t *)g_cp - (uint8_t *)defer[i].patch) / 4;
        *defer[i].patch = recode_cond(defer[i].in, d);
        emit_chain_exit(defer[i].target);
    }
    // Only the REGION HEAD (start) is registered; intermediate inlined block-starts are left
    // unregistered so a later mid-region entry self-heals via re-translate + back-patch.
    // W4E tier-2: the promoter (g_tier2_build) recompiles in place and updates the EXISTING map entry
    // itself, so don't insert a duplicate. Expose the body for it.
    g_last_body = body;
    if (!g_tier2_build) map_put(start, host, body);
    // patch_links_to is MOVED to the dispatcher, AFTER the new block's icache is invalidated:
    // chaining an existing block X -> this new block before its code is icache-coherent on a peer
    // core lets that core fetch stale instructions. Only chain to it once it's visible everywhere.
    return host;
}
#undef STITCH_OK

// W4E tier-2: promote a hot self-loop (its in-cache counter hit the threshold and exited R_TIER2 with
// pc == gpc). Recompile the block with the folded back-edge (no counter), then SWAP it in under live
// execution: emit+icache-flush the tier-2 code, repoint the live map entry, repoint any still-pending
// chains, and drop a stale IBTC entry so the indirect path refills to tier-2. The old tier-1 code is left
// in place (harmless dead bytes). Single-threaded only -- promotion mutates the cache outside the threaded
// translate-lock discipline, so it's skipped once a guest thread exists (loop keeps running tier-1, still
// correct). Caller is the dispatcher between block runs, so guest state is settled.
static void tier2_promote(uint64_t gpc) {
    if (g_threaded || g_notier2) return;
    int mi = map_idx(gpc);
    if (mi < 0) return;
    pthread_jit_write_protect_np(0);
    g_emit_start = g_cp;
    g_tier2_build = 1;
    void *nh = translate_block(gpc); // folded recompile; no counter, no map_put
    void *nb = g_last_body;
    g_tier2_build = 0;
    if (getenv("T2DUMP")) {
        fprintf(stderr, "[t2dump] gpc=%llx body+%ld:", (unsigned long long)gpc, (long)((uint8_t *)nb - (uint8_t *)nh));
        for (uint32_t *p = (uint32_t *)nb; (uint8_t *)p < g_cp; p++)
            fprintf(stderr, " %08x", *p);
        fprintf(stderr, "\n");
    }
    // make the tier-2 code coherent on all cores BEFORE anything can branch into it
    sys_icache_invalidate(g_emit_start, (size_t)(g_cp - g_emit_start));
    // Redirect the OLD tier-1 body to tier-2: overwrite its first instruction with `b nb`. Chains from
    // predecessors were resolved to the old body when they were translated (patch_links_to only fixes
    // still-PENDING ones), so without this an outer loop re-entering this inner loop would keep hitting
    // the spent counter stub. The bounce costs one branch per loop ENTRY (negligible vs the loop body).
    void *old_body = g_map[mi].body;
    int64_t bd = ((uint8_t *)nb - (uint8_t *)old_body) / 4;
    *(uint32_t *)old_body = 0x14000000u | ((uint32_t)bd & 0x3FFFFFFu);
    sys_icache_invalidate(old_body, 4);
    // swap the live map entry: future dispatcher lookups + IBTC fills resolve to tier-2 directly
    g_map[mi].host = nh;
    g_map[mi].body = nb;
    // repoint any still-unresolved chains to this gpc straight at the tier-2 body
    patch_links_to(gpc, nb);
    // drop a stale IBTC entry (if this block is an indirect-branch target) so it refills to tier-2
    uint32_t h = (uint32_t)((gpc >> 2) & (IBTC_N - 1));
    if (g_ibtc[h].target == gpc) {
        g_ibtc[h].target = 0;
        g_ibtc[h].body = NULL;
    }
    pthread_jit_write_protect_np(1);
    g_prof_t2++;
}
