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
    // NULL: non-colliding (main + interp)
    uint8_t *base = mmap(NULL, span, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
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
    if (envc > 255) envc = 255;
    for (int i = 0; i < envc; i++) {
        size_t l = strlen(g_guest_env[i]) + 1;
        top -= l;
        memcpy(top, g_guest_env[i], l);
        envp_[i] = (uint64_t)top;
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
        {6, 16384},
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
