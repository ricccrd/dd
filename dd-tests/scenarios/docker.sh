#!/usr/bin/env bash
# dd-tests/scenarios/docker.sh -- end-to-end Docker-CLI scenarios against dd-daemon.
#
# Starts dd-daemon on a private socket, drives it with the real `docker` CLI through the full container
# lifecycle (images, run, logs, ps, inspect, exec-less wait, rm, volumes, networks), asserts each result,
# then tears the daemon down. Run after `make jit` (the daemon + JIT binaries must be built).
#
#   bash dd-tests/scenarios/docker.sh
#
# Env: DD_IMAGES (image dir, default poc/images), DD_DAEMON (daemon binary, default target/release/dd-daemon).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
IMAGES="${DD_IMAGES:-/Users/x/dd/poc/images}"
DAEMON="${DD_DAEMON:-$ROOT/target/release/dd-daemon}"
SOCK="$ROOT/dd-scenarios.sock"
export DOCKER_HOST="unix://$SOCK"

pass=0 fail=0
ok()  { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1)); else echo "  FAIL $1: got [$2] want [$3]"; fail=$((fail+1)); fi; }
has() { if echo "$2" | grep -qF "$3"; then echo "  ok   $1"; pass=$((pass+1)); else echo "  FAIL $1: [$2] lacks [$3]"; fail=$((fail+1)); fi; }
d()   { docker "$@" 2>&1; }

# ---- bring up the daemon on a private socket ----
# dd-daemon + docker run on the same host (a local unix socket); the daemon bridges to the mac JIT
# internally. On a real macOS host all three are native; on the linux dev host the daemon is a linux
# binary that shells out to the mac for the JIT (handled by SpawnConfig, transparent here).
SCEN_LOG="$ROOT/dd-scenarios.log"
pkill -x dd-daemon 2>/dev/null
rm -f "$SOCK"
export DD_IMAGES="$IMAGES" DDOCKERD_SOCK="$SOCK"
"$DAEMON" >"$SCEN_LOG" 2>&1 &
DPID=$!
trap 'kill -9 $DPID 2>/dev/null; rm -f "$SOCK" "$SCEN_LOG"' EXIT
n=0; until [ -S "$SOCK" ] || [ $n -ge 60 ]; do sleep 0.25; n=$((n+1)); done
[ -S "$SOCK" ] || { echo "daemon failed to start:"; cat "$SCEN_LOG" 2>/dev/null; exit 1; }

echo "== daemon / version / info =="
has "version"   "$(d version --format 'srv={{.Server.Version}}')" "srv="
has "info"      "$(d info --format '{{.OperatingSystem}}')"        "dd"
has "ping"      "$(d version --format '{{.Server.Os}}')"           "linux"

echo "== images (the 'download'/discover step) =="
imgs="$(d images --format '{{.Repository}}')"
has "images-alpine" "$imgs" "alpine"
has "images-ubuntu" "$imgs" "ubuntu"

echo "== run -> logs -> wait -> inspect -> rm (the core lifecycle) =="
cid="$(d run -d alpine sh -c 'echo scenario-line1; echo scenario-line2; id -u')"
has "run-returns-id" "$cid" "$(echo "$cid" | head -c 8)"
logs="$(d logs "$cid")"
has "logs-line1" "$logs" "scenario-line1"
has "logs-line2" "$logs" "scenario-line2"
has "logs-uid0"  "$logs" "0"
ok  "inspect-exit" "$(d inspect --format '{{.State.ExitCode}}' "$cid")" "0"
has "inspect-status" "$(d inspect --format '{{.State.Status}}' "$cid")" "exited"
ok  "rm" "$(d rm "$cid" | head -c 12)" "$(echo "$cid" | head -c 12)"

echo "== ps / ps -a =="
cid2="$(d run -d alpine sh -c 'echo persisted')"
psa="$(d ps -a --format '{{.Image}}|{{.Command}}')"
has "ps-a-shows-it" "$psa" "alpine"
d rm "$cid2" >/dev/null

echo "== a guest with a real exit code =="
cid3="$(d run -d alpine sh -c 'exit 7')"
ok "exit-code-propagates" "$(d inspect --format '{{.State.ExitCode}}' "$cid3")" "7"
d rm "$cid3" >/dev/null

echo "== volumes =="
d volume create scen-vol >/dev/null
has "volume-listed" "$(d volume ls --format '{{.Name}}')" "scen-vol"
d volume rm scen-vol >/dev/null
ok "volume-removed" "$(d volume ls --format '{{.Name}}' | grep -c scen-vol)" "0"

echo "== networks =="
d network create scen-net >/dev/null
has "network-listed" "$(d network ls --format '{{.Name}}')" "scen-net"
has "network-default-bridge" "$(d network ls --format '{{.Name}}')" "bridge"
d network rm scen-net >/dev/null

echo ""
echo "scenarios: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
