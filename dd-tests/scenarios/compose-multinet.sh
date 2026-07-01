#!/usr/bin/env bash
# dd-tests/scenarios/compose-multinet.sh -- multi-network Docker Compose against dd-daemon.
#
# The plain `compose.sh` scenario runs a single project network, so a container only ever needs the
# primary network named by HostConfig.NetworkMode. Compose v2, however, attaches a service to EVERY
# network listed under its `networks:` via the top-level NetworkingConfig.EndpointsConfig of
# `POST /containers/create` (NetworkMode only carries the *first*). This scenario stands one service on
# TWO project networks and asserts the container shows up in BOTH, exercising the EndpointsConfig path.
#
#   bash dd-tests/scenarios/compose-multinet.sh
#
# Self-skips with rc=0 when neither `docker compose` nor `docker-compose` is installed. Run after `make jit`.
# Env: DD_IMAGES (image dir, default poc/images), DD_DAEMON (daemon binary, default target/release/dd-daemon).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
IMAGES="${DD_IMAGES:-/Users/x/dd/poc/images}"
DAEMON="${DD_DAEMON:-$ROOT/target/release/dd-daemon}"
SOCK="$ROOT/dd-compose-mnet.sock"
export DOCKER_HOST="unix://$SOCK"

# ---- pick a compose driver (v2 plugin preferred, v1 fallback) ----
if docker compose version >/dev/null 2>&1; then
    COMPOSE() { docker compose "$@"; }
elif command -v docker-compose >/dev/null 2>&1; then
    COMPOSE() { docker-compose "$@"; }
else
    echo "SKIP: docker compose / docker-compose not installed on this host"
    exit 0
fi

pass=0 fail=0
has() { if echo "$2" | grep -qF "$3"; then echo "  ok   $1"; pass=$((pass+1)); else echo "  FAIL $1: [$2] lacks [$3]"; fail=$((fail+1)); fi; }

# ---- bring up an isolated daemon (private socket + private state/volumes) ----
SCEN_LOG="$ROOT/dd-compose-mnet.log"
pkill -x dd-daemon 2>/dev/null
rm -f "$SOCK"
STATE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dd-compose-mnet.XXXXXX")"
export DD_IMAGES="$IMAGES" DDOCKERD_SOCK="$SOCK" DD_STATE="$STATE_DIR/state.json" DD_VOLUMES="$STATE_DIR/volumes"
"$DAEMON" >"$SCEN_LOG" 2>&1 &
DPID=$!
PROJ="ddmnet"
WORK="$ROOT/scen-compose-mnet"
cleanup() {
    COMPOSE -p "$PROJ" -f "$WORK/compose.yml" down -v --remove-orphans >/dev/null 2>&1
    kill -9 $DPID 2>/dev/null
    rm -rf "$SOCK" "$SCEN_LOG" "$WORK" "$STATE_DIR"
}
trap cleanup EXIT
n=0; until [ -S "$SOCK" ] || [ $n -ge 60 ]; do sleep 0.25; n=$((n+1)); done
[ -S "$SOCK" ] || { echo "daemon failed to start:"; cat "$SCEN_LOG" 2>/dev/null; exit 1; }

# ---- one service straddling two networks ----
rm -rf "$WORK"; mkdir -p "$WORK"
cat > "$WORK/compose.yml" <<'YML'
services:
  app:
    image: alpine
    command: ["sh","-c","echo app-up; sleep 30"]
    networks:
      - front
      - back
networks:
  front:
  back:
YML

echo "== compose up -d (service on two networks) =="
upout="$(COMPOSE -p "$PROJ" -f "$WORK/compose.yml" up -d 2>&1)"
has "up-ran" "$upout$(COMPOSE -p "$PROJ" -f "$WORK/compose.yml" ps 2>&1)" "app"

echo "== both project networks exist =="
nets="$(docker network ls --format '{{.Name}}' 2>&1)"
has "front-network-created" "$nets" "${PROJ}_front"
has "back-network-created"  "$nets" "${PROJ}_back"

echo "== the app container is attached to BOTH networks (EndpointsConfig path) =="
fmt='{{range .Containers}}{{.Name}} {{end}}'
has "app-on-front" "$(docker network inspect "${PROJ}_front" --format "$fmt" 2>&1)" "${PROJ}-app"
has "app-on-back"  "$(docker network inspect "${PROJ}_back"  --format "$fmt" 2>&1)" "${PROJ}-app"

echo "== compose down -v (teardown) =="
COMPOSE -p "$PROJ" -f "$WORK/compose.yml" down -v >/dev/null 2>&1

echo ""
echo "compose multinet scenarios: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
