// dd/runtime/frontend/x86_64 -- ELF loader (load PT_LOAD high; static-PIE + dynamic via ld.so) + stack.

// ---------------- minimal ELF loader (load high; copied from jit.c) ----------------
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

// struct loaded is defined by the shared os/linux (container/netns.c).

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
    if (rd16(f + 18) != 0x3E) fprintf(stderr, "[jit86] warning: e_machine=%u (want 62=x86-64)\n", rd16(f + 18));
    uint64_t e_entry = rd64(f + 24), phoff = rd64(f + 32);
    int phnum = rd16(f + 56), phentsize = rd16(f + 54);
    uint64_t minv = ~0ull, maxv = 0;
    for (int i = 0; i < phnum; i++) {
        uint8_t *ph = f + phoff + (uint64_t)i * phentsize;
        if (rd32(ph) != 1) continue;
        uint64_t v = rd64(ph + 16), msz = rd64(ph + 40);
        if (v < minv) minv = v;
        if (v + msz > maxv) maxv = v + msz;
    }
    uint64_t basepage = minv & ~0xFFFull;
    uint64_t span = (maxv - basepage + 0xFFFF) & ~0xFFFFull;
    // opt8: the persistent cache needs deterministic guest bases across runs so the translated bytes
    // (RIP-relative leas, baked branch targets, block-map keys) are byte-identical. When g_force_base is
    // set, map MAP_FIXED at the caller-requested address; the image is PIE so basepage is ~0 and the chosen
    // base becomes out->base, deriving all guest PCs/addresses identically each run. One-shot per image.
    uint8_t *base;
    if (g_force_base) {
        base = mmap((void *)(g_force_base + basepage), span, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        g_force_base = 0;
    } else {
        base = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    }
    if (base == MAP_FAILED) {
        perror("mmap base");
        exit(1);
    }
    uint64_t bias = (uint64_t)base - basepage;
    for (int i = 0; i < phnum; i++) {
        uint8_t *ph = f + phoff + (uint64_t)i * phentsize;
        if (rd32(ph) != 1) continue;
        uint64_t off = rd64(ph + 8), v = rd64(ph + 16), fsz = rd64(ph + 32);
        memcpy((void *)(v + bias), f + off, fsz);
    }
    mprotect(base, span, PROT_READ | PROT_WRITE | PROT_EXEC);
    out->entry = e_entry + bias;
    out->base = (uint64_t)base;
    out->phdr = (uint64_t)base + phoff;
    out->phent = phentsize;
    out->phnum = phnum;
    extern int g_diag;
    if (g_trace || g_diag || getenv("JT"))
        fprintf(stderr, "[LOADED] %s base=%llx span=%llx end=%llx entry=%llx\n", path, (unsigned long long)base,
                (unsigned long long)span, (unsigned long long)((uint64_t)base + span), (unsigned long long)out->entry);
    munmap(f, st.st_size);
    close(fd);
}

// Build the SysV x86-64 process stack (identical layout to aarch64). Returns rsp.
static char *g_guest_env[] = {"PATH=/usr/bin:/bin", "HOME=/root", "TERM=dumb", "LANG=C", NULL};
static uint64_t build_stack(int argc, char **argv, struct loaded *lm, uint64_t at_base) {
    size_t SZ = 8u << 20, GUARD = 0x10000;
    // GUARD bytes are mapped ABOVE the logical top: the topmost stack objects are the
    // AT_PLATFORM "x86_64" string and the 16 AT_RANDOM bytes, which glibc strlen/reads
    // with 16-byte SSE loads -> those over-read past the top. Keep that region mapped.
    uint8_t *stk = mmap(NULL, SZ + GUARD, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint8_t *top = stk + SZ;
    uint64_t argp[256], envp_[256];
    int envc = 0;
    for (int i = 0; i < argc; i++) {
        size_t l = strlen(argv[i]) + 1;
        top -= l;
        memcpy(top, argv[i], l);
        argp[i] = (uint64_t)top;
    }
    while (g_guest_env[envc])
        envc++;
    for (int i = 0; i < envc; i++) {
        size_t l = strlen(g_guest_env[i]) + 1;
        top -= l;
        memcpy(top, g_guest_env[i], l);
        envp_[i] = (uint64_t)top;
    }
    top -= 8;
    memcpy(top, "x86_64", 7);
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
        {11, 0},
        {12, 0},
        {13, 0}, // AT_UID/EUID/GID -> container root
        {14, 0},
        {16, 0},
        {15, plat},
        {25, rnd},
        {23, 0},
        {0, 0}, // AT_EGID 0; AT_SECURE 0
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
    extern int g_diag;
    if (g_diag)
        fprintf(stderr, "[stack] base=%p top=%p guard_end=%p sp=%p plat=%llx rnd=%llx\n", (void *)stk, (void *)top,
                (void *)(stk + SZ + GUARD), (void *)sp, (unsigned long long)plat, (unsigned long long)rnd);
    return (uint64_t)sp;
}

// debug fault handler (only installed under TRACE_ON): print faulting address + guest cpu.
// Lazy-guard fault handler (default): glibc's vectorized string ops (strlen/memchr/
// memcmp) issue 16-byte SSE loads that legitimately over-read past a buffer's end into
// the adjacent page. On Darwin an unmapped page -> SIGBUS. We map the faulting page as
// zero and retry: the zero terminator makes strlen/memchr return the correct result, and
// vectorized loads mask out the bytes past the real end. Bounded so genuine wild
// accesses (a real bug) still abort once the budget is spent.
static _Atomic int g_lazymaps;
void jit86_lazyguard(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    void *a = si ? si->si_addr : NULL;
    if (a && g_lazymaps < 4096) {
        uintptr_t pg = (uintptr_t)a & ~(uintptr_t)0xFFF;
        // macOS won't MAP_FIXED over a sub-range of an existing VM entry (EINVAL); try
        // mprotect first (the page often exists as a PROT_NONE guard), then a fresh map.
        if (mprotect((void *)pg, 0x1000, PROT_READ | PROT_WRITE) == 0) {
            g_lazymaps++;
            return;
        }
        void *r = mmap((void *)pg, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (r != MAP_FAILED) {
            g_lazymaps++;
            return;
        } // retry the faulting instruction
    }
    signal(sig, SIG_DFL);
    raise(sig); // out of budget / mmap failed -> real crash
}
void jit86_faulth(int sig, siginfo_t *si, void *uc) {
    struct cpu *c = (struct cpu *)pthread_getspecific(g_cpu_key);
    static const char *nm[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                 "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
    extern uint64_t g_prevpc, g_curpc;
    fprintf(stderr, "[FAULT] sig=%d addr=%p  guest rip(last blk)=%llx  curpc=%llx prevblk=%llx ibranch_src=%llx\n", sig,
            si ? si->si_addr : 0, c ? (unsigned long long)c->rip : 0, (unsigned long long)g_curpc,
            (unsigned long long)g_prevpc, c ? (unsigned long long)c->dbg_ibsrc : 0);
    if (c)
        for (int i = 0; i < 16; i++)
            fprintf(stderr, "  %s=%llx%s", nm[i], (unsigned long long)c->r[i], (i % 4 == 3) ? "\n" : "");
    if (c && c->rip) {
        fprintf(stderr, "  bytes@rip:");
        uint8_t *p = (uint8_t *)c->rip;
        for (int i = 0; i < 24; i++)
            fprintf(stderr, " %02x", p[i]);
        fprintf(stderr, "\n");
    }
    if (c) {
        uint64_t pp = c->r[7];
        if (pp > 0x100000000ull && pp < 0x200000000ull) { // rdi: dump chunk header [p-16..p+8)
            fprintf(stderr, "  hdr[rdi-16..p+8):");
            uint8_t *b = (uint8_t *)(pp - 16);
            for (int i = 0; i < 24; i++)
                fprintf(stderr, " %02x", b[i]);
            fprintf(stderr, "  (p-8 u32=%x p-4 u8=%x p-2 u16=%x)\n", *(uint32_t *)(pp - 8), *(uint8_t *)(pp - 4),
                    *(uint16_t *)(pp - 2));
            fprintf(stderr, "  scan-back for group->meta (qword at p-16*off-16):");
            for (int off = 0; off <= 32; off++) {
                uint64_t bv = *(uint64_t *)(pp - 16 * off - 16);
                if (bv > 0x100000000ull && bv < 0x200000000ull)
                    fprintf(stderr, " off=%d->%llx", off, (unsigned long long)bv);
            }
            fprintf(stderr, "\n");
        }
    }
    if (c)
        for (int rr = 0; rr < 16; rr++) { // dump memory at any reg that looks like a heap pointer
            uint64_t v = c->r[rr];
            if (v > 0x100000000ull && v < 0x200000000ull && (v & 7) == 0) {
                fprintf(stderr, "  mem[%d=%llx]:", rr, (unsigned long long)v);
                for (int i = 0; i < 6; i++)
                    fprintf(stderr, " %016llx", (unsigned long long)((uint64_t *)v)[i]);
                fprintf(stderr, "\n");
                if (rr >= 3) break; // a couple is enough
            }
        }
    _exit(133);
}
