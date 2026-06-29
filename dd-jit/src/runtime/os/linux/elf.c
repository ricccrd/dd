// dd/runtime/os/linux -- the ELF loader (map PT_LOAD high; static-PIE + dynamic via ld.so; build stack).

// ---------------- minimal ELF loader (load segments HIGH; PC-relative stays valid) ----------------
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

// Read PT_INTERP (the dynamic loader path) out of an ELF.
static int elf_interp(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    fstat(fd, &st);
    uint8_t *f = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED) return -1;
    int r = -1;
    uint64_t phoff = rd64(f + 32);
    int phnum = rd16(f + 56), phent = rd16(f + 54);
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = f + phoff + (size_t)i * phent;
        if (rd32(ph) == 3) {
            // PT_INTERP
            uint64_t off = rd64(ph + 8), fsz = rd64(ph + 32);
            size_t l = fsz < n ? fsz : n - 1;
            memcpy(out, f + off, l);
            out[l] = 0;
            r = 0;
            break;
        }
    }
    munmap(f, st.st_size);
    return r;
}

// ---------------- non-PIE absolute-DATA fixup (SIGSEGV/SIGBUS guard) ----------------
// A non-PIE ET_EXEC links at a fixed low vaddr with absolute code AND data refs baked in. load_elf maps
// the image HIGH (macOS reserves the low 4 GB via __PAGEZERO) and records the original low link range
// [g_nonpie_lo,g_nonpie_hi) plus the bias to the real mapping. The dispatcher already redirects absolute
// *code* jumps (g_nonpie_*); this handler catches the absolute *data* accesses. On a fault whose access
// address lands in the original low range, it decodes the native arm64 load/store the JIT emitted -- the
// aarch64 frontend copies guest loads/stores nearly verbatim, so the full LDR/STR/LDP/STP/SIMD/LSE family
// can appear -- re-serves the access at addr+bias, applies any base-register writeback, advances the host
// PC past the faulting instruction, and resumes. Inert unless a non-PIE image is loaded (g_nonpie_lo == 0
// for PIE / static-PIE, the only state the test matrix ever sees). A form we cannot decode returns 0 so
// the guard re-raises = a clean abort, never silent wrong data.
static int64_t nonpie_sext(uint64_t v, int bits) {
    uint64_t m = 1ull << (bits - 1);
    return (int64_t)((v ^ m) - m);
}
// Atomic RMW helpers (truly atomic, width-typed) used by the LSE/CAS fixup paths below.
static int nonpie_lse_rmw(void *p, int size, int opc, uint64_t v, uint64_t *old) {
    // opc: 0=ADD 1=CLR(&~) 2=EOR 3=SET(|). Returns 1 if handled, 0 for an unsupported subform.
    switch (size) {
#define NP_RMW(TY)                                                                                                     \
    {                                                                                                                  \
        TY *a = (TY *)p, ov = (TY)v, o;                                                                                \
        switch (opc) {                                                                                                 \
        case 0: o = __atomic_fetch_add(a, ov, __ATOMIC_SEQ_CST); break;                                               \
        case 1: o = __atomic_fetch_and(a, (TY)~ov, __ATOMIC_SEQ_CST); break;                                          \
        case 2: o = __atomic_fetch_xor(a, ov, __ATOMIC_SEQ_CST); break;                                               \
        case 3: o = __atomic_fetch_or(a, ov, __ATOMIC_SEQ_CST); break;                                                \
        default: return 0;                                                                                            \
        }                                                                                                              \
        *old = (uint64_t)o;                                                                                            \
        return 1;                                                                                                      \
    }
    case 0: NP_RMW(uint8_t)
    case 1: NP_RMW(uint16_t)
    case 2: NP_RMW(uint32_t)
    default: NP_RMW(uint64_t)
#undef NP_RMW
    }
}
static uint64_t nonpie_lse_swp(void *p, int size, uint64_t v) {
    switch (size) {
    case 0: return __atomic_exchange_n((uint8_t *)p, (uint8_t)v, __ATOMIC_SEQ_CST);
    case 1: return __atomic_exchange_n((uint16_t *)p, (uint16_t)v, __ATOMIC_SEQ_CST);
    case 2: return __atomic_exchange_n((uint32_t *)p, (uint32_t)v, __ATOMIC_SEQ_CST);
    default: return __atomic_exchange_n((uint64_t *)p, v, __ATOMIC_SEQ_CST);
    }
}
static uint64_t nonpie_cas(void *p, int size, uint64_t expected, uint64_t newv) {
    // Compare-and-swap; returns the pre-CAS memory value. __atomic_compare_exchange_n leaves the loaded
    // value in `e` on failure, and `e` unchanged (== old, since it matched) on success -> `e` is the old
    // value in both cases, which is what cas writes back into Rs.
    switch (size) {
    case 0: {
        uint8_t e = (uint8_t)expected;
        __atomic_compare_exchange_n((uint8_t *)p, &e, (uint8_t)newv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return e;
    }
    case 1: {
        uint16_t e = (uint16_t)expected;
        __atomic_compare_exchange_n((uint16_t *)p, &e, (uint16_t)newv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return e;
    }
    case 2: {
        uint32_t e = (uint32_t)expected;
        __atomic_compare_exchange_n((uint32_t *)p, &e, (uint32_t)newv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return e;
    }
    default: {
        uint64_t e = expected;
        __atomic_compare_exchange_n((uint64_t *)p, &e, newv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return e;
    }
    }
}
// zero-extend a `size`-byte value to register width (matches W-register upper-32 clearing for size<8).
static uint64_t nonpie_zext(uint64_t v, int size) { return size >= 3 ? v : (v & ((1ull << (8 << size)) - 1)); }

static int nonpie_fixup(siginfo_t *si, void *ucv) {
    if (!g_nonpie_lo || !ucv || !si) return 0;
    uint64_t va = (uint64_t)si->si_addr;
    if (va < g_nonpie_lo || va >= g_nonpie_hi) return 0;
    ucontext_t *uc = (ucontext_t *)ucv;
    uint32_t insn = *(uint32_t *)(uc->uc_mcontext->__ss.__pc);
    uint64_t real = va + g_nonpie_bias;         // the datum's real (high) mapped location
    uint64_t *X = uc->uc_mcontext->__ss.__x;    // __x[0..28] then fp/lr/sp contiguous -> X[29]=fp X[30]=lr X[31]=sp
    __uint128_t *V = uc->uc_mcontext->__ns.__v; // SIMD/FP register file
    int rt = insn & 0x1F;

    // ---- Load/store PAIR (LDP/STP, GPR + SIMD; no-alloc / offset / pre / post). (insn&0x3a000000)==0x28000000.
    if ((insn & 0x3a000000u) == 0x28000000u) {
        int v = (insn >> 26) & 1;   // SIMD&FP pair
        int load = (insn >> 22) & 1;
        int op2 = (insn >> 23) & 3; // 00=no-alloc 01=post 10=offset 11=pre
        int opc = insn >> 30;       // GPR: 00=32b 01=LDPSW 10=64b ; SIMD: 00=S 01=D 10=Q
        int rt2 = (insn >> 10) & 0x1F;
        int bytes = v ? (4 << opc) : (opc == 2 ? 8 : 4);
        if (v) { // SIMD pair
            if (load) {
                __uint128_t a = 0, b = 0;
                memcpy(&a, (void *)real, (size_t)bytes);
                memcpy(&b, (void *)(real + bytes), (size_t)bytes);
                V[rt] = a;
                V[rt2] = b;
            } else {
                __uint128_t a = V[rt], b = V[rt2];
                memcpy((void *)real, &a, (size_t)bytes);
                memcpy((void *)(real + bytes), &b, (size_t)bytes);
            }
        } else { // GPR pair (LDPSW sign-extends each 32b element to 64b)
            if (load) {
                uint64_t a = 0, b = 0;
                memcpy(&a, (void *)real, (size_t)bytes);
                memcpy(&b, (void *)(real + bytes), (size_t)bytes);
                if (opc == 1) {
                    a = (uint64_t)nonpie_sext(a, 32);
                    b = (uint64_t)nonpie_sext(b, 32);
                }
                if (rt != 31) X[rt] = a;
                if (rt2 != 31) X[rt2] = b;
            } else {
                uint64_t a = (rt == 31) ? 0 : X[rt], b = (rt2 == 31) ? 0 : X[rt2];
                memcpy((void *)real, &a, (size_t)bytes);
                memcpy((void *)(real + bytes), &b, (size_t)bytes);
            }
        }
        if (op2 == 1 || op2 == 3) { // writeback: post -> Xn=va+off, pre -> Xn=va (keep guest addr, not biased)
            int rn = (insn >> 5) & 0x1F;
            int64_t off = nonpie_sext((insn >> 15) & 0x7F, 7) * bytes;
            X[rn] = (op2 == 1) ? va + off : va;
        }
        uc->uc_mcontext->__ss.__pc += 4;
        return 1;
    }

    // ---- LSE atomic RMW: size[31:30] 111 0 00 A R 1 Rs[20:16] o3[15] opc[14:12] 00 Rn[9:5] Rt[4:0] ----
    if ((insn & 0x3F200C00u) == 0x38200000u) {
        int size = insn >> 30, o3 = (insn >> 15) & 1, opc = (insn >> 12) & 7;
        int rs = (insn >> 16) & 0x1F;
        uint64_t operand = (rs == 31) ? 0 : X[rs], old;
        if (o3 && opc == 0) { // swp: x = [m]; [m] = operand
            old = nonpie_lse_swp((void *)real, size, operand);
        } else if (!o3 && opc < 4) { // ldadd / ldclr / ldeor / ldset
            if (!nonpie_lse_rmw((void *)real, size, opc, operand, &old)) return 0;
        } else {
            return 0; // signed/unsigned min/max -> clean abort
        }
        if (rt != 31) X[rt] = nonpie_zext(old, size); // Rt receives the old value
        uc->uc_mcontext->__ss.__pc += 4;
        return 1;
    }

    // ---- CAS/CASA/CASL/CASAL (B/H/W/X): size[31:30] 001000 1 A 1 Rs[20:16] R 11111 Rn[9:5] Rt[4:0].
    //      A(bit22)/R(bit15) are the acquire/release flavors (all served as SEQ_CST). Rs=cmp in / old out. ----
    if ((insn & 0x3FA07C00u) == 0x08A07C00u) {
        int size = insn >> 30, rs = (insn >> 16) & 0x1F;
        uint64_t expected = (rs == 31) ? 0 : X[rs], newv = (rt == 31) ? 0 : X[rt];
        uint64_t old = nonpie_cas((void *)real, size, expected, newv);
        if (rs != 31) X[rs] = nonpie_zext(old, size); // Rs receives the old value
        uc->uc_mcontext->__ss.__pc += 4;
        return 1;
    }

    // ---- Load/store exclusive + ordered, single register: LDXR/STXR/LDAXR/STLXR/LDAR/STLR/LDLAR/STLLR.
    //      bits[29:24]==001000, o1(bit21)==0 (the CAS o1==1 forms are handled above; pair STXP/LDXP/CASP,
    //      o1==1, are rare and left to clean-abort). Served non-atomically at +bias; STXR reports success. ----
    if ((insn & 0x3F200000u) == 0x08000000u) {
        int size = insn >> 30, o2 = (insn >> 23) & 1, load = (insn >> 22) & 1, rs = (insn >> 16) & 0x1F;
        if (load) {
            uint64_t val = 0;
            memcpy(&val, (void *)real, (size_t)(1 << size));
            if (rt != 31) X[rt] = val;
        } else {
            uint64_t val = (rt == 31) ? 0 : X[rt];
            memcpy((void *)real, &val, (size_t)(1 << size));
            if (!o2 && rs != 31) X[rs] = 0; // store-exclusive status register: report success
        }
        uc->uc_mcontext->__ss.__pc += 4;
        return 1;
    }

    // ---- Load/store register, single (unsigned-offset / unscaled / pre / post / unpriv / register-offset).
    //      bits[29:27]==111; V=bit26. Addressing: bit24=scaled-unsigned, else bit21=register-offset, and
    //      bits[11:10] select unscaled(00)/post(01)/unpriv(10)/pre(11). Reject anything else (clean abort).
    if (((insn >> 27) & 7) != 7) return 0;
    int v = (insn >> 26) & 1;
    int scaled = (insn >> 24) & 1;
    int regoff = (insn >> 21) & 1;
    int mode = (insn >> 10) & 3;
    if (!scaled && regoff && mode != 2) return 0; // unallocated / atomic subform we don't decode
    int size = insn >> 30, opc = (insn >> 22) & 3;

    if (v) { // SIMD&FP single. width = 1<<((opc[1]<<2)|size): B1 H2 S4 D8 Q16. opc[0]=load.
        int bytes = 1 << (((opc >> 1) << 2) | size);
        if (opc & 1) {
            __uint128_t z = 0;
            memcpy(&z, (void *)real, (size_t)bytes);
            V[rt] = z;
        } else {
            __uint128_t s = V[rt];
            memcpy((void *)real, &s, (size_t)bytes);
        }
    } else if (opc == 0) { // integer store: rt's low `size` bytes
        uint64_t val = (rt == 31) ? 0 : X[rt];
        memcpy((void *)real, &val, (size_t)(1 << size));
    } else if (size == 3 && opc == 2) { // PRFM (prefetch hint) -- no transfer
        // fall through to PC advance
    } else { // integer load: 01=zero-ext, 10=sign-ext to 64, 11=sign-ext to 32 (W, upper zeroed)
        uint64_t val = 0;
        memcpy(&val, (void *)real, (size_t)(1 << size));
        if (opc == 2)
            val = (uint64_t)nonpie_sext(val, 8 << size);
        else if (opc == 3)
            val = (uint32_t)nonpie_sext(val, 8 << size);
        if (rt != 31) X[rt] = val;
    }
    if (!scaled && !regoff && (mode == 1 || mode == 3)) { // pre/post writeback (imm9): keep the guest addr
        int rn = (insn >> 5) & 0x1F;
        int64_t off = nonpie_sext((insn >> 12) & 0x1FF, 9);
        X[rn] = (mode == 1) ? va + off : va; // post -> Xn=va+imm, pre -> Xn=va
    }
    uc->uc_mcontext->__ss.__pc += 4;
    return 1;
}
// SIGSEGV/SIGBUS guard installed on the normal aarch64 run path. Serves a non-PIE absolute data access at
// +bias (nonpie_fixup); anything else re-raises with the default action (a real crash). Inert for PIE.
static void nonpie_guard(int sig, siginfo_t *si, void *uc) {
    if (nonpie_fixup(si, uc)) return;
    signal(sig, SIG_DFL);
    raise(sig);
}

static void load_elf(const char *path, struct loaded *out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        exit(1);
    }
    struct stat st;
    fstat(fd, &st);
    uint8_t *f = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (f == MAP_FAILED) {
        perror("mmap elf");
        exit(1);
    }
    uint64_t e_entry = rd64(f + 24), phoff = rd64(f + 32);
    int phnum = rd16(f + 56), phentsize = rd16(f + 54);
    uint64_t minv = ~0ull, maxv = 0;
    for (int i = 0; i < phnum; i++) {
        uint8_t *ph = f + phoff + (uint64_t)i * phentsize;
        // PT_LOAD
        if (rd32(ph) != 1) continue;
        uint64_t v = rd64(ph + 16), msz = rd64(ph + 40);
        if (v < minv) minv = v;
        if (v + msz > maxv) maxv = v + msz;
    }
    uint64_t basepage = minv & ~0xFFFull;
    uint64_t span = (maxv - basepage + 0xFFFF) & ~0xFFFFull;
    int etype = rd16(f + 16);
    // NULL: non-colliding (main + interp). A non-PIE ET_EXEC gets biased here; the dispatcher redirects its
    // absolute code jumps (g_nonpie_*) and the nonpie_guard SIGSEGV handler re-serves its absolute DATA refs
    // to the low link vaddr at +bias (see nonpie_fixup above).
    uint8_t *base = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) {
        perror("mmap base");
        exit(1);
    }
    gmap_add((uint64_t)base, span); // track so execve() can reclaim the inherited image
    uint64_t bias = (uint64_t)base - basepage;
    if (etype == 2) {
        g_nonpie_lo = basepage;
        g_nonpie_hi = basepage + span;
        g_nonpie_bias = bias;
    }
    for (int i = 0; i < phnum; i++) {
        uint8_t *ph = f + phoff + (uint64_t)i * phentsize;
        if (rd32(ph) != 1) continue;
        uint64_t off = rd64(ph + 8), v = rd64(ph + 16), fsz = rd64(ph + 32);
        memcpy((void *)(v + bias), f + off, fsz);
    }
    // per-segment W^X from p_flags: .text R+X, .rodata R, .data R+W
    for (int i = 0; i < phnum; i++) {
        uint8_t *ph = f + phoff + (uint64_t)i * phentsize;
        if (rd32(ph) != 1) continue;
        // PF_X=1, PF_W=2, PF_R=4
        uint32_t fl = rd32(ph + 4);
        uint64_t v = rd64(ph + 16), msz = rd64(ph + 40);
        uint64_t s = (v + bias) & ~0xFFFull, e = (v + bias + msz + 0xFFFull) & ~0xFFFull;
        int prot = PROT_READ | ((fl & 2) ? PROT_WRITE : 0) | ((fl & 1) ? PROT_EXEC : 0);
        if (e > s) mprotect((void *)s, e - s, prot);
    }
    out->entry = e_entry + bias;
    out->base = (uint64_t)base;
    if (getenv("JT"))
        fprintf(stderr, "[LOADED] %s base=%llx entry=%llx\n", path, (unsigned long long)base,
                (unsigned long long)out->entry);
    // phdrs live at file offset phoff in seg 0
    out->phdr = (uint64_t)base + phoff;
    out->phent = phentsize;
    out->phnum = phnum;
    munmap(f, st.st_size);
    close(fd);
}

// Build the Linux process stack: [argc][argv..][NULL][envp..][NULL][auxv..][AT_NULL].
extern char **environ;
static char *g_guest_env[] = {
    "PATH=/usr/bin:/bin", "HOME=/root", "TERM=dumb", "LANG=C", "GLIBC_TUNABLES=glibc.cpu.aarch64_gcs=0", NULL,
};
static uint64_t build_stack(int argc, char **argv, struct loaded *lm, uint64_t at_base) {
    size_t SZ = 8u << 20;
    uint8_t *stk = mmap(NULL, SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    gmap_add((uint64_t)stk, SZ); // track so execve() can reclaim the inherited stack
    uint8_t *top = stk + SZ;
    uint64_t argp[256], envp_[256];
    int envc = 0;
    for (int i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]) + 1;
        top -= l;
        memcpy(top, argv[i], l);
        argp[i] = (uint64_t)top;
    }
    // the container's env arrives as DD_GUEST_ENV="K=V\nK=V\n…" (set by the daemon) -- forward EXACTLY
    // these to the guest FIRST so they override the defaults, NOT the daemon/host environment. Then the
    // built-in defaults fill ONLY the keys the container didn't set.
    char *ge = getenv("DD_GUEST_ENV");
    if (ge) {
        char *copy = strdup(ge), *save = NULL;
        for (char *ln = strtok_r(copy, "\n", &save); ln && envc < 250; ln = strtok_r(NULL, "\n", &save)) {
            size_t l = strlen(ln) + 1;
            top -= l;
            memcpy(top, ln, l);
            envp_[envc++] = (uint64_t)top;
        }
        free(copy);
    }
    int guest_envc = envc; // [0..guest_envc) came from the container; the rest are defaults
    for (int i = 0; g_guest_env[i] && envc < 255; i++) {
        // Skip a default whose KEY the container already set: a duplicate "PATH=" would otherwise appear
        // in envp, and shells (bash) honor the LAST occurrence -> the default would shadow the image PATH
        // (this is what made `gosu` unresolvable in the postgres entrypoint). Match on the "KEY=" prefix.
        const char *eq = strchr(g_guest_env[i], '=');
        size_t klen = eq ? (size_t)(eq - g_guest_env[i]) + 1 : 0;
        int dup = 0;
        for (int j = 0; j < guest_envc && klen; j++)
            if (strncmp((char *)envp_[j], g_guest_env[i], klen) == 0) {
                dup = 1;
                break;
            }
        if (dup) continue;
        size_t l = strlen(g_guest_env[i]) + 1;
        top -= l;
        memcpy(top, g_guest_env[i], l);
        envp_[envc++] = (uint64_t)top;
    }
    top -= 8;
    memcpy(top, "aarch64", 8);
    uint64_t plat = (uint64_t)top;
    top -= 16;
    arc4random_buf(top, 16);
    uint64_t rnd = (uint64_t)top;
    top = (uint8_t *)((uint64_t)top & ~15ull);
    uint64_t aux[][2] = {
        {3, lm->phdr},
        {4, (uint64_t)lm->phent},
        {5, (uint64_t)lm->phnum},
        {6, 4096},
        {7, at_base},
        {8, 0},
        {9, lm->entry},
        {11, (uint64_t)cuid()},
        {12, (uint64_t)cuid()},
        {13, (uint64_t)cgid()},
        {14, (uint64_t)cgid()},
        {16, 0x1fb},
        {17, 100},
        {15, plat},
        {25, rnd},
        {23, 0},
        {31, argc ? argp[0] : 0},
        {0, 0},
    };
    int naux = (int)(sizeof aux / sizeof aux[0]);
    size_t nslots = 1 + (argc + 1) + (envc + 1) + (size_t)naux * 2;
    uint64_t *sp = (uint64_t *)top - nslots;
    sp = (uint64_t *)((uint64_t)sp & ~15ull);
    uint64_t *p = sp;
    *p++ = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        *p++ = argp[i];
    *p++ = 0;
    for (int i = 0; i < envc; i++)
        *p++ = envp_[i];
    *p++ = 0;
    for (int i = 0; i < naux; i++) {
        *p++ = aux[i][0];
        *p++ = aux[i][1];
    }
    // also serialize for /proc/self/auxv
    g_auxv_len = 0;
    for (int i = 0; i < naux && g_auxv_len + 16 <= (int)sizeof g_auxv_data; i++) {
        memcpy(g_auxv_data + g_auxv_len, &aux[i][0], 8);
        memcpy(g_auxv_data + g_auxv_len + 8, &aux[i][1], 8);
        g_auxv_len += 16;
    }
    return (uint64_t)sp;
}
