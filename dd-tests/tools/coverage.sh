#!/usr/bin/env bash
# dd-tests/tools/coverage.sh -- find syscalls/opcodes the engines DON'T implement.
#
# Two complementary lenses:
#   static   parse the runtime's mapping switches and diff them against the full kernel ABI:
#              * canonical (aarch64/asm-generic) syscalls handled by os/linux/service.c's `switch (nr)`
#                vs every __NR_* in <asm-generic/unistd.h>  -> the MISSING canonical syscalls (by name)
#              * x86-64 syscalls mapped by frontend/x86_64/sysmap.h vs the ones that fall through to the
#                default (CANON_X86ONLY) -> x86 syscalls with no canonical target
#   dynamic  run a corpus (the compiled dd-tests guests + busybox applets) through each engine and
#              aggregate the engine's own diagnostics:
#                "[jit] unhandled syscall N ..."     (syscall reached the default arm)
#                "[jit86] UNIMPL <0F|1B> opcode 0xNN" (x86 instruction not translated)
#              resolving each syscall number back to its name.
#
#   bash dd-tests/tools/coverage.sh [static|dynamic|all]   (default: all)
#
# Static needs only the host headers + the runtime sources. Dynamic needs the built engines
# (`make jit`) and reaches the macOS JIT through the `mac` bridge (same as the test harness).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RT="$ROOT/dd-jit/src/runtime"
SERVICE="$RT/os/linux/service.c"
SYSMAP="$RT/frontend/x86_64/sysmap.h"
UNISTD="/usr/include/asm-generic/unistd.h"
MODE="${1:-all}"

# ---- build the canonical syscall name<->number table from the kernel headers (via the preprocessor) ----
# names_table: lines "<num> <name>" for every __NR_* the asm-generic ABI defines.
names_table() {
    [ -r "$UNISTD" ] || return 0
    local names
    names="$(grep -oE '__NR_[a-z0-9_]+' "$UNISTD" | sort -u)"
    { echo '#include <asm-generic/unistd.h>'; for n in $names; do echo "$n ${n#__NR_}"; done; } \
        | gcc -E -P - 2>/dev/null | awk 'NF==2 && $1 ~ /^[0-9]+$/ {print $1" "$2}' | sort -n -u
}

# ---- handled canonical syscalls: the top-level `switch (nr)` case labels in service.c ----
# Brace-depth tracked so nested switches (ioctl/fcntl cmd values) are excluded; comments stripped.
handled_syscalls() {
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
            if (depth<=0 && seenopen) exit               # left the switch
            if (n>0) seenopen=1
        }
    ' "$SERVICE" | sort -n -u
}

static_syscalls() {
    echo "== static: canonical (aarch64) syscalls NOT handled by service.c =="
    local tbl handled
    tbl="$(names_table)"
    handled="$(handled_syscalls)"
    if [ -z "$tbl" ]; then echo "  (no $UNISTD on this host; skipping name resolution)"; return; fi
    local total=0 miss=0
    while read -r num name; do
        [ -z "$num" ] && continue
        total=$((total+1))
        if ! grep -qx "$num" <<<"$handled"; then miss=$((miss+1)); printf "  MISSING %-4s %s\n" "$num" "$name"; fi
    done <<<"$tbl"
    echo "  --- $miss missing / $total canonical syscalls (handled: $((total-miss))) ---"
}

static_x86() {
    echo ""
    echo "== static: x86-64 syscalls mapped by sysmap.h whose canonical target is NOT handled =="
    echo "   (these translate to a clean canonical number but then hit service.c's default -> -ENOSYS)"
    local mapped handled tbl mapped_count dead=0
    mapped="$(grep -oE 'case [0-9]+: return [0-9]+;' "$SYSMAP" | grep -oE '^case [0-9]+' | grep -oE '[0-9]+' | sort -n -u)"
    mapped_count="$(grep -c -E 'case [0-9]+: return [0-9]+;' "$SYSMAP")"
    handled="$(handled_syscalls)"
    tbl="$(names_table)"
    echo "  sysmap.h maps $mapped_count x86 syscall numbers; cross-checking their canonical targets:"
    # unique canonical targets that are not handled, named from the clean kernel table
    grep -oE 'case [0-9]+: return [0-9]+;' "$SYSMAP" | grep -oE 'return [0-9]+' | grep -oE '[0-9]+' | sort -n -u \
      | while read -r tgt; do
        if ! grep -qx "$tgt" <<<"$handled"; then
            local nm; nm="$(awk -v x="$tgt" '$1==x{print $2}' <<<"$tbl")"
            printf "  DEAD-MAP -> canonical %-4s %s\n" "$tgt" "${nm:-?}"
        fi
    done
}

# ---- dynamic: run a corpus and collect what the engines actually choke on ----
dyn_one() { # $1=engine-binary  $2..=argv (prefixed with optional --rootfs handled by caller)
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
    local tbl; tbl="$(names_table)"
    echo "  -- unhandled syscalls (number -> name) --"
    grep -oE 'unhandled syscall [0-9]+' "$log" | awk '{print $3}' | sort -n -u | while read -r n; do
        local nm; nm="$(awk -v x="$n" '$1==x{print $2}' <<<"$tbl")"
        printf "    %-4s %s\n" "$n" "${nm:-?}"
    done
    echo "  -- UNIMPL x86 opcodes --"
    grep -oE 'UNIMPL (0F|1B) opcode 0x[0-9a-fA-F]+' "$log" | sort -u | sed 's/^/    /'
    grep -qE 'unhandled syscall|UNIMPL' "$log" || echo "    (none hit over this corpus)"
    rm -f "$log"
}

case "$MODE" in
    static)  static_syscalls; static_x86 ;;
    dynamic) dynamic ;;
    all)     static_syscalls; static_x86; dynamic ;;
    *) echo "usage: coverage.sh [static|dynamic|all]"; exit 2 ;;
esac
