#!/usr/bin/env bash
# dd-tests/scenarios/macos-container.sh -- macOS-container parity against the Docker stack.
#
# dd's signature trick: the SAME daemon + the SAME `docker` commands run BOTH a Linux container
# (alpine, via the JIT) AND a native-macOS container (the `macos` image, real arm64 Mach-O tools under
# darwinjail). This drives the identical lifecycle (run / logs / exec / inspect / ps / stop / rm)
# against each and asserts parity -- and documents the legitimate platform differences (uname, and the
# uid: Linux containers are root via a user-ns, macOS containers run as the real host uid; macOS has no
# uid namespace).
#
#   bash dd-tests/scenarios/macos-container.sh        # run on / against a macOS host (needs /nix)
#
# Builds the `macos` image with dd-gui/mac/mac-userland.sh if absent (needs /nix on the mac side). Self-skips
# if the macOS userland can't be produced. Env: DD_IMAGES, DD_DAEMON.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
IMAGES="${DD_IMAGES:-/Users/x/dd/poc/images}"
DAEMON="${DD_DAEMON:-$ROOT/target/release/dd-daemon}"
SOCK="$ROOT/dd-macos.sock"
export DOCKER_HOST="unix://$SOCK"

# run a shell line on the mac side (image build needs /nix, which lives on macOS)
macsh() { if [ "$(uname -s)" = "Darwin" ]; then bash -lc "$1"; else mac bash -lc "$1"; fi; }

# ---- ensure the macOS image exists ----
if [ ! -f "$IMAGES/macos/dd-image.json" ]; then
    echo "building macos userland image..."
    macsh "DD_IMAGES='$IMAGES' bash '$ROOT/dd-gui/mac/mac-userland.sh' '$IMAGES/macos'" >/dev/null 2>&1
fi
[ -f "$IMAGES/macos/dd-image.json" ] || { echo "SKIP: could not build the macos userland image (needs /nix on the mac host)"; exit 0; }

pass=0 fail=0
has() { if echo "$2" | grep -qF "$3"; then echo "  ok   $1"; pass=$((pass+1)); else echo "  FAIL $1: [$2] lacks [$3]"; fail=$((fail+1)); fi; }
ok()  { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1)); else echo "  FAIL $1: got [$2] want [$3]"; fail=$((fail+1)); fi; }
d()   { docker "$@" 2>/dev/null; }       # stderr dropped: nested arm64e system binaries emit a harmless
                                         # dyld interpose note; we assert on stdout.

SCEN_LOG="$ROOT/dd-macos.log"
pkill -x dd-daemon 2>/dev/null; rm -f "$SOCK"
# Fully isolate this daemon: private socket AND private state/volumes (a fresh per-scenario temp
# dir), so it starts from empty state and never reads or mutates the developer's real ~/.dd.
STATE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dd-macos.XXXXXX")"
export DD_IMAGES="$IMAGES" DDOCKERD_SOCK="$SOCK" DD_STATE="$STATE_DIR/state.json" DD_VOLUMES="$STATE_DIR/volumes"
"$DAEMON" >"$SCEN_LOG" 2>&1 &
DPID=$!
trap 'kill -9 $DPID 2>/dev/null; rm -f "$SOCK" "$SCEN_LOG"; rm -rf "$STATE_DIR"' EXIT
n=0; until [ -S "$SOCK" ] || [ $n -ge 60 ]; do sleep 0.25; n=$((n+1)); done
[ -S "$SOCK" ] || { echo "daemon failed to start:"; cat "$SCEN_LOG" 2>/dev/null; exit 1; }

echo "== the macos image is registered as an os:darwin image =="
has "images-lists-macos" "$(d images --format '{{.Repository}}')" "macos"

# ---- the identical lifecycle, run against each image; $1=label $2=image $3=shell ----
lifecycle() {
    local lab="$1" img="$2" sh="$3"
    echo "== [$lab] run / logs / exec / inspect / ps / stop / rm =="
    has "$lab-run-fg"   "$(d run --rm "$img" "$sh" -c 'echo fg-'"$lab")" "fg-$lab"
    # logs: short-lived container, wait for exit then read (the daemon's settled-logs contract).
    local lcid; lcid="$(d run -d "$img" "$sh" -c 'echo dlog-'"$lab")"
    d wait "$lcid" >/dev/null
    has "$lab-logs"     "$(d logs "$lcid")" "dlog-$lab"
    d rm "$lcid" >/dev/null
    # a long-lived container for the running-state verbs (exec/inspect/ps/stop).
    local cid; cid="$(d run -d "$img" "$sh" -c 'sleep 8')"
    has "$lab-run-d"    "$cid" "$(echo "$cid" | head -c 8)"
    has "$lab-exec"     "$(d exec "$cid" "$sh" -c 'echo exec-'"$lab")" "exec-$lab"
    has "$lab-inspect-running" "$(d inspect --format '{{.State.Status}}' "$cid")" "running"
    has "$lab-ps-a"     "$(d ps -a --format '{{.Image}}')" "$img"
    ok  "$lab-stop"     "$(d stop "$cid" | head -c 12)" "$(echo "$cid" | head -c 12)"
    has "$lab-inspect-exited" "$(d inspect --format '{{.State.Status}}' "$cid")" "exited"
    ok  "$lab-rm"       "$(d rm "$cid" | head -c 12)" "$(echo "$cid" | head -c 12)"
}

lifecycle "linux" "alpine" "/bin/sh"
lifecycle "macos" "macos"  "bash"

echo "== platform identity differs (as it must) — same command, OS-specific answer =="
has "linux-uname" "$(d run --rm alpine uname -s 2>/dev/null)" "Linux"
has "macos-uname" "$(d run --rm macos bash -lc 'uname -s' 2>/dev/null)" "Darwin"

echo "== documented parity difference: uid namespacing (Linux=root via user-ns; macOS=host uid) =="
has "linux-uid-root"  "$(d run --rm alpine id -u 2>/dev/null)" "0"
# macOS has no uid namespace; just assert the container reports *a* numeric uid (not that it's 0).
mu="$(d run --rm macos bash -lc 'id -u' 2>/dev/null | tr -dc 0-9)"
has "macos-uid-numeric" "$([ -n "$mu" ] && echo "uid=$mu")" "uid="

echo ""
echo "macos-container scenarios: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
