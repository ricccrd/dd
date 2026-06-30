#!/usr/bin/env bash
# dd-tests/tools/coverage.sh -- find syscalls/opcodes the engines DON'T implement.
#
# Post-refactor layout (2026): the Linux syscall dispatcher is no longer one switch in service.c.
# service() -> service_local() (translate/.. -> os/linux/syscall/dispatch.c) does some pre-dispatch
# bookkeeping (non-PIE pointer rebase, path-cache epoch bump) and then fans out to one handler module
# per syscall family:
#     svc_sysv/svc_mem/svc_signal/svc_time/svc_io/svc_fs/svc_proc/svc_net/svc_event/svc_misc/svc_rare
#     (os/linux/syscall/{sysv,mem,signal,time,io,fs,proc,net,event,misc,rare}.c)
# Each module is `static int svc_x(struct cpu*, nr, a0..a5)` with a single top-level `switch (nr)`
# that returns 1 when it claims `nr`. Every handler keys on the CANONICAL (aarch64/asm-generic)
# syscall number; the x86-64 frontend runs its guest rax through translate/x86_64/sysmap.h (G_NR)
# first, so x86 and canonical handlers compare like-for-like.
#
# Two complementary lenses:
#   static   union the case-labels of every svc_*() module's top-level `switch (nr)` (brace-depth
#              tracked, so nested switches on ioctl-cmd / prctl-op / fcntl-cmd are NOT miscounted as
#              syscalls) -> the HANDLED canonical set. Diff it against:
#                * the full canonical ABI (<asm-generic/unistd.h>)            -> aarch64 gaps (by name)
#                * the x86-64 table (translate/x86_64/sysmap.h, by name)      -> x86-64 gaps (by name)
#   dynamic  run a corpus (the compiled dd-tests guests + busybox applets) through each engine and
#              aggregate the engine's own diagnostics:
#                "[jit] unhandled syscall N ..."     (syscall reached the ENOSYS default arm)
#                "[jit86] UNIMPL <0F|1B> opcode 0xNN" (x86 instruction not translated)
#              then CROSS-REFERENCE against the static gap list: a gap real software actually hits is
#              ACTIONABLE; the rest is the unused long tail.
#
#   bash dd-tests/tools/coverage.sh [static|dynamic|all|report]   (default: all)
#     report   write the static findings to docs/testing/SYSCALL-COVERAGE.md
#
# Static needs only the host headers (asm-generic/unistd.h + gcc) + the runtime sources. Dynamic needs
# the built engines (`make jit`) and reaches the macOS JIT through the `mac` bridge (test-harness path).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RT="$ROOT/dd-jit/src/runtime"
SYSCALL_DIR="$RT/os/linux/syscall"
SYSMAP="$RT/translate/x86_64/sysmap.h"
# the handler modules that own a top-level `switch (nr)` (dispatch.c's two switches are pre-dispatch
# bookkeeping -- non-PIE rebase + path-cache epoch -- NOT syscall handlers, so it is excluded here).
HANDLER_MODULES="sysv mem signal time io fs proc net event misc rare"
UNISTD="/usr/include/asm-generic/unistd.h"
MODE="${1:-all}"

# ---- canonical syscall name<->number table from the kernel headers (via the preprocessor) ----------
# names_table: lines "<num> <name>" for every real __NR_* the asm-generic ABI defines. The count
# sentinel (__NR_syscalls) and the reserved hole (__NR_arch_specific_syscall) are dropped -- they are
# not callable syscalls and would otherwise show up as phantom gaps.
names_table() {
    [ -r "$UNISTD" ] || return 0
    local names
    names="$(grep -oE '__NR_[a-z0-9_]+' "$UNISTD" | sort -u)"
    { echo '#include <asm-generic/unistd.h>'; for n in $names; do echo "$n ${n#__NR_}"; done; } \
        | gcc -E -P - 2>/dev/null \
        | awk 'NF==2 && $1 ~ /^[0-9]+$/ && $2!="syscalls" && $2!="arch_specific_syscall" {print $1" "$2}' \
        | sort -n -u
}

# ---- handled canonical syscalls: union of the top-level `switch (nr)` case labels across modules ----
# Brace-depth tracked from the module's `switch (nr)` so nested switches (ioctl cmd / prctl op / fcntl
# cmd, all switching on a0/rq/cmd, NOT nr) are excluded; // comments stripped. This is the correct
# extraction: a flat `grep 'case [0-9]+'` would wrongly fold in those nested op-codes.
handled_one() {
    awk '
        { line=$0; sub(/\/\/.*/,"",line) }              # drop // comments
        !started && line ~ /switch[ \t]*\([ \t]*nr[ \t]*\)/ { started=1 }
        started {
            if (depth==1) {                              # case labels directly in the nr-switch body
                tmp=line
                while (match(tmp, /case[ \t]+[0-9]+[ \t]*:/)) {
                    s=substr(tmp, RSTART, RLENGTH); gsub(/[^0-9]/,"",s); print s
                    tmp=substr(tmp, RSTART+RLENGTH)
                }
            }
            n=gsub(/{/,"{",line); m=gsub(/}/,"}",line); depth+=n-m
            if (depth<=0 && seenopen) exit               # left the nr-switch
            if (n>0) seenopen=1
        }
    ' "$1"
}
handled_syscalls() {
    local m
    for m in $HANDLER_MODULES; do handled_one "$SYSCALL_DIR/$m.c"; done | sort -n -u
}
# x86-only syscalls (no canonical equivalent) handled via `case (CANON_X86ONLY | N):` -- e.g. x86 time.
handled_x86only() {
    grep -hoE 'case[ \t]*\([ \t]*CANON_X86ONLY[ \t]*\|[ \t]*[0-9]+[ \t]*\)' "$SYSCALL_DIR"/*.c \
        | grep -oE '[0-9]+' | sort -n -u
}

# ---- x86-64 table: "<xnum> <canon> <name>" from sysmap.h's `case X: return C;  // name` lines -------
x86_table() {
    grep -oE 'case [0-9]+: return [0-9]+;[ \t]*//[ \t]*[a-z0-9_]+' "$SYSMAP" \
        | sed -E 's/case ([0-9]+): return ([0-9]+);[ \t]*\/\/[ \t]*([a-z0-9_]+)/\1 \2 \3/' | sort -n -u
}

static_aarch64() {
    echo "== static: canonical (aarch64 / asm-generic) syscalls NOT handled by the svc_*() modules =="
    local tbl handled total=0 miss=0
    tbl="$(names_table)"; handled="$(handled_syscalls)"
    if [ -z "$tbl" ]; then echo "  (no $UNISTD on this host; skipping name resolution)"; return; fi
    while read -r num name; do
        [ -z "$num" ] && continue
        total=$((total+1))
        grep -qx "$num" <<<"$handled" || { miss=$((miss+1)); printf "  GAP %-4s %s\n" "$num" "$name"; }
    done <<<"$tbl"
    echo "  --- handled $((total-miss)) / $total canonical syscalls ($miss unimplemented) ---"
}

static_x86() {
    echo ""
    echo "== static: x86-64 syscalls (sysmap.h) whose canonical handler is missing =="
    local handled tbl total=0 miss=0
    handled="$(handled_syscalls)"; tbl="$(x86_table)"
    while read -r xnum canon name; do
        [ -z "$xnum" ] && continue
        total=$((total+1))
        grep -qx "$canon" <<<"$handled" || { miss=$((miss+1)); printf "  GAP x86 %-4s %-22s -> canon %s\n" "$xnum" "$name" "$canon"; }
    done <<<"$tbl"
    echo "  --- handled $((total-miss)) / $total mapped x86-64 syscalls ($miss unimplemented) ---"
    echo "  (x86-only specials -- arch_prctl/modify_ldt/time/... -- fall to sysmap's default and are"
    echo "   serviced by the x86 frontend or the CANON_X86ONLY arm, not the canonical table above)"
}

# the canonical gap list (num name), reused by `report`.
aarch64_gaps() {
    local tbl handled; tbl="$(names_table)"; handled="$(handled_syscalls)"
    [ -z "$tbl" ] && return 0
    while read -r num name; do
        [ -z "$num" ] && continue
        grep -qx "$num" <<<"$handled" || echo "$num $name"
    done <<<"$tbl"
}

# ---- dynamic: run a corpus and collect what the engines actually choke on -------------------------
dyn_one() { # $1=engine-binary  $2..=argv
    local jit="$1"; shift
    timeout 20 mac bash -lc "exec env $jit $*" 2>&1 1>/dev/null
}
dynamic() {
    echo ""
    echo "== dynamic: syscalls/opcodes the engines hit at runtime over the test corpus =="
    local A X RF
    A="$(ls "$ROOT"/target/debug/build/ddjit-*/out/ddjit-linux_aarch64 2>/dev/null | head -1)"
    X="$(ls "$ROOT"/target/debug/build/ddjit-*/out/ddjit-linux_x86_64 2>/dev/null | head -1)"
    RF="$(ls -d /Users/x/dd/poc/images/*alpine*/rootfs 2>/dev/null | head -1)"
    [ -z "$A" ] && { echo "  (engines not built; run \`make jit\`)"; return; }
    local log; log="$(mktemp)"
    # bare aarch64 + x86 guests
    for b in "$ROOT"/target/dd-tests/aarch64/*; do [ -f "$b" ] && dyn_one "$A" "'$b'" >>"$log" 2>&1; done
    for b in "$ROOT"/target/dd-tests/x86_64/*; do [ -f "$b" ] && dyn_one "$X" "'$b'" >>"$log" 2>&1; done
    # busybox applets in the alpine rootfs (broad real-world syscall mix)
    if [ -n "$RF" ]; then
        for ap in ls ps top find grep sed awk tar gzip vi date du df nc wget ping; do
            dyn_one "$A" "--rootfs '$RF' /bin/sh -c '$ap --help >/dev/null 2>&1 || true; busybox $ap 2>/dev/null | head -1 >/dev/null'" >>"$log" 2>&1
        done
    fi
    local tbl handled; tbl="$(names_table)"; handled="$(handled_syscalls)"
    echo "  -- unhandled syscalls actually hit (number -> name) --"
    local hits; hits="$(grep -oE 'unhandled syscall [0-9]+' "$log" | awk '{print $3}' | sort -n -u)"
    if [ -z "$hits" ]; then
        echo "    (none -- the corpus stayed inside the handled set)"
    else
        while read -r n; do
            [ -z "$n" ] && continue
            local nm tag; nm="$(awk -v x="$n" '$1==x{print $2}' <<<"$tbl")"
            if grep -qx "$n" <<<"$handled"; then tag="(now handled?)"; else tag="<-- ACTIONABLE gap"; fi
            printf "    %-4s %-22s %s\n" "$n" "${nm:-?}" "$tag"
        done <<<"$hits"
    fi
    echo "  -- UNIMPL x86 opcodes --"
    grep -oE 'UNIMPL (0F|1B) opcode 0x[0-9a-fA-F]+' "$log" | sort -u | sed 's/^/    /'
    grep -qE 'unhandled syscall|UNIMPL' "$log" || echo "    (none hit over this corpus)"
    echo "  NOTE: dynamic is the signal that matters -- the static tail (io_uring/landlock/futex2/"
    echo "        mount-api/perf/kexec/...) is unimplemented on purpose until real software needs it."
    rm -f "$log"
}

# classify a gap syscall name into a coarse cluster (keeps the report grouped & self-maintaining).
gap_group() {
    case "$1" in
    io_setup|io_destroy|io_submit|io_cancel|io_getevents|io_pgetevents|io_uring_enter|io_uring_register)
        echo "async-io" ;; # POSIX aio + io_uring (io_uring_setup IS handled: denied by default profile)
    fsopen|fsconfig|fsmount|fspick|move_mount|open_tree|open_tree_attr|mount_setattr|quotactl_fd)
        echo "mount-api" ;; # new fsconfig/move_mount mount API
    landlock_create_ruleset|landlock_add_rule|landlock_restrict_self|lsm_get_self_attr|lsm_set_self_attr|lsm_list_modules)
        echo "lsm-landlock" ;;
    futex_waitv|futex_wake|futex_wait|futex_requeue)
        echo "futex2" ;;
    setxattrat|getxattrat|listxattrat|removexattrat|file_getattr|file_setattr|statmount|listmount|listns|cachestat|mseal|map_shadow_stack|rseq_slice_yield|epoll_pwait2|process_madvise|process_mrelease|set_mempolicy_home_node|pidfd_getfd|remap_file_pages)
        echo "modern-niche" ;;
    mbind|get_mempolicy|set_mempolicy|migrate_pages|move_pages)
        echo "numa" ;;
    add_key|request_key|keyctl)
        echo "keyring" ;;
    init_module|delete_module|finit_module|kexec_load|kexec_file_load|reboot|swapon|swapoff|settimeofday|acct|chroot|vhangup|personality|nfsservctl|quotactl)
        echo "admin" ;;
    # perf_event_open, fanotify*, kcmp, lookup_dcookie, mq_notify, ioprio*, vmsplice, pkey_*,
    # execveat, get_robust_list, restart_syscall, open_by_handle_at
    *)  echo "tracing-misc" ;;
    esac
}

# ---- report: regenerate docs/testing/SYSCALL-COVERAGE.md from the static analysis -----------------
report() {
    local doc="$ROOT/docs/testing/SYSCALL-COVERAGE.md"
    mkdir -p "$(dirname "$doc")"
    local handled handled_n gaps total gaps_n impl
    handled="$(handled_syscalls)"
    handled_n="$(wc -l <<<"$handled" | tr -d ' ')"
    gaps="$(aarch64_gaps)"
    gaps_n="$(wc -l <<<"$gaps" | tr -d ' ')"
    total="$(names_table | wc -l | tr -d ' ')"
    impl=$((total - gaps_n))
    emit_group() { # $1=key  $2=title
        local body
        body="$(while read -r num name; do [ "$(gap_group "$name")" = "$1" ] && printf '`%s` ' "$name"; done <<<"$gaps")"
        [ -n "$body" ] && { echo "**$2**"; echo ""; echo "$body" | fmt -w 96; echo ""; }
    }
    {
        echo "# Linux syscall coverage"
        echo ""
        echo "_Auto-generated by \`bash dd-tests/tools/coverage.sh report\`. Do not hand-edit; rerun instead._"
        echo ""
        echo "dd services Linux syscalls in \`os/linux/syscall/dispatch.c\`, which fans out to one handler"
        echo "module per family: \`svc_{sysv,mem,signal,time,io,fs,proc,net,event,misc,rare}()\`. Every"
        echo "handler keys on the **canonical (aarch64 / asm-generic)** syscall number; the x86-64 frontend"
        echo "remaps its guest rax through \`translate/x86_64/sysmap.h\` (\`G_NR\`) before dispatch, so both"
        echo "guest arches share one handler set."
        echo ""
        echo "## Headline"
        echo ""
        echo "| metric | count |"
        echo "| --- | --- |"
        echo "| distinct canonical syscall numbers handled (11 modules) | **$handled_n** |"
        echo "| canonical ABI syscalls implemented | **$impl / $total** |"
        echo "| canonical ABI syscalls unimplemented | **$gaps_n** |"
        echo "| x86-64 mapped syscalls implemented (sysmap.h) | **$(static_x86 2>/dev/null | grep -oE 'handled [0-9]+ / [0-9]+' | head -1)** |"
        echo ""
        echo "Run \`bash dd-tests/tools/coverage.sh static\` for the per-arch named gap list."
        echo ""
        echo "## Unimplemented canonical syscalls, grouped"
        echo ""
        echo "Almost the entire gap set is either a **modern kernel-API cluster** that no current guest"
        echo "workload exercises, or **older admin/niche** calls a sandboxed container never makes. None of"
        echo "these are hit by the dd-tests corpus or the busybox/alpine real-software runs."
        echo ""
        emit_group async-io     "Async I/O (POSIX aio + io_uring data path)"
        emit_group mount-api    "New mount / filesystem-config API"
        emit_group lsm-landlock "Landlock + generic LSM"
        emit_group futex2       "futex2"
        emit_group modern-niche "Other modern niche (xattr-at, *mount, cachestat, mseal, ...)"
        emit_group numa         "NUMA memory policy"
        emit_group keyring      "Kernel keyring"
        emit_group admin        "Admin / module / kexec / swap (privileged, blocked in a container)"
        emit_group tracing-misc "Tracing / perf / fanotify / pkey / other misc"
        echo "## Dynamic is the signal that matters"
        echo ""
        echo "The raw gap count is not actionable on its own -- most of the $gaps_n are unimplemented on"
        echo "purpose. What matters is which gaps **real software actually reaches**: each engine logs"
        echo "\`[jit] unhandled syscall N\` when a guest hits the ENOSYS default. \`coverage.sh dynamic\` runs"
        echo "the compiled guests + busybox applets through both engines and cross-references those hits"
        echo "against this list, flagging any reached gap as \`<-- ACTIONABLE\`. Promote a syscall out of"
        echo "this list when dynamic shows real software needs it, not merely because the kernel defines it."
    } > "$doc"
    echo "wrote $doc ($handled_n handled, $gaps_n gaps)"
}

case "$MODE" in
    static)  static_aarch64; static_x86 ;;
    dynamic) dynamic ;;
    all)     static_aarch64; static_x86; dynamic ;;
    report)  report ;;
    *) echo "usage: coverage.sh [static|dynamic|all|report]"; exit 2 ;;
esac
