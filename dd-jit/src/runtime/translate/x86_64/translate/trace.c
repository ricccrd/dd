// ---- W3-A: trace / superblock formation (gate NOSTITCH=1; default ON) ----
// Port of the aarch64 opt4 PoC to the x86 engine. Greedily lay successor blocks inline:
//  - unconditional `jmp rel` (E9/EB): if the target is a fresh (untranslated, not-yet-inlined)
//    block, continue translating it inline -- the inter-block `b body` disappears.
//  - conditional `jcc` fall-through: lay the not-taken (`next`) successor inline, branchless.
//    The ARM condition is INVERTED so the emitted b.cond jumps over the (tiny, out-of-line)
//    taken-side exit; the taken successor becomes the out-of-line exit.
// Intermediate blocks are deliberately NOT registered in g_map, so any mid-region entry simply
// re-translates a (correct) duplicate and self-heals via the existing add_pend/patch_links_to
// back-patch path. Region bounded to TRACE_MAX_BLK blocks / TRACE_MAX_BYTES host bytes.
//
// x86 lazy-flag interplay (opt3, the critical correctness point): a width-4/8 sub/cmp/add/logic
// producer DEFERS its NZCV materialization, leaving its result flags live in ARM NZCV with
// g_fl_pending naming the finalizer that would spill them to cpu->nzcv. A chained/inlined entry
// reaches a successor's post-prologue body (no NZCV reload), so cpu->nzcv MUST be canonical at
// every stitched boundary:
//  - `jmp` stitch is flag-clean for free: the top-of-loop already materializes g_fl_pending
//    before any non-Jcc instruction (the jmp itself), so g_fl_pending == FL_NONE and the membank
//    is current when the jmp handler runs -- inline continuation needs nothing extra.
//  - `jcc` fall-through stitch runs AFTER the existing fast-path flags_materialize() (the
//    producer's exact spill, which also msr's the value back so live NZCV stays canonical). So
//    the out-of-line taken exit AND the inline fall-through both see a correct cpu->nzcv, and
//    g_fl_pending == FL_NONE for the inlined successor.
static int g_stitch = -1; // -1 uninit; resolved lazily from env (NOSTITCH=1 disables)
#define TRACE_MAX_BLK 16
#define TRACE_MAX_BYTES (16u * 1024u)
static int seen_has(const uint64_t *s, int n, uint64_t v) {
    for (int i = 0; i < n; i++)
        if (s[i] == v) return 1;
    return 0;
}
// A successor whose first byte is a trap (hlt / int3 / ud2) is a dynamically-dead guard arm --
// e.g. musl's alloca size check `cmp size,0xfff; jbe ok; hlt`, where the hlt fall-through is the
// never-taken oversize trap. Do NOT eagerly inline it: leave it as a normal out-of-line exit so
// it is only ever translated if actually reached (it isn't), avoiding wasted code + report_unimpl.
static int trap_head(uint64_t a) {
    const uint8_t *p = (const uint8_t *)a;
    return p[0] == 0xF4 || p[0] == 0xCC || (p[0] == 0x0F && p[1] == 0x0B);
}

// ==================== W5B adaptive tier-2 (x86 engine) ====================
// emit the in-cache back-edge hotness counter for a hot-candidate self-loop (tier-1 build only). Runs on
// the TAKEN (loop) edge. x16/x17 are host scratch here (NOT guest regs on the x86 engine: guest GPRs are
// x0..x15), so no spill is needed — simpler than the aarch64 port. The counter is flag-free (movconst /
// ldr / sub-imm(D1) / str / cbnz never touch NZCV), so the guest's condition flags survive the back-edge.
// Counts DOWN from g_t2thresh; on reaching zero it exits R_TIER2 (rip = loop start) so the dispatcher
// promotes the block, after which this stub is dead.
static void emit_t2_counter_x86(int slot, uint64_t start, void *body) {
    emit_host_ptr(16, (uint64_t)&g_t2cnt[slot], PRELOC_HOSTGLOBAL); // x16 = &g_t2cnt[slot]  (plain RW data)
    e_ldr(17, 16, 0);                         // x17 = count
    e_subi(17, 17, 1, 1);                      // --count (sub-imm: flag-free)
    e_str(17, 16, 0);
    uint32_t *p_cbnz = (uint32_t *)g_cp;
    emit32(0); // cbnz x17, Lcont (still counting -> keep looping; flag-free)
    // reached 0 -> exit to the dispatcher to promote (rip = loop start; counter dead afterwards)
    emit_exit_const(start, R_TIER2);
    uint8_t *Lcont = g_cp;
    *p_cbnz = 0xB5000000u | (((uint32_t)(((uint8_t *)Lcont - (uint8_t *)p_cbnz) / 4) & 0x7FFFF) << 5) | 17;
    int64_t d = ((uint8_t *)body - (uint8_t *)g_cp) / 4; // b body (the loop back-edge, in-cache)
    emit32(0x14000000u | ((uint32_t)d & 0x3FFFFFFu));
}

// store-to-load-forwarding hazard guard (ported verbatim from W4E): folding the back-edge tightens the
// loop enough that a store immediately followed by a load of the SAME address starts hitting an
// Apple-Silicon store-forwarding replay. Scan the EMITTED host code [body,end) for a load that reuses a
// stored (size,base,offset); if found, leave the loop on tier-1 (no counter, no fold). Pure-store /
// load-only / distinct-address loops are NOT flagged and still tier up.
static int loop_has_rmw_hazard(uint64_t body, uint64_t end) {
    uint64_t stores[32];
    int ns = 0;
    for (uint64_t p = body; p < end; p += 4) {
        uint32_t in = *(uint32_t *)p;
        uint64_t key = 0;
        int opc = -1;
        if ((in & 0x3B000000u) == 0x39000000u) { // load/store unsigned imm12
            opc = (in >> 22) & 3;
            key = ((uint64_t)((in >> 30) & 3) << 24) | (((in >> 5) & 31) << 12) | ((in >> 10) & 0xFFF);
        } else if ((in & 0x3B200C00u) == 0x38000000u) { // STUR/LDUR unscaled imm9
            opc = (in >> 22) & 3;
            key = (1ull << 40) | ((uint64_t)((in >> 30) & 3) << 24) | (((in >> 5) & 31) << 12) | ((in >> 12) & 0x1FF);
        }
        if (opc == 0) {
            if (ns < 32) stores[ns++] = key;
        } else if (opc > 0) {
            for (int i = 0; i < ns; i++)
                if (stores[i] == key) return 1;
        }
    }
    return 0;
}

// flag-liveness class of a guest x86 insn (for the dead-flag-save elision proof):
//   2 = full NZCV producer (writes all flags, reads none): add/or/and/sub/xor/cmp/test/neg
//   1 = flag CONSUMER (reads flags): adc/sbb/jcc/setcc/cmovcc
//   0 = flag-transparent and reads no flags: mov/lea/push/pop/movzx/movsx/inc/dec/not/nop
//  -1 = anything else -> treat conservatively as flag-live (keeps the save). Default-unknown is SAFE.
static int x86_flag_class(struct insn *I) {
    uint8_t op = I->op;
    if (I->two) {
        if ((op & 0xF0) == 0x80) return 1;                          // jcc rel32
        if ((op & 0xF0) == 0x90) return 1;                          // setcc
        if ((op & 0xF0) == 0x40) return 1;                          // cmovcc
        if (op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF) return 0; // movzx/movsx
        return -1;                                                  // unknown 2-byte
    }
    if (op <= 0x3D && (op & 7) <= 5) {                              // primary ALU 00..3D
        int k = (op >> 3) & 7;
        return (k == 2 || k == 3) ? 1 : 2;                         // adc/sbb read CF; rest full producers
    }
    if (op == 0x80 || op == 0x81 || op == 0x83) {                  // group1 ALU r/m,imm
        int k = I->reg & 7;
        return (k == 2 || k == 3) ? 1 : 2;
    }
    if (op == 0x84 || op == 0x85 || op == 0xA8 || op == 0xA9) return 2; // test
    if (op == 0xF6 || op == 0xF7) {                                // group3
        int k = I->reg & 7;
        if (k == 0 || k == 1 || k == 3) return 2;                  // test imm / neg
        if (k == 2) return 0;                                      // not (no flags)
        return -1;                                                 // mul/imul/div/idiv
    }
    if (op == 0xFE) { int k = I->reg & 7; return (k == 0 || k == 1) ? 0 : -1; } // inc/dec byte
    if (op == 0xFF) { int k = I->reg & 7; return (k == 0 || k == 1) ? 0 : -1; } // inc/dec (call/jmp/push -> -1)
    if ((op >= 0x88 && op <= 0x8B) || op == 0x8D) return 0;        // mov r/m,r ; r,r/m ; lea
    if ((op >= 0xB0 && op <= 0xBF) || op == 0xC6 || op == 0xC7) return 0; // mov imm
    if ((op >= 0x50 && op <= 0x5F) || op == 0x68 || op == 0x6A) return 0; // push/pop
    if (op == 0x90) return 0;                                      // nop
    return -1;                                                     // conservative
}

// Decode the self-loop body from `start` and decide whether the guest's NZCV is DEAD at loop top, i.e.
// the first flag-touching insn is a full producer (so on the back-edge the loop overwrites flags before
// any read). Returns 1 -> the per-iteration NZCV save can be elided onto the loop-exit edge. Returns 0
// (keep the save) on any consumer-before-producer or any unrecognized/uncertain insn. Conservative + safe.
static int loop_flags_dead(uint64_t start) {
    uint64_t gpc = start;
    int produced = 0;
    for (int n = 0; n < 256; n++) {
        struct insn I;
        decode(gpc, &I);
        if (I.len == 0) return 0;
        uint8_t op = I.op;
        // terminator of a self-loop block: the loop jcc (reads flags produced THIS iter iff produced)
        if ((!I.two && op >= 0x70 && op <= 0x7F) || (I.two && (op & 0xF0) == 0x80)) return produced;
        // any other control-flow ends the scan too
        if (!I.two && (op == 0xE9 || op == 0xEB || op == 0xE8 || op == 0xC3 || op == 0xC2 || op == 0xE3 ||
                       (op == 0xFF && (I.reg & 7) >= 2)))
            return produced;
        int cl = x86_flag_class(&I);
        if (cl == 1) {
            if (!produced) return 0; // consumer reads flags from a prior iteration -> live at top
        } else if (cl == 2) {
            produced = 1;
        } else if (cl != 0) {
            return 0; // unknown -> conservative
        }
        gpc += I.len;
    }
    return 0;
}

// Emit a single-block self-loop terminating jcc (taken target == block start). `cc` is the ARM cond.
// TIER-1 (with counter): flag handling byte-identical to the baseline jcc handler (opt3 lazy flags:
//   if a producer's flags are deferred, flags_materialize() spills them to cpu->nzcv AND leaves the
//   live ARM NZCV canonical; otherwise e_nzcv_load() reloads cpu->nzcv) -- only the back-edge differs:
//   it routes through emit_t2_counter_x86 (which promotes when hot) instead of a plain `b body`.
// TIER-2 (g_tier2_build): FOLD the trampoline to a single `b.cond body`; additionally, when the deferred
//   flags are a *sub/cmp* producer (g_fl_pending == FL_SUB) and provably dead at loop top, ELIDE the
//   per-iteration `mrs;str` save onto the loop-exit (fall) edge only. The elision is restricted to
//   FL_SUB because its finalizer (e_nzcv_save) leaves the live ARM NZCV already in the canonical borrow
//   convention x86cc_to_arm() assumes -- so the back-edge branch can read the live NZCV directly and the
//   save is pure spill-for-successor. FL_ADD/FL_LOGIC finalizers msr a *corrected* value into the live
//   NZCV, so they MUST materialize before the branch (fold-only). Bit-identical guest-visible control
//   flow + cpu->nzcv in every case.
static void emit_selfloop_x86(int cc, uint64_t start, uint64_t fall, void *body, int slot) {
    if (g_tier2_build) {
        int dead = (g_fl_pending == FL_SUB) && !getenv("NOFLAGELIDE") && loop_flags_dead(start);
        if (g_fl_pending == FL_SUB) {
            if (dead) {
                // FOLD + ELIDE: branch off the live NZCV (still holds the subs result); save only on the
                // loop-exit (fall) path. e_nzcv_save here == the FL_SUB finalizer flags_materialize would
                // emit, so the exit successor reads byte-identical cpu->nzcv.
                uint32_t *patch = (uint32_t *)g_cp;
                emit32(0);          // b.cond -> body (filled below)
                e_nzcv_save();      // loop-exit: materialize for the successor block's prologue
                g_fl_pending = FL_NONE;
                emit_chain_exit(fall);
                int64_t d = ((uint8_t *)body - (uint8_t *)patch) / 4;
                *patch = 0x54000000u | (((uint32_t)d & 0x7FFFF) << 5) | (cc & 0xF);
                g_prof_t2fold++;
                return;
            }
            flags_materialize(); // FOLD only: spill before the branch (FL_SUB -> e_nzcv_save, == tier-1)
        } else if (g_fl_pending) {
            flags_materialize(); // FL_ADD/FL_LOGIC: materialize (msr's corrected NZCV) before the branch
        } else {
            e_nzcv_load();
        }
        uint32_t *patch = (uint32_t *)g_cp;
        emit32(0); // b.cond -> body
        emit_chain_exit(fall);
        int64_t d = ((uint8_t *)body - (uint8_t *)patch) / 4;
        *patch = 0x54000000u | (((uint32_t)d & 0x7FFFF) << 5) | (cc & 0xF);
        return;
    }
    // TIER-1: flag handling byte-identical to baseline; only the back-edge differs (counter -> b body).
    if (g_fl_pending)
        flags_materialize();
    else
        e_nzcv_load();
    uint32_t *patch = (uint32_t *)g_cp;
    emit32(0);             // b.cond -> Lcnt (counter)
    emit_chain_exit(fall); // fall = loop exit
    int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
    *patch = 0x54000000u | (((uint32_t)d & 0x7FFFF) << 5) | (cc & 0xF);
    emit_t2_counter_x86(slot, start, body);
}
