//! memory — RSS leak / sustainability probes. The gap nothing else covered: a guest faults a large
//! working set repeatedly and frees it, then checks its OWN resident set didn't grow (a per-iteration
//! engine leak makes RSS climb monotonically; a clean release stays flat). Verdict `bounded=1` is golden
//! (RSS itself isn't), so these run on every engine. These catch GUEST-VISIBLE leaks (engine failing to
//! release guest pages / per-thread / per-fork / per-fd state). Engine-INTERNAL leaks (code cache, the
//! mc_ stat cache, execve teardown) need external host-RSS sampling — see tools/memwatch + docs/MEMORY.md.
//! Owner: memory lane. A `bounded=0` / fail here = a real leak → xfail + GAPS row, don't fix the engine.
#![allow(unused_imports)]
use crate::{group, src, port, Group};

pub fn groups() -> Vec<Group> {
    vec![group("memory", vec![
        // anon mmap+touch+munmap ×128 of 32 MiB — munmap must return pages to the OS.
        port("mmapfree", "ext_mem/mmapfree.c").out("mmapfree bounded=1\n"),
        // large malloc+memset+free ×160 of 24 MiB — >128 KiB chunks return to the OS.
        port("mallocfree", "ext_mem/mallocfree.c").out("mallocfree bounded=1\n"),
        // pthread create+join ×512 — per-thread stacks/contexts must be reclaimed.
        port("threadrss", "ext_mem/threadrss.c").out("threadrss bounded=1\n"),
        // fork+wait ×400 — parent RSS must not grow per child (host pid/struct leak).
        port("forkrss", "ext_mem/forkrss.c").out("forkrss bounded=1\n"),
        // open+close /dev/null ×8000 — fd-table / per-fd metadata must not grow.
        port("fdrss", "ext_mem/fdrss.c").out("fdrss bounded=1\n"),
        // file-backed mmap+munmap ×128 of 16 MiB — mapping bookkeeping must be released.
        port("mmapfilerss", "ext_mem/mmapfilerss.c").out("mmapfilerss bounded=1\n"),
    ])]
}
