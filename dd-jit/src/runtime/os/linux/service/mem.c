// Extracted from service(): Memory — mmap/brk/mprotect/madvise syscalls. Returns 1 if nr was handled, 0 otherwise. Included by service.c
// after service/helpers.c, before service() — same TU scope (globals + helpers).
static int svc_mem(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
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
        int r = munmap((void *)a0, len);
        if (r == 0) {
            gmap_del(a0); // drop from the execve() teardown registry
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
        // mremap (copy+grow)
        void *r = mmap(0, (size_t)a2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (r == MAP_FAILED) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        size_t old = (size_t)a1, n = old < (size_t)a2 ? old : (size_t)a2;
        memcpy(r, (void *)a0, n);
        // W4-B leak fix: mremap MOVES the mapping, so free the OLD region (it was leaked — every
        // realloc-driven grow leaked the whole previous buffer; busybox `sort` of a 2M-line file does
        // ~3,874 grows leaking ~31 GB -> ~30 GB RSS -> OOM SIGKILL). Free the tracked extent (incl. the
        // 64 KB guard tail); allocate-before-free preserved above.
        uint64_t old_full = gmap_find_len(a0); // tracked extent (incl. guard tail); fall back to old length
        if (old_full < old) old_full = old;
        if (a0) {
            munmap((void *)a0, (size_t)old_full);
            gmap_del(a0);
            anon_untrack(a0, (size_t)old_full); // untrack the FULL extent (was untracking only `old`)
        }
        gmap_add((uint64_t)r, (uint64_t)a2);    // track the new region for execve() teardown
        anon_track((uint64_t)r, (size_t)a2, PROT_READ | PROT_WRITE); // fresh private-anon copy
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
        void *r = mmap((void *)a0, (size_t)a1 + guard, prot, mmap_flags((int)a3), (a3 & 0x20) ? -1 : (int)a4,
                       (off_t)a5);
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
            if (lf & 0x1) mf |= MS_ASYNC;       // Linux MS_ASYNC=1
            if (lf & 0x2) mf |= MS_INVALIDATE;  // Linux MS_INVALIDATE=2
            if (lf & 0x4) mf |= MS_SYNC;        // Linux MS_SYNC=4 -> macOS MS_SYNC(16)
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
    // mincore -> unsupported (callers fall back)
    case 232: G_RET(c) = (uint64_t)(-ENOSYS); break;
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
        if (adv >= 0 && adv <= 4) hadv = adv;
        else if (adv == 8) hadv = MADV_FREE;
        if (hadv >= 0 && madvise((void *)a0, (size_t)a1, hadv) < 0) { /* advisory: ignore */ }
        G_RET(c) = 0;
        break;
    }
    default: return 0;
    }
    return 1;
}
