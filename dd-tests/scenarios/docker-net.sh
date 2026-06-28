#!/usr/bin/env bash
# dd-tests/scenarios/docker-net.sh -- container-to-container networking against dd-daemon.
#
# The everyday multi-container case: two containers on the same user-defined network must reach each
# other — by container NAME (Docker's embedded DNS) and by IP — while a container on a DIFFERENT network
# stays isolated. This is what `docker compose` services, a web+db pair, etc. all rely on.
#
# NOTE: dd currently models networking as a per-container loopback netns with NO shared bridge (see
# docs/PLAN.md "Networking Phase-2b" + the Docker-API `inspect .NetworkSettings` gap), so these tests
# are expected to FAIL today — they're the executable spec for that work, and will go green when a real
# bridge/IPAM + embedded DNS land. A failure here is a known gap, not a regression.
#
#   bash dd-tests/scenarios/docker-net.sh        # run after `make jit`
# Env: DD_IMAGES, DD_DAEMON.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
IMAGES="${DD_IMAGES:-/Users/x/dd/poc/images}"
DAEMON="${DD_DAEMON:-$ROOT/target/release/dd-daemon}"
SOCK="$ROOT/dd-net.sock"
export DOCKER_HOST="unix://$SOCK"

pass=0 fail=0
has() { if echo "$2" | grep -qF "$3"; then echo "  ok   $1"; pass=$((pass+1)); else echo "  FAIL $1: [$(echo "$2"|tr '\n' '|'|head -c160)] lacks [$3]"; fail=$((fail+1)); fi; }
no()  { if echo "$2" | grep -qF "$3"; then echo "  FAIL $1: [$2] unexpectedly has [$3]"; fail=$((fail+1)); else echo "  ok   $1"; pass=$((pass+1)); fi; }
d()   { docker "$@" 2>&1; }

SCEN_LOG="$ROOT/dd-net.log"
pkill -x dd-daemon 2>/dev/null; rm -f "$SOCK"
# Fully isolate this daemon: private socket AND private state/volumes (a fresh per-scenario temp
# dir), so it starts from empty state and never reads or mutates the developer's real ~/.dd.
STATE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dd-net.XXXXXX")"
export DD_IMAGES="$IMAGES" DDOCKERD_SOCK="$SOCK" DD_STATE="$STATE_DIR/state.json" DD_VOLUMES="$STATE_DIR/volumes"
"$DAEMON" >"$SCEN_LOG" 2>&1 &
DPID=$!
cleanup() { docker rm -f net-srv net-cli net-other >/dev/null 2>&1; docker network rm ddnet ddnet2 >/dev/null 2>&1; kill -9 $DPID 2>/dev/null; rm -f "$SOCK" "$SCEN_LOG"; rm -rf "$STATE_DIR"; }
trap cleanup EXIT
n=0; until [ -S "$SOCK" ] || [ $n -ge 60 ]; do sleep 0.25; n=$((n+1)); done
[ -S "$SOCK" ] || { echo "daemon failed to start:"; cat "$SCEN_LOG" 2>/dev/null; exit 1; }

echo "== a user-defined network + a named server container joined to it =="
d network create ddnet >/dev/null 2>&1
has "network-created" "$(d network ls --format '{{.Name}}')" "ddnet"
# echo server: busybox nc, one connection at a time, echoes input back via /bin/cat.
srv=$(d run -d --network ddnet --name net-srv alpine sh -c 'while true; do nc -l -p 8080 -e /bin/cat; done')
sleep 1
has "server-running" "$(d ps --format '{{.Names}}')" "net-srv"

echo "== the server has an IP on the network (inspect .NetworkSettings) =="
ip="$(d inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}} {{end}}' net-srv 2>&1 | tr -d ' ')"
has "server-has-ip" "${ip:-<none>}" "."   # any non-empty IP-ish string
has "network-inspect-lists-member" "$(d network inspect ddnet 2>&1)" "net-srv"

echo "== container -> container by NAME (embedded DNS) =="
out="$(timeout 20 docker run --rm --network ddnet alpine sh -c 'echo by-name-msg | nc -w3 net-srv 8080' 2>&1)"
has "reach-by-name" "$out" "by-name-msg"

echo "== container -> container by IP =="
if echo "$ip" | grep -qE '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+'; then
    out="$(timeout 20 docker run --rm --network ddnet alpine sh -c "echo by-ip-msg | nc -w3 $ip 8080" 2>&1)"
    has "reach-by-ip" "$out" "by-ip-msg"
else
    echo "  FAIL reach-by-ip: no server IP to dial"; fail=$((fail+1))
fi

echo "== isolation: a container on a DIFFERENT network must NOT reach the server =="
d network create ddnet2 >/dev/null 2>&1
out="$(timeout 20 docker run --rm --network ddnet2 alpine sh -c 'echo cross | nc -w3 net-srv 8080; echo rc=$?' 2>&1)"
no "cross-network-isolated" "$out" "cross"   # the echo must NOT come back across networks

d rm -f net-srv >/dev/null 2>&1; d network rm ddnet ddnet2 >/dev/null 2>&1
echo ""
echo "docker-net: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
