// Extracted from service(): Memory — mmap/brk/mprotect/madvise syscalls. Returns 1 if nr was handled, 0 otherwise.
// Included by service.c after service/helpers.c, before service() — same TU scope (globals + helpers).
// process_vm_readv/writev between two iovec arrays. In this single-address-space DBT the "remote"
// process is always the guest itself, so both vectors point into directly-dereferenceable guest memory
// and the transfer is a scatter/gather memcpy -- exactly the kernel's stream semantics: bytes flow from
// the src vectors into the dst vectors in order, stopping when either side is exhausted. Returns the
// number of bytes copied.
static ssize_t svc_vm_iov_copy(const struct iovec *dst, unsigned long dcnt, const struct iovec *src,
                               unsigned long scnt) {
    ssize_t total = 0;
    unsigned long di = 0, si = 0;
    size_t doff = 0, soff = 0;
    while (di < dcnt && si < scnt) {
        size_t drem = dst[di].iov_len - doff, srem = src[si].iov_len - soff;
        size_t n = drem < srem ? drem : srem;
        if (n) {
            memcpy((char *)dst[di].iov_base + doff, (char *)src[si].iov_base + soff, n);
            total += (ssize_t)n;
            doff += n;
            soff += n;
        }
        if (doff == dst[di].iov_len) di++, doff = 0;
        if (soff == src[si].iov_len) si++, soff = 0;
    }
    return total;
}

// True if [addr,addr+len) lies entirely within a single tracked GUEST mapping (the gmap registry that
// records every guest mmap). Used to confine the cage-hint honoring in case 222 to memory the guest
// itself reserved -- never the engine's own internal allocations.
static int gmap_contains(uint64_t addr, uint64_t len) {
    uint64_t end = addr + len;
    for (int i = 0; i < g_ngmap; i++)
        if (g_gmap[i].addr <= addr && end <= g_gmap[i].addr + g_gmap[i].len) return 1;
    return 0;
}

static int svc_mem(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                   uint64_t a5) {
    switch (nr) {
    // ===================== Memory — mmap/brk/mprotect/madvise (anon charged against cgroup memory.max)
    // =====================
    // brk
    case 214: {
        if (!G_BRK_GROWABLE) { // fixed, non-growable break -> glibc/musl fall back to their mmap allocator
            G_RET(c) = brk_lo;
            break;
        }
        if (a0 == 0) {
            G_RET(c) = brk_cur;
            break;
        }
        if (a0 >= brk_lo && a0 <= brk_hi) {
            // heap growth -> charge cgroup memory.max
            if (g_mem_max && a0 > brk_cur) {
                uint64_t delta = a0 - brk_cur;
                if (atomic_fetch_add(&g_mem_charged, delta) + delta > g_mem_max) {
                    atomic_fetch_sub(&g_mem_charged, delta);
                    G_RET(c) = brk_cur;
                    // over limit -> break unchanged (ENOMEM)
                    break;
                }
                // shrink -> uncharge
            } else if (g_mem_max && a0 < brk_cur) {
                uint64_t delta = brk_cur - a0, cur = atomic_load(&g_mem_charged);
                atomic_fetch_sub(&g_mem_charged, delta > cur ? cur : delta);
            }
            brk_cur = a0;
        }
        G_RET(c) = brk_cur;
        break;
    }
    case 215: {
        // munmap. A non-fixed anon mapping carries a 64 KB guard tail that mmap (case 222) reserved
        // past the guest's logical length (so glibc's vectorized over-reads land in mapped memory).
        // The guest only knows its logical length a1, so a plain munmap(a0, a1) leaves that tail mapped
        // -> ~64 KB of address space (plus its gmap/anon_track bookkeeping) leaks per map/unmap cycle.
        // When a0 starts a tracked mapping whose FULL extent is exactly a1 + the 64 KB guard, extend the
        // unmap to cover the tail too. The gmap registry stores the full extent (incl. guard); requiring
        // an exact `full == a1 + 0x10000` match means a0 is the mapping start AND a1 is its original
        // logical length -- i.e. a complete unmap -- so this can never reach past the mapping into a
        // neighbour (a partial unmap, full == a1, leaves the tail alone). Guard-less mappings (file/fixed,
        // full == a1) and untracked mappings (full == 0) keep the plain a1 unmap unchanged.
        size_t len = (size_t)a1;
        uint64_t full = gmap_find_len(a0);
        if (full == (uint64_t)a1 + 0x10000) len = (size_t)full; // complete unmap of a guarded mapping
        // Page-size mismatch: guest pages are 4 KB (AT_PAGESZ) but the macOS host uses 16 KB pages.
        // A guest that releases a 4 KB-granular SUB-RANGE of a larger mapping -- e.g. V8's page
        // allocator freeing an interior chunk of a reservation -- passes an address that is 4 KB- but
        // not 16 KB-aligned, which host munmap rejects with EINVAL (V8 then aborts on its CHECK(0 ==
        // munmap)). A host-page-aligned address keeps the exact original path (native munmap rounds the
        // length up to a host page, preserving the guarded-complete-unmap behaviour above). For an
        // unaligned start we can only release whole HOST pages lying ENTIRELY inside [a0, a0+len); the
        // partially-covered edge pages stay mapped because they still back the neighbouring sub-regions
        // the guest keeps. The guest's logical unmap still succeeds (return 0) -- matching Linux, which
        // never faults an unmap of a partially/already-unmapped range.
        size_t hp = (size_t)getpagesize();
        int r;
        if ((a0 & (hp - 1)) == 0) {
            r = munmap((void *)a0, len);
        } else {
            uint64_t lo = (a0 + hp - 1) & ~(uint64_t)(hp - 1); // first host page fully in range
            uint64_t hi = (a0 + len) & ~(uint64_t)(hp - 1);    // end of last host page fully in range
            r = (lo < hi) ? munmap((void *)lo, (size_t)(hi - lo)) : 0;
        }
        if (r == 0) {
            gmap_del(a0);          // drop from the execve() teardown registry
            anon_untrack(a0, len); // and from the DONTNEED anon-range registry (covers the freed tail)
        }
        if (r == 0 && g_mem_max) {
            // uncharge (clamp >=0)
            uint64_t cur = atomic_load(&g_mem_charged), d = (uint64_t)a1;
            atomic_fetch_sub(&g_mem_charged, d > cur ? cur : d);
        }
        G_RET(c) = (uint64_t)r;
        break;
    }
    case 216: {
        // mremap (a0=old, a1=old_len, a2=new_len, a3=flags, a4=new_addr). macOS has no mremap, so
        // emulate it -- but honor the FLAGS contract, which the guest relies on:
        //   flags==0        : the mapping MUST NOT move. Grow only if the tail is free, else -ENOMEM.
        //   MREMAP_MAYMOVE  : may relocate (allocate a new region, copy, free the old).
        // Getting this wrong corrupts the guest: a flags==0 caller keeps using the OLD address (Linux
        // guarantees it is unchanged), so relocating -- and freeing the old region out from under those
        // still-live pointers -- is a use-after-free (BUG #211: glibc/ZendMM grows a ~2 MB json_encode
        // buffer by one page with a no-move mremap; the old code always moved it -> SIGSEGV).
        // The original anon mmap (case 222) reserved a 64 KB guard tail past the guest's logical length,
        // so the tracked extent is a1+guard; a grow whose new length still fits inside that already-mapped
        // extent needs neither new memory nor a move.
        const uint64_t guard = 0x10000;
        uint64_t tracked = gmap_find_len(a0);          // full mapped extent at a0 (incl. guard), 0 if untracked
        uint64_t phys = tracked ? tracked : (uint64_t)a1; // bytes we can assume are mapped at a0
        // Shrink, or a grow that still fits within the already-mapped extent: stay in place, touch nothing.
        if ((uint64_t)a2 <= phys) {
            G_RET(c) = a0;
            break;
        }
        // Grow beyond the current extent. Unless a fixed destination was requested, first try to extend in
        // place by mapping the fresh tail right after the current extent; macOS relocates a hinted (non-
        // FIXED) mmap when the target range isn't free, so an exact-address result means the tail was free.
        if (!(a3 & 2 /*MREMAP_FIXED*/)) {
            uint64_t end = a0 + phys, want = (uint64_t)a2 + guard;
            void *ext =
                mmap((void *)end, (size_t)(a0 + want - end), PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
            if (ext == (void *)end) {
                gmap_del(a0);
                gmap_add(a0, want); // track the grown extent (incl. fresh guard) for execve() teardown
                anon_track(a0, want, PROT_READ | PROT_WRITE);
                G_RET(c) = a0;
                break;
            }
            if (ext != MAP_FAILED) munmap(ext, (size_t)(a0 + want - end)); // landed elsewhere -> discard
        }
        // Cannot extend in place. Without MREMAP_MAYMOVE we may not relocate -> ENOMEM (the caller then
        // does its own alloc+copy+free, exactly as it would when Linux can't grow a no-move mapping).
        if (!(a3 & 1 /*MREMAP_MAYMOVE*/)) {
            G_RET(c) = (uint64_t)(-ENOMEM);
            break;
        }
        // Relocate: allocate the new region (+guard tail so glibc's vectorized over-reads stay mapped),
        // copy the old bytes, then free the old extent. Allocate-before-free so a failure leaves old intact.
        void *r = mmap(0, (size_t)a2 + guard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (r == MAP_FAILED) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        size_t n = (size_t)a1 < (size_t)a2 ? (size_t)a1 : (size_t)a2;
        memcpy(r, (void *)a0, n);
        if (a0) {
            munmap((void *)a0, (size_t)phys); // free the FULL tracked extent (incl. old guard tail)
            gmap_del(a0);
            anon_untrack(a0, (size_t)phys);
        }
        gmap_add((uint64_t)r, (uint64_t)a2 + guard);                         // track for execve() teardown
        anon_track((uint64_t)r, (uint64_t)a2 + guard, PROT_READ | PROT_WRITE); // fresh private-anon copy
        G_RET(c) = (uint64_t)r;
        break;
    }
    // mmap
    case 222: {
        // File-backed mmap of a RAM-backed scratch fd: flush the cache so the mapping sees the real bytes.
        if (!(a3 & 0x20)) memf_materialize((int)a4);
        // charge anon, but NOT MAP_NORESERVE
        int charge = g_mem_max && (a3 & 0x20) && !(a3 & 0x4000);
        //   (libc reserves huge virtual arenas it never commits;
        if (charge) {
            if (atomic_fetch_add(&g_mem_charged, (uint64_t)a1) + (uint64_t)a1 >
                // real memory.max counts RSS, not reservations)
                g_mem_max) {
                atomic_fetch_sub(&g_mem_charged, (uint64_t)a1);
                G_RET(c) = (uint64_t)(-ENOMEM);
                break;
            }
        }
        // glibc's vectorized string ops over-read up to 16 bytes past a buffer's logical end; on Darwin
        // that hits an unmapped page -> SIGBUS. Map a 64KB guard tail on non-fixed anon maps so the
        // over-read lands in mapped zero memory (x86 glibc relies on this; harmless for aarch64).
        size_t guard = (!(a3 & 0x10) && (a3 & 0x20)) ? 0x10000 : 0;
        // mprotect (case 226) is a no-op (the JIT never executes guest pages), so a later PROT_READ ->
        // PROT_READ|WRITE upgrade would be silently dropped. Map ANON memory writable up front so the
        // upgrade is already in effect (redis' checkLinuxMadvFreeForkBug mmaps R then mprotects RW then stores).
        int prot = (a3 & 0x20) ? ((int)a2 | PROT_READ | PROT_WRITE) : (int)a2;
        // W6A item 3: guest RWX / PROT_EXEC mmaps (JVM/V8/LuaJIT/.NET/PyPy JIT arenas). On macOS a
        // non-MAP_JIT mmap that requests PROT_EXEC fails with EPERM under the hardened W^X policy, so
        // these guests can't allocate their code arena. But this is a DBT: the host NEVER executes guest
        // pages natively -- guest "execution" is translate_block() reading the page's bytes and emitting
        // host code into the (separately RX) code cache. So PROT_EXEC on a guest mapping is meaningless to
        // the host and only triggers the EPERM. Strip it: the page is mapped R+W, the guest writes its
        // generated code, "executes" it (guest PC enters the page -> map_host miss -> translate), and runs.
        // Setting g_rwx_guest also arms the (otherwise inert) SMC write-fault invalidation in frontend/x86_64
        // so a guest that OVERWRITES already-translated code re-translates. NORWXFIX=1 disables the strip.
        if (!getenv("NORWXFIX") && (a3 & 0x20) && (prot & PROT_EXEC)) {
            prot = (prot & ~PROT_EXEC) | PROT_READ | PROT_WRITE;
            g_rwx_guest = 1; // a JIT guest is present (informational + SMC gate)
        }
        void *r =
            mmap((void *)a0, (size_t)a1 + guard, prot, mmap_flags((int)a3), (a3 & 0x20) ? -1 : (int)a4, (off_t)a5);
        // Past-EOF tail zero-fill. A file mmap whose length runs past the file's end leaves the trailing
        // WHOLE pages with no backing: macOS SIGBUSes on any read of them. ld.so does exactly this -- it maps
        // a .so's WHOLE vaddr span from the FIRST segment, so the inter-segment bytes become such past-EOF
        // pages. On Linux they are equally unbacked, but ld.so PROT_NONEs / replaces that region and never
        // reads it; with macOS's 16 KB pages, though, a later 4 KB-granular segment map (x86_64 .so p_align
        // 0x1000) shares its low host page with one of those past-EOF pages, so a stray access SIGBUSes where
        // Linux stayed quiet (julia's libdl/libjulia abort here). Re-map the genuinely-past-EOF whole-page
        // tail as anonymous zero -- the inaccessible-but-quiet region Linux effectively presents -- so such a
        // shared host page reads back zero instead of faulting. The partial page straddling EOF keeps macOS's
        // file bytes + zero-fill, a later MAP_FIXED segment map overwrites whatever it needs, and a fully
        // file-backed mapping (valid_end >= a1) is left byte-identical. RW only (an ANON PROT_EXEC map hits
        // macOS W^X EPERM; the JIT never executes guest pages anyway). MAP_PRIVATE only: a MAP_SHARED file
        // map past EOF can be made valid later by ftruncate-extending the file (sqlite/lmdb), so its tail
        // must stay the real shared mapping; ld.so's .so segments are all MAP_PRIVATE, so julia is covered.
        if (r != MAP_FAILED && (a3 & 0x02) && !(a3 & 0x20) && (int)a4 >= 0 && a1) {
            struct stat st;
            if (fstat((int)a4, &st) == 0) {
                uint64_t avail = (uint64_t)st.st_size > a5 ? (uint64_t)st.st_size - (uint64_t)a5 : 0;
                size_t hp = (size_t)getpagesize();
                uint64_t valid_end = (avail + hp - 1) & ~(uint64_t)(hp - 1); // first host page wholly past EOF
                if (valid_end < a1)
                    mmap((char *)r + valid_end, (size_t)(a1 - valid_end), PROT_READ | PROT_WRITE,
                         MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
            }
        }
        // 16 KB-vs-4 KB MAP_FIXED reconciliation. macOS arm64 mmap REQUIRES a 16 KB-aligned address for
        // MAP_FIXED, but x86_64 .so PT_LOAD segments are only p_align=0x1000 (4 KB), so ld.so's MAP_FIXED
        // mapping of e.g. libc's text segment at a 4 KB- (not 16 KB-) aligned guest address returns EINVAL
        // -> "failed to map segment from shared object" (file-backed) / "cannot map zero-fill pages" (the
        // anon BSS tail). (aarch64 .so segments use p_align 0x10000, a multiple of 16 KB, so they never hit
        // this.) ld.so has ALREADY reserved this whole .so address range with an earlier (kernel-placed,
        // 16 KB-aligned) mmap, so the range is ours -- emulate the failing fixed map with a private ANON
        // map at the 16 KB-rounded base, then pread the file bytes (file-backed) or leave it zero (anon
        // BSS): a private, writable copy, exactly what MAP_PRIVATE promises. Gated on the DIRECT mmap having
        // FAILED for a MAP_FIXED request, so every working case (non-fixed maps, and 16 KB-aligned aarch64
        // file/anon maps) takes the unchanged direct path above and is byte-identical.
        if (r == MAP_FAILED && (a3 & 0x10)) {
            uint64_t lo = a0 & ~(uint64_t)0x3fff; // round the start DOWN to a 16 KB host page
            size_t head = (size_t)(a0 - lo);      // bytes in the low page that belong to the PREVIOUS segment
            // The low page may also hold the tail of the previous PT_LOAD (a0 sits mid-16 KB-page). The ANON
            // MAP_FIXED below zeros that whole page, so snapshot the neighbour's bytes FIRST and restore them
            // after -- they were already written (prev segment / ld.so's reservation) and must survive. (The
            // past-EOF tail fill above guarantees the head is now readable -- a real neighbour byte or quiet
            // zero -- never a SIGBUSing hole. The HIGH edge needs no save: bytes past a0+a1 belong to the
            // NEXT segment, which refills them via its own map, or are this segment's BSS -> read as zero.)
            void *hsave = head ? malloc(head) : NULL;
            if (hsave) memcpy(hsave, (void *)lo, head);
            // RW only: the JIT never executes guest pages, and an ANON PROT_EXEC map hits macOS W^X EPERM.
            void *ar =
                mmap((void *)lo, (size_t)a1 + head, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
            if (ar != MAP_FAILED) {
                if (hsave) memcpy((void *)lo, hsave, head);            // restore the previous seg's tail
                if (!(a3 & 0x20) && (int)a4 >= 0)                      // file-backed: load the file bytes;
                    pread((int)a4, (void *)a0, (size_t)a1, (off_t)a5); //   short read => trailing BSS zeros
                r = (void *)a0; // success: the mapping now lives at the requested fixed guest address
            }
            free(hsave);
        }
        // V8 pointer-compression cage placement. macOS treats a non-MAP_FIXED address as a weak hint: it
        // lands AT the hint when that range is free (so node's randomly-based cage reservations work), but
        // when the hint overlaps an existing mapping macOS RELOCATES the new map far away (e.g. to
        // 0x70xx_xxxx). Linux instead honors the hint when a guest COMMITS fresh pages over its own
        // reservation (V8's BoundedPageAllocator carving heap pages out of the pointer-compression cage);
        // a guest that derives cage-relative (compressed) pointers from the hint faults when the page lands
        // outside the cage. So when macOS diverged from a high hint whose whole requested range is still
        // inside one of the guest's OWN tracked reservations, re-place the mapping AT the hint with
        // MAP_FIXED -- committing the fresh anon pages exactly where the guest expects. Gated on a DIVERGENT
        // result and on guest-owned coverage, so every already-correct placement (incl. all of node's, which
        // macOS honors) and anything touching engine-internal memory is left byte-identical and untouched.
        if (r != MAP_FAILED && a0 && (uint64_t)(uintptr_t)r != a0 && !(a3 & 0x10) && (a3 & 0x20) &&
            a0 >= 0x100000000ull && gmap_contains(a0, (uint64_t)a1 + guard)) {
            void *fr = mmap((void *)a0, (size_t)a1 + guard, prot, mmap_flags((int)a3) | MAP_FIXED, -1, 0);
            if (fr != MAP_FAILED) {
                munmap(r, (size_t)a1 + guard); // drop the relocated placement macOS chose
                r = fr;                        // mapping now lives at the requested cage-relative hint
            }
        }
        // refund
        if (r == MAP_FAILED && charge) atomic_fetch_sub(&g_mem_charged, (uint64_t)a1);
        if (r != MAP_FAILED) {
            gmap_add((uint64_t)r, (uint64_t)a1 + guard); // track for execve() teardown
            // DONTNEED anon registry: record PRIVATE-ANON ranges (incl. the guard tail); for any other
            // (file-backed/shared) mapping, forget overlapping anon coverage -- a MAP_FIXED file map may
            // now sit where anon used to, and we must never anon-remap over it.
            if ((a3 & 0x20) && (a3 & 0x02))
                anon_track((uint64_t)r, (uint64_t)a1 + guard, prot);
            else
                anon_untrack((uint64_t)r, (uint64_t)a1 + guard);
        }
        G_RET(c) = (r == MAP_FAILED) ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // mprotect
    case 226: // mprotect: NO-OP. The JIT translates guest code and never executes guest pages, so it does
        // not enforce guest page protection. Actually calling mprotect is harmful on macOS -- it would make
        // a region truly read-only and then fault the guest's own legitimate writes to it (e.g. RELRO).
        G_RET(c) = 0;
        break;
    case 227: // msync: stores through a MAP_SHARED mapping are already in the unified page cache, so the
        // file is coherent without an explicit flush; treat as success (avoids a spurious -ENOSYS).
        // Default/fast/none keep the no-op (page-cache coherent). Only `strict` issues a real host
        // msync for on-platter writeback durability, translating Linux MS_* flags to macOS (macOS
        // MS_SYNC=16 != Linux 4; MS_ASYNC=1/MS_INVALIDATE=2 match), tolerating EINVAL.
        if (s3db_durability() == 2) {
            int lf = (int)a2, mf = 0;
            if (lf & 0x1) mf |= MS_ASYNC;                    // Linux MS_ASYNC=1
            if (lf & 0x2) mf |= MS_INVALIDATE;               // Linux MS_INVALIDATE=2
            if (lf & 0x4) mf |= MS_SYNC;                     // Linux MS_SYNC=4 -> macOS MS_SYNC(16)
            if (!(mf & (MS_ASYNC | MS_SYNC))) mf |= MS_SYNC; // default to a sync flush
            int r = msync((void *)a0, (size_t)a1, mf);
            G_RET(c) = (r < 0 && errno != EINVAL) ? (uint64_t)(-errno) : 0;
        } else {
            G_RET(c) = 0;
        }
        break;
    case 228:
    case 229:
        G_RET(c) = 0;
        // mlock/munlock (no-op)
        break;
    // Container-init compat: in the single-process model these are no-ops that return success so
    // entrypoints (mount /proc, unshare, drop caps, set hostname) proceed; the path-jail is the
    // real boundary, and a faked namespace grants no actual privilege (program still runs as our uid).
    // mincore: report page residency. The host mincore(2) fills one status byte per HOST page; Linux
    // wants one byte per page with bit0 = resident. macOS sets MINCORE_INCORE(0x1) in bit0 already, so
    // masking each byte to bit0 yields the Linux convention. (Host pages are 16 KB vs the guest's 4 KB,
    // so sub-host-page granularity is coarser than a real 4 KB kernel, but residency of the covering
    // page is faithful.) Untouched trailing bytes (the guest zero-filled its vector) stay 0 = absent.
    case 232: {
        int r = mincore((void *)a0, (size_t)a1, (char *)a2);
        if (r == 0 && a1) {
            size_t hps = (size_t)getpagesize();
            size_t npages = ((size_t)a1 + hps - 1) / hps;
            unsigned char *vec = (unsigned char *)a2;
            for (size_t i = 0; i < npages; i++)
                vec[i] &= 1u; // Linux: bit0 = resident
        }
        G_RET(c) = (r < 0) ? (uint64_t)(-errno) : 0;
        break;
    }
    case 233: {
        // madvise: best-effort, advisory (never fail the guest). Only forward advice values whose
        // meaning is identical on both kernels -- NORMAL/RANDOM/SEQUENTIAL/WILLNEED/DONTNEED(0..4)
        // match, and Linux MADV_FREE(8) -> macOS MADV_FREE. Every OTHER Linux advice number collides
        // with an unrelated macOS one (e.g. Linux DONTFORK=10 vs macOS PAGEOUT=10), so no-op those.
        // (Note: macOS MADV_DONTNEED does not zero anonymous pages the way Linux's does.)
        int adv = (int)a2, hadv = -1;
        // MADV_DONTNEED(4): Linux drops the pages so the NEXT access faults in fresh ZERO pages. macOS
        // MADV_DONTNEED does not zero anon pages, so a reread would return stale data (breaks
        // redis/jemalloc, which lean on the zeroing). For a range fully inside a tracked PRIVATE-ANON
        // region we re-establish it with a fresh MAP_FIXED|MAP_ANON|MAP_PRIVATE mapping -> next read
        // faults in zeros. File-backed/shared mappings are NEVER touched here (the containment check
        // fails for them); they keep the safe advisory passthrough below.
        if (adv == 4 && a1) {
            int aprot = anon_prot_if_contained(a0, (size_t)a1);
            if (aprot >= 0) {
                void *r = mmap((void *)a0, (size_t)a1, aprot, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
                if (r != MAP_FAILED) {
                    G_RET(c) = 0;
                    break;
                }
                // remap failed (e.g. unaligned) -> never fail the guest; fall through to advisory
            }
        }
        if (adv >= 0 && adv <= 4)
            hadv = adv;
        else if (adv == 8)
            hadv = MADV_FREE;
        if (hadv >= 0 && madvise((void *)a0, (size_t)a1, hadv) < 0) { /* advisory: ignore */
        }
        G_RET(c) = 0;
        break;
    }
    // process_vm_readv: copy FROM the remote iovecs (a3/a4) INTO the local iovecs (a1/a2). Same address
    // space here, so it's a direct scatter/gather memcpy (the remote pid in a0 is the guest itself).
    case 270:
        G_RET(c) = (uint64_t)svc_vm_iov_copy((const struct iovec *)a1, (unsigned long)a2, (const struct iovec *)a3,
                                             (unsigned long)a4);
        break;
    // process_vm_writev: the mirror -- copy FROM the local iovecs (a1/a2) INTO the remote iovecs (a3/a4).
    case 271:
        G_RET(c) = (uint64_t)svc_vm_iov_copy((const struct iovec *)a3, (unsigned long)a4, (const struct iovec *)a1,
                                             (unsigned long)a2);
        break;
    // membarrier: CMD_QUERY(0) returns the bitmask of supported commands; the barrier commands issue a
    // process-wide full memory barrier. The host is cache-coherent and a seq-cst fence orders all threads,
    // so every (expedited or not, global or private) barrier is satisfied by a single host fence. The
    // REGISTER_* commands only arm the kernel's per-mm expedited fast path -- there is nothing to register
    // here, so they succeed as a no-op. SYNC_CORE variants additionally guarantee instruction-cache
    // coherence for self-modifying code; the guest's own JIT already flushes via its code-patch path, so a
    // fence suffices. glibc/Go/HAProxy probe QUERY, then REGISTER_PRIVATE_EXPEDITED(16) + PRIVATE_EXPEDITED(8).
    case 283:
        switch ((int)a0) {
        case 0: // CMD_QUERY -> bitmask of supported commands (every command we accept below)
            G_RET(c) = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6);
            break;
        case 1:        // CMD_GLOBAL
        case 2:        // CMD_GLOBAL_EXPEDITED
        case 8:        // CMD_PRIVATE_EXPEDITED
        case 32:       // CMD_PRIVATE_EXPEDITED_SYNC_CORE
            atomic_thread_fence(memory_order_seq_cst);
            G_RET(c) = 0;
            break;
        case 4:        // CMD_REGISTER_GLOBAL_EXPEDITED
        case 16:       // CMD_REGISTER_PRIVATE_EXPEDITED
        case 64:       // CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE
            G_RET(c) = 0; // arm the expedited fast path -> nothing to do in this coherent DBT
            break;
        default: G_RET(c) = (uint64_t)(-EINVAL); break;
        }
        break;
    default: return 0;
    }
    return 1;
}
