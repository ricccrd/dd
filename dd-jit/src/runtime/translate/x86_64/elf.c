// dd/runtime/frontend/x86_64 -- ELF loader (load PT_LOAD high; static-PIE + dynamic via ld.so) + stack.

// W6A: the x86 target (linux_x86_64.c) does not pull in the mach VM headers (only the aarch64 target
// does, for item 2's dual map). The lazy-fault budget classifier (item 4) queries the real VM map via
// mach_vm_region, and the non-PIE data fixup (item 1) reads the arm64 thread state out of the ucontext.
// Include guards make these idempotent if the unity build ever includes them earlier.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/ucontext.h>

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
static void wr64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

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
// W6A item 1 (Go non-PIE): a Go ET_EXEC's runtime keeps its OWN code/data addresses in the
// firstmoduledata struct (and the pclntab it points at) as LINK-TIME absolute values: text/etext,
// minpc/maxpc, the pclntab slice pointers, findfunctab, the type/gc/gofunc bases, the init-task
// list, etc. After we bias the image HIGH (macOS reserves the low 4GB), the runtime's live code PCs
// -- return addresses pushed by `call`, function pointers materialized by rip-relative `lea` -- are
// all BIASED, but findfunc() still compares them against the un-biased moduledata: findmoduledatap()
// rejects the pc (pc >= maxpc), findfunc() returns a nil funcInfo, and runtime.pcdatavalue derefs
// it -> SIGSEGV at offset 0x1c. Fix: at load time, add g_nonpie_bias to every ABSOLUTE pointer word
// of firstmoduledata so the comparisons line up with the biased PCs. The pclntab's own tables are
// pc-DELTAS and text-RELATIVE offsets (and pcHeader.textStart no longer exists in Go 1.20+, it is a
// reserved zero), so only the moduledata pointer words and the textsect baseaddr are rebased; slice
// len/cap, relative offsets and flags are left untouched. Each rebase is guarded to the link range
// [lo,hi) so a zero/small/already-mapped field is never disturbed. The module is located via the
// runtime.firstmoduledata symbol and validated by the pclntab magic -> a no-op for a stripped image
// or a non-Go ET_EXEC. Layout per Go runtime/symtab.go (verified against the go1.26 nats-server).
// Look up a symbol's st_value in the ELF .symtab (+ its linked string table). Returns 0 if absent.
static uint64_t go_symval(const uint8_t *f, size_t fsz, const char *name) {
    uint64_t shoff = rd64(f + 0x28);
    int shnum = rd16(f + 0x3C), shent = rd16(f + 0x3A);
    if (!shoff || !shent || (uint64_t)shoff + (uint64_t)shnum * shent > fsz) return 0;
    for (int i = 0; i < shnum; i++) {
        const uint8_t *sh = f + shoff + (uint64_t)i * shent;
        if (rd32(sh + 4) != 2) continue; // SHT_SYMTAB
        uint64_t symoff = rd64(sh + 0x18), symsz = rd64(sh + 0x20), syment = rd64(sh + 0x38);
        uint32_t strndx = rd32(sh + 0x28); // sh_link -> string table section
        if (!syment || strndx >= (uint32_t)shnum) continue;
        const uint8_t *strsh = f + shoff + (uint64_t)strndx * shent;
        uint64_t stroff = rd64(strsh + 0x18), strsz = rd64(strsh + 0x20);
        if (symoff + symsz > fsz || stroff + strsz > fsz) continue;
        for (uint64_t o = 0; o + syment <= symsz; o += syment) {
            const uint8_t *sym = f + symoff + o;
            uint32_t nameoff = rd32(sym);
            if (nameoff < strsz && strcmp((const char *)(f + stroff + nameoff), name) == 0) return rd64(sym + 8);
        }
    }
    return 0;
}

static void go_rebase_nonpie(const uint8_t *f, size_t fsz, uint64_t bias, uint64_t lo, uint64_t hi) {
    uint64_t md_va = go_symval(f, fsz, "runtime.firstmoduledata");
    if (!md_va || md_va < lo || md_va >= hi) return;
    uint8_t *md = (uint8_t *)(md_va + bias); // the mapped (biased) copy of firstmoduledata
    // Validate: field 0 is &pclntab, whose first u32 is the Go pclntab magic. Bail if not Go.
    uint64_t pch = rd64(md);
    if (pch < lo || pch >= hi) return;
    uint32_t magic = rd32((const uint8_t *)(pch + bias));
    if (magic != 0xfffffff0u && magic != 0xfffffff1u && magic != 0xfffffffau && magic != 0xfffffffbu) return;
    // Absolute pointer words of moduledata, in 8-byte units (Go 1.26 runtime/symtab.go). Slice headers
    // contribute only their .ptr word; len/cap follow and are skipped. The guard below also skips any
    // word that is zero or outside the link range, so unused fields cost nothing.
    // Only the words that name CODE PCs / live read bases / GC segment bounds are rebased high: minpc/maxpc
    // and text are compared against the high return-address PCs; the pcln tables, findfunctab, data/bss
    // bounds and gc masks are dereferenced as mapped memory by findfunc/the GC. The type-system bases
    // (types,etypes,rodata,gofunc -- words 37..40) are deliberately LEFT LOW: type/data pointers materialized
    // by rip-relative lea are rewritten low (translate.c) to match the image's baked-absolute low pointers,
    // so findmoduledatap's `types <= p < etypes` range check and resolveTypeOff must use the low bases too
    // (rebasing them high made Go's type identity -- e.g. runtime.SetFinalizer's `fint == etyp` -- diverge).
    // Any low type/data access is served by nonpie_fixup at +bias.
    static const int ptr_words[] = {
        0,                                  // pcHeader
        1, 4, 7, 10, 13, 16,                // funcnametab/cutab/filetab/pctab/pclntable/ftab slice ptrs
        19, 20, 21,                         // findfunctab, minpc, maxpc
        22, 23, 24, 25, 26, 27, 28, 29,     // text,etext,noptrdata,enoptrdata,data,edata,bss,ebss
        30, 31, 32, 33, 34, 35, 36,         // noptrbss,enoptrbss,covctrs,ecovctrs,end,gcdata,gcbss
        41,                                 // epclntab (types,etypes,rodata,gofunc = 37..40 stay low)
        42, 45, 48, 51,                     // textsectmap,typelinks,itablinks,ptab slice ptrs
        54, 56, 59, 62, 64,                 // pluginpath,pkghashes,inittasks,modulename,modulehashes ptrs
        69, 71, 72, 73,                     // gcdatamask.bytedata, gcbssmask.bytedata, typemap, next
    };
    for (size_t k = 0; k < sizeof ptr_words / sizeof *ptr_words; k++) {
        uint8_t *slot = md + (size_t)ptr_words[k] * 8;
        uint64_t cur = rd64(slot);
        if (cur >= lo && cur < hi) wr64(slot, cur + bias);
    }
    // textsectmap is []textsect{vaddr, end, baseaddr}; only baseaddr is an absolute (relocated) address
    // -- vaddr/end are text-relative -- so rebase each entry's baseaddr explicitly. (With a single text
    // section the runtime ignores baseaddr, but keep it consistent for the multi-section case.)
    uint64_t ts_ptr = rd64(md + 42 * 8), ts_len = rd64(md + 43 * 8); // ts_ptr already rebased above
    if (ts_ptr >= lo + bias && ts_ptr < hi + bias) {
        for (uint64_t i = 0; i < ts_len && i < 64; i++) {
            uint8_t *ba = (uint8_t *)(ts_ptr + i * 24 + 16);
            uint64_t cur = rd64(ba);
            if (cur >= lo && cur < hi) wr64(ba, cur + bias);
        }
    }
    // W6A item 1: runtime.lastmoduledatap is a global *moduledata holding the baked-absolute (low) address
    // of firstmoduledata. The runtime compares it against &firstmoduledata taken by a rip-relative lea --
    // which materializes the HIGH mapped address (firstmoduledata is in the writable data segment, outside
    // the type section, so its lea is NOT rewritten low). A low lastmoduledatap vs high &firstmoduledata
    // makes runtime.main's `for md := &firstmoduledata; ...; md = md.next` loop overrun the single module
    // (md never equals lastmoduledatap) and dereference md.next == nil -> SIGSEGV. Rebase the pointer the
    // global holds to its high mapping so the identity holds. (modulesSlice entries stay low: they are only
    // DEREFERENCED -- served by nonpie_fixup -- never compared against a lea.)
    uint64_t lmdp_va = go_symval(f, fsz, "runtime.lastmoduledatap");
    if (lmdp_va >= lo && lmdp_va < hi) {
        uint8_t *slot = (uint8_t *)(lmdp_va + bias);
        uint64_t cur = rd64(slot);
        if (cur >= lo && cur < hi) wr64(slot, cur + bias);
    }
    // W6A item 1: publish the (left-low) type section [types, etypes) -- moduledata words 37,38, which are
    // deliberately NOT rebased above. translate.c rewrites a rip-relative lea whose target lands here to the
    // low link address so lea-built *_type pointers match the image's baked-absolute (low) type pointers and
    // Go's type identity holds. Only set when both bounds are sane low link addresses.
    uint64_t tlo = rd64(md + 37 * 8), thi = rd64(md + 38 * 8);
    if (tlo >= lo && thi <= hi && tlo < thi) {
        g_nonpie_types_lo = tlo;
        g_nonpie_types_hi = thi;
    }
    extern int g_diag;
    if (g_trace || g_diag || getenv("JT"))
        fprintf(stderr, "[go-rebase] firstmoduledata@%llx +bias=%llx (magic=%x) types=[%llx,%llx)\n",
                (unsigned long long)md_va, (unsigned long long)bias, magic, (unsigned long long)g_nonpie_types_lo,
                (unsigned long long)g_nonpie_types_hi);
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
    if (rd16(f + 18) != 0x3E) fprintf(stderr, "[dd] warning: e_machine=%u (want 62=x86-64)\n", rd16(f + 18));
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
    // W6A item 1: a non-PIE ET_EXEC (etype==2) links at a fixed low vaddr (e.g. 0x400000) but macOS
    // __PAGEZERO reserves the low 4GB, so the loader (above) biases it to a high mmap. Its un-relocated
    // ABSOLUTE refs then point at the original low vaddr (unmapped/__PAGEZERO) and fault. Record the
    // original link range + bias so the dispatcher can redirect absolute CODE jumps and the SIGSEGV
    // handler (nonpie_fixup) can serve absolute DATA loads/stores at +bias. PIE/static-PIE leave these
    // 0 -> no redirect, no fixup, byte-identical. Coexists with the opt8 g_force_base path above (that
    // only fires for PIE images under DDJIT_PCACHE; a non-PIE ET_EXEC takes the NULL-hint branch).
    // NONPIE_NOFIXUP=1 disables (legacy: code jump still faults on the low vaddr). g_nonpie_* live in the
    // shared os/linux/container/vfs.c; service.c resets them across execve (case 221) for re-loaded images.
    int etype = rd16(f + 16);
    if (etype == 2 && !getenv("NONPIE_NOFIXUP")) {
        g_nonpie_lo = basepage;
        g_nonpie_hi = basepage + span;
        g_nonpie_bias = bias;
        g_nonpie_types_lo = g_nonpie_types_hi = 0; // set by go_rebase_nonpie iff this is a Go image
    }
    for (int i = 0; i < phnum; i++) {
        uint8_t *ph = f + phoff + (uint64_t)i * phentsize;
        if (rd32(ph) != 1) continue;
        uint64_t off = rd64(ph + 8), v = rd64(ph + 16), fsz = rd64(ph + 32);
        memcpy((void *)(v + bias), f + off, fsz);
    }
    // W6A item 1: for a biased non-PIE Go image, rebase firstmoduledata so the runtime's findfunc()
    // resolves the biased code PCs (otherwise runtime.pcdatavalue nil-derefs). Gated on g_nonpie_lo
    // (ET_EXEC only); NOGOREBASE=1 disables for A/B testing.
    if (g_nonpie_lo && !getenv("NOGOREBASE")) go_rebase_nonpie(f, st.st_size, bias, g_nonpie_lo, g_nonpie_hi);
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
    extern uint64_t g_stack_lo, g_stack_hi; // publish for /proc/self/maps [stack] synthesis (vfs.c)
    g_stack_lo = (uint64_t)stk;
    g_stack_hi = (uint64_t)(stk + SZ + GUARD);
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
        {23, 0},     // AT_SECURE 0
        {17, 100},   // AT_CLKTCK
        {26, 0},     // AT_HWCAP2
        {31, argp[0]}, // AT_EXECFN -> argv[0] path string. Rust std / uutils' multicall read this to pick the
                       // applet name; missing it made getauxval(AT_EXECFN)==0 -> strlen(0) -> SIGSEGV.
        {0, 0},      // AT_NULL terminator
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
    // Serialize the same auxv for /proc/self/auxv (read by Rust std / hwcap crates; the x86 path previously
    // left it empty -> a 0-length auxv that those readers mis-parse). g_auxv_data/_len live in vfs.c (same TU).
    g_auxv_len = 0;
    for (int i = 0; i < naux && g_auxv_len + 16 <= (int)sizeof g_auxv_data; i++) {
        memcpy(g_auxv_data + g_auxv_len, &aux[i][0], 8);
        memcpy(g_auxv_data + g_auxv_len + 8, &aux[i][1], 8);
        g_auxv_len += 16;
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
static _Atomic int g_lazymaps; // isolated/wild faults (small bounded budget)
static _Atomic int g_growmaps; // adjacent (stack-grow / over-read) faults: large bounded budget
// W6A item 4: the original cap was a single global, monotonic, never-reset budget of 4096 pages shared
// by BOTH legitimate growth (stack-down, SSE over-reads adjacent to a real allocation) AND genuine wild
// pointers. A long-running / large-working-set guest that legitimately faults >4096 DISTINCT guard pages
// exhausts it and the next legitimate fault is re-raised as a fatal SIGSEGV (exit 139). Fix: classify the
// fault by adjacency to an existing mapping. A fault page whose immediate neighbor (above OR below) is
// already mapped is provably legitimate (stack growth is one page below the committed stack; an SSE
// over-read is one page past a real buffer) -> map it against a large grow budget. A fault with NO mapped
// neighbor is an isolated wild pointer -> the small bounded budget still catches it and aborts (safety
// net PRESERVED). Page contents + retry are unchanged, so this is bit-identical for any workload the old
// code completed. Gate: NOLAZYFIX=1 reverts to the single 4096 monotonic budget (everything on g_lazymaps);
// LAZYBUDGET=<n> overrides the small cap (repro/testing); LAZYDIAG=1 prints final counts at exit.
static int lazy_addr_mapped(uintptr_t a) {
    // mincore() is useless here -- on macOS it returns 0 for ANY address, mapped or not. Query the real
    // VM map: mach_vm_region returns the first region at-or-above `a`; `a` is mapped iff it falls inside
    // that region's [start,start+size).
    mach_vm_address_t addr = a;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    kern_return_t kr =
        mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj);
    if (kr != KERN_SUCCESS) return 0; // nothing at/above -> unmapped
    return a >= (uintptr_t)addr && a < (uintptr_t)addr + (uintptr_t)size;
}
static int lazy_neighbor_mapped(uintptr_t pg) {
    // A fault adjacent to a live mapping is legitimate growth/over-read: the byte just below the fault
    // page is the end of a real region (over-read), or the page just above is the committed stack
    // (grow-down). An isolated fault (both neighbors unmapped) is a candidate wild pointer. Use the 16KB
    // macOS hardware page granularity for the "above" probe.
    if (pg >= 1 && lazy_addr_mapped(pg - 1)) return 1;
    if (lazy_addr_mapped(pg + 0x4000)) return 1;
    return 0;
}
static int lazy_budget(void) {
    static int b = -1;
    if (b < 0) {
        const char *e = getenv("LAZYBUDGET");
        b = (e && *e) ? atoi(e) : 4096;
    }
    return b;
}
static int lazy_nofix(void) {
    static int v = -1;
    if (v < 0) v = getenv("NOLAZYFIX") ? 1 : 0;
    return v;
}
static void lazy_diag(void) {
    if (getenv("LAZYDIAG"))
        fprintf(stderr, "[lazy] grow=%d wild=%d (budget=%d nofix=%d)\n", (int)g_growmaps, (int)g_lazymaps,
                lazy_budget(), lazy_nofix());
}
// W6A item 1: emulate a faulting host load/store against the biased non-PIE image. A non-PIE guest's
// absolute ref resolves to the original low link vaddr (in [g_nonpie_lo,g_nonpie_hi)); the real data
// lives at that vaddr + g_nonpie_bias. We decode the faulting emitted arm64 access, perform it at +bias,
// and skip the instruction. Every guest memory access is emitted with the effective address pre-folded
// into x17 (off=0) -> si_addr is the access base. Three families are served:
//   * INTEGER ld/st-register (scaled uimm + unscaled ldur/stur, signed/unsigned, b/h/w/x),
//   * SIMD&FP ld/st-register Q/D/S/H/B (the SSE constant/spill paths emit `ldr q`/`str q` etc. against
//     low .rodata/.data) -- moved through the ucontext NEON state (__ns.__v[t]); FP loads zero the upper
//     lanes, matching arm64,
//   * LSE atomic RMW (ldadd/ldclr/ldeor/ldset/swp) + compare-and-swap (cas) -- the x86 LOCK path emits
//     these against the absolute EA; performed ATOMICALLY at +bias, old value written back to the reg.
// Returns 1 if handled. Anything it can't decode safely (e.g. an LSE signed/unsigned min/max subform the
// x86 backend never emits) returns 0 -> the normal handler re-raises = a clean abort, never silent wrong
// data. Gated on g_nonpie_lo (set only for ET_EXEC) -> PIE/static-PIE never enter here. OUT OF SCOPE
// (documented residual, a separate broad g2h change): syscall POINTER args that point into the low
// non-PIE image are read 1:1 in service.c and are NOT redirected here.

// Atomic RMW helpers (truly atomic, width-typed) used by the LSE/CAS fixup paths below.
static int nonpie_lse_rmw(void *p, int size, int opc, uint64_t v, uint64_t *old) {
    // opc: 0=ADD 1=CLR(&~) 2=EOR 3=SET(|). Returns 1 if handled, 0 for an unsupported subform.
    switch (size) {
#define NP_RMW(TY)                                                                                             \
    {                                                                                                          \
        TY *a = (TY *)p, ov = (TY)v, o;                                                                        \
        switch (opc) {                                                                                         \
        case 0: o = __atomic_fetch_add(a, ov, __ATOMIC_SEQ_CST); break;                                       \
        case 1: o = __atomic_fetch_and(a, (TY)~ov, __ATOMIC_SEQ_CST); break;                                  \
        case 2: o = __atomic_fetch_xor(a, ov, __ATOMIC_SEQ_CST); break;                                       \
        case 3: o = __atomic_fetch_or(a, ov, __ATOMIC_SEQ_CST); break;                                        \
        default: return 0;                                                                                    \
        }                                                                                                     \
        *old = (uint64_t)o;                                                                                   \
        return 1;                                                                                             \
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
    case 0: { uint8_t e = (uint8_t)expected; __atomic_compare_exchange_n((uint8_t *)p, &e, (uint8_t)newv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    case 1: { uint16_t e = (uint16_t)expected; __atomic_compare_exchange_n((uint16_t *)p, &e, (uint16_t)newv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    case 2: { uint32_t e = (uint32_t)expected; __atomic_compare_exchange_n((uint32_t *)p, &e, (uint32_t)newv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    default: { uint64_t e = expected; __atomic_compare_exchange_n((uint64_t *)p, &e, newv, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
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
    uint64_t real = va + g_nonpie_bias;      // the actual mapped location of the datum
    uint64_t *X = uc->uc_mcontext->__ss.__x; // __x[0..28]; 29=fp 30=lr 31=zr/sp
    int v = (insn >> 26) & 1;                // SIMD&FP?
    int rt = insn & 0x1F;
    // Load/store-register IMMEDIATE family: bits[29:27]==111, and either the scaled unsigned-imm form
    // (bit24==1) or the unscaled ldur/stur form (bit24==0 && bit21==0 && bits[11:10]==00). This EXCLUDES
    // the register-offset form (bit21==1) and the LSE atomics (bit21==1, decoded separately below) -- both
    // also have bits[29:27]==111, but the backend never emits a register-offset access for a guest EA.
    int fam = ((insn >> 27) & 7) == 7;
    int scaled = (insn >> 24) & 1;
    int ls_imm = fam && (scaled || (!((insn >> 21) & 1) && !((insn >> 10) & 3)));

    // ---- SIMD&FP Q/D/S/H/B load/store (V==1). width = 1<<((opc[1]<<2)|size): B1 H2 S4 D8 Q16. ----
    if (v && ls_imm) {
        int size = insn >> 30, opc = (insn >> 22) & 3;
        int bytes = 1 << (((opc >> 1) << 2) | size);
        if (opc & 1) { // load -> write Vt, zeroing the upper lanes (arm64 FP-load semantics)
            __uint128_t z = 0;
            memcpy(&z, (void *)real, (size_t)bytes);
            uc->uc_mcontext->__ns.__v[rt] = z;
        } else { // store -> low `bytes` of Vt to memory
            __uint128_t s = uc->uc_mcontext->__ns.__v[rt];
            memcpy((void *)real, &s, (size_t)bytes);
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
            return 0; // signed/unsigned min/max (never emitted by the x86 backend) -> clean abort
        }
        if (rt != 31) X[rt] = nonpie_zext(old, size); // Rt receives the old value
        uc->uc_mcontext->__ss.__pc += 4;
        return 1;
    }

    // ---- CAS/CASAL: size[31:30] 001000 1 1 1 Rs[20:16] o0 11111 Rn[9:5] Rt[4:0]. Rs=cmp in/old out. ----
    if ((insn & 0x3FE0FC00u) == 0x08E0FC00u) {
        int size = insn >> 30, rs = (insn >> 16) & 0x1F;
        uint64_t expected = (rs == 31) ? 0 : X[rs], newv = (rt == 31) ? 0 : X[rt];
        uint64_t old = nonpie_cas((void *)real, size, expected, newv);
        if (rs != 31) X[rs] = nonpie_zext(old, size); // Rs receives the old value
        uc->uc_mcontext->__ss.__pc += 4;
        return 1;
    }

    // ---- INTEGER load/store-register (scaled + unscaled, signed/unsigned, b/h/w/x) ----
    if (!(ls_imm && !v)) return 0; // not a form we decode -> clean abort (the handler re-raises)
    int size = insn >> 30;         // 0=B 1=H 2=W 3=X
    int opc = (insn >> 22) & 3;    // 01=load(zext) 00=store 10=load-sext(64) 11=ldrsw
    uint64_t val;
    if (opc == 0) { // store: write rt's low `size` bytes
        val = (rt == 31) ? 0 : X[rt];
        switch (size) {
        case 0: *(uint8_t *)real = (uint8_t)val; break;
        case 1: *(uint16_t *)real = (uint16_t)val; break;
        case 2: *(uint32_t *)real = (uint32_t)val; break;
        default: *(uint64_t *)real = val; break;
        }
    } else { // load
        switch (size) {
        case 0: val = *(uint8_t *)real; if (opc == 2) val = (uint64_t)(int64_t)(int8_t)val; break;
        case 1: val = *(uint16_t *)real; if (opc == 2) val = (uint64_t)(int64_t)(int16_t)val; break;
        case 2: val = *(uint32_t *)real; if (opc == 2 || opc == 3) val = (uint64_t)(int64_t)(int32_t)val; break;
        default: val = *(uint64_t *)real; break;
        }
        if (rt != 31) X[rt] = val;
    }
    uc->uc_mcontext->__ss.__pc += 4; // skip the faulting load/store
    return 1;
}
void jit86_lazyguard(int sig, siginfo_t *si, void *uc) {
    // W6A item 1: a non-PIE absolute DATA ref into the low link range -> serve the access at +bias and
    // advance the host PC. Inert unless g_nonpie_lo is set (ET_EXEC only).
    if (nonpie_fixup(si, uc)) return;
    // W6A item 3 (SMC): a guest write to a translated, write-protected JIT code page. Drop the cached
    // translations + IBTC (they're stale; do NOT reset g_cp -> the currently-running block's host code
    // stays intact, orphaned translations are reclaimed by the normal wholesale flush), unprotect the
    // page (smc_on_write retries + the write lands), and let the modified bytes re-translate on next
    // execution. smc_on_write is inert unless a JIT guest is present (g_rwx_guest) -> matrix bit-exact.
    if (si && si->si_addr && smc_on_write((uint64_t)si->si_addr)) {
        memset(g_map, 0, sizeof g_map);
        memset(g_ibtc, 0, sizeof g_ibtc);
        g_npend = 0;
        return;
    }
    // A genuine guest fault (isolated wild pointer / null deref) with a registered handler is the guest's
    // to handle; legitimate glibc vector over-reads are ADJACENT to a live mapping and still fall through
    // to the lazy zero-page map below.
    { void *fa = si ? si->si_addr : NULL; uintptr_t fpg = (uintptr_t)fa & ~(uintptr_t)0xFFF;
      if (!(fa && !lazy_nofix() && lazy_neighbor_mapped(fpg)) && deliver_guest_fault(sig, si, uc)) return; }
    void *a = si ? si->si_addr : NULL;
    if (a) {
        uintptr_t pg = (uintptr_t)a & ~(uintptr_t)0xFFF;
        // W6A item 4: classify by adjacency. A fault adjacent to an existing mapping is legitimate
        // growth/over-read and draws on the large grow budget; an isolated fault is a candidate wild
        // pointer on the small budget. NOLAZYFIX=1 forces the legacy single small monotonic budget.
        int adjacent = !lazy_nofix() && lazy_neighbor_mapped(pg);
        int ok = adjacent ? (g_growmaps < (256 << 10)) /* 1GB of grow pages */ : (g_lazymaps < lazy_budget());
        if (ok) {
            static int hooked;
            if (!hooked) { hooked = 1; atexit(lazy_diag); }
            // macOS won't MAP_FIXED over a sub-range of an existing VM entry (EINVAL); try mprotect
            // first (the page often exists as a PROT_NONE guard), then a fresh map.
            if (mprotect((void *)pg, 0x1000, PROT_READ | PROT_WRITE) == 0) {
                if (adjacent) g_growmaps++;
                else g_lazymaps++;
                return;
            }
            void *r = mmap((void *)pg, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
            if (r != MAP_FAILED) {
                if (adjacent) g_growmaps++;
                else g_lazymaps++;
                return;
            } // retry the faulting instruction
        }
    }
    if (getenv("CRASHDBG")) { // diagnostic: dump the guest instruction that faulted (gated; off by default)
        struct cpu *c = (struct cpu *)pthread_getspecific(g_cpu_key);
        fprintf(stderr, "[FAULT] sig=%d addr=%p guest_rip=%llx\n", sig, si ? si->si_addr : 0,
                c ? (unsigned long long)c->rip : 0);
        if (c && c->rip) {
            fprintf(stderr, "  bytes@rip:");
            uint8_t *p = (uint8_t *)c->rip;
            for (int i = 0; i < 16; i++) fprintf(stderr, " %02x", p[i]);
            fprintf(stderr, "\n  rax=%llx rsi=%llx rdi=%llx rsp=%llx rbp=%llx\n",
                    (unsigned long long)c->r[0], (unsigned long long)c->r[6], (unsigned long long)c->r[7],
                    (unsigned long long)c->r[4], (unsigned long long)c->r[5]);
        }
    }
    signal(sig, SIG_DFL);
    raise(sig); // out of budget / mmap failed -> real crash
}
// Synchronous CPU faults other than SIGSEGV/SIGBUS (which the run path wires to jit86_lazyguard above): a
// guest may install a handler for SIGILL/SIGFPE/SIGTRAP and DELIBERATELY trigger it -- e.g. a CPU-feature
// probe that executes an optional instruction guarded by a SIGILL handler (ud2 / 0F 0B once the feature is
// declared absent), an integer div-by-zero relying on a SIGFPE handler, or an int3 caught via SIGTRAP. The
// x86 frontend emits/raises these as real host signals, but rt_sigaction only records the guest handler --
// it does not install a host handler for synchronous signals (they are served by the guards installed here)
// -- so without this the trap is fatal (exit 255) instead of reaching the guest's handler.
//
// This is the analogue of os/linux/elf.c's install_sync_fault_guards() (aarch64). We do NOT reuse
// jit86_lazyguard: its lazy zero-page path keys off si_addr, which for these signals is the faulting PC (in
// a mapped, executable JIT page) -- lazy_neighbor_mapped() would judge it "legitimate growth", skip
// deliver_guest_fault, and mprotect/retry the PC page in a loop. Instead route straight to nonpie_fixup
// (which self-declines: si_addr is the high faulting PC, never in the low non-PIE link range) and then
// deliver_guest_fault (delivers the guest signal when the guest has a handler, else re-raises the default).
// CRASHDBG handles these via its mach exception port + diagnostics instead, so leave that path untouched.
static void jit86_syncguard(int sig, siginfo_t *si, void *uc) {
    if (nonpie_fixup(si, uc)) return;
    if (deliver_guest_fault(sig, si, uc)) return;
    signal(sig, SIG_DFL);
    raise(sig);
}
__attribute__((constructor)) static void jit86_install_sync_fault_guards(void) {
    if (getenv("CRASHDBG")) return;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = jit86_syncguard;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
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
