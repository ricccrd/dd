#!/usr/bin/env bash
# dd-tests/scenarios/compose.sh -- end-to-end Docker Compose scenarios against dd-daemon.
#
# Compose is the real stress test of the Engine API: one `compose up` drives image pulls, a project
# network (create + connect), multi-container create/start with labels, label-filtered `ps`, log
# multiplexing, `exec`, and an orderly `down` (stop + remove containers + remove the network). It
# exercises far more of the API surface, and in a more demanding order, than the plain CLI does.
#
#   bash dd-tests/scenarios/compose.sh
#
# Requires the compose v2 plugin (`docker compose`) or the v1 binary (`docker-compose`). If neither is
# installed this self-skips with rc=0 (so CI on a compose-less host stays green). Run after `make jit`.
#
# Env: DD_IMAGES (image dir, default poc/images), DD_DAEMON (daemon binary, default target/release/dd-daemon).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
IMAGES="${DD_IMAGES:-/Users/x/dd/poc/images}"
DAEMON="${DD_DAEMON:-$ROOT/target/release/dd-daemon}"
SOCK="$ROOT/dd-compose.sock"
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
no()  { if echo "$2" | grep -qF "$3"; then echo "  FAIL $1: [$2] still has [$3]"; fail=$((fail+1)); else echo "  ok   $1"; pass=$((pass+1)); fi; }

# ---- bring up the daemon on a private socket ----
SCEN_LOG="$ROOT/dd-compose.log"
pkill -x dd-daemon 2>/dev/null
rm -f "$SOCK"
# Fully isolate this daemon: private socket AND private state/volumes (a fresh per-scenario temp
# dir), so it starts from empty state and never reads or mutates the developer's real ~/.dd.
STATE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dd-compose.XXXXXX")"
export DD_IMAGES="$IMAGES" DDOCKERD_SOCK="$SOCK" DD_STATE="$STATE_DIR/state.json" DD_VOLUMES="$STATE_DIR/volumes"
"$DAEMON" >"$SCEN_LOG" 2>&1 &
DPID=$!
PROJ="ddcompose"
WORK="$ROOT/scen-compose"
cleanup() {
    COMPOSE -p "$PROJ" -f "$WORK/compose.yml" down -v --remove-orphans >/dev/null 2>&1
    kill -9 $DPID 2>/dev/null
    rm -rf "$SOCK" "$SCEN_LOG" "$WORK" "$STATE_DIR"
}
trap cleanup EXIT
n=0; until [ -S "$SOCK" ] || [ $n -ge 60 ]; do sleep 0.25; n=$((n+1)); done
[ -S "$SOCK" ] || { echo "daemon failed to start:"; cat "$SCEN_LOG" 2>/dev/null; exit 1; }

# ---- write a small two-service project ----
# `api`  : a long-lived service that writes a marker to a named volume and idles.
# `worker`: a one-shot service (depends_on api) that prints a marker and exits.
# A custom network + a named volume exercise the network/volume API paths compose drives.
rm -rf "$WORK"; mkdir -p "$WORK"
cat > "$WORK/compose.yml" <<'YML'
services:
  api:
    image: alpine
    command: ["sh","-c","echo api-up > /data/marker; echo api-listening; sleep 30"]
    environment:
      - ROLE=api
    volumes:
      - shared:/data
    networks:
      - appnet
  worker:
    image: alpine
    command: ["sh","-c","echo worker-ran; echo role=$$ROLE"]
    environment:
      - ROLE=worker
    depends_on:
      - api
    networks:
      - appnet
volumes:
  shared:
networks:
  appnet:
YML

echo "== compose config (parse the project) =="
has "config-services" "$(COMPOSE -p "$PROJ" -f "$WORK/compose.yml" config --services 2>&1)" "api"

echo "== compose up -d (pull/network/volume/create/start the whole project) =="
upout="$(COMPOSE -p "$PROJ" -f "$WORK/compose.yml" up -d 2>&1)"
has "up-ran" "$upout$(COMPOSE -p "$PROJ" -f "$WORK/compose.yml" ps 2>&1)" "api"

echo "== compose ps (label-filtered container list) =="
psout="$(COMPOSE -p "$PROJ" -f "$WORK/compose.yml" ps --format '{{.Service}}' 2>&1)"
has "ps-shows-api" "$psout" "api"

echo "== compose logs (multiplexed across services) =="
sleep 1
logs="$(COMPOSE -p "$PROJ" -f "$WORK/compose.yml" logs 2>&1)"
has "logs-api-marker"    "$logs" "api-listening"
has "logs-worker-marker" "$logs" "worker-ran"
has "logs-env-interp"    "$logs" "role=worker"

echo "== compose exec (into the running api service) =="
has "exec-into-api" "$(COMPOSE -p "$PROJ" -f "$WORK/compose.yml" exec -T api cat /data/marker 2>&1)" "api-up"

echo "== project network + volume were created =="
has "network-created" "$(docker network ls --format '{{.Name}}' 2>&1)" "${PROJ}_appnet"
has "volume-created"  "$(docker volume ls  --format '{{.Name}}' 2>&1)" "${PROJ}_shared"

echo "== compose down -v (stop + remove containers, network, volume) =="
COMPOSE -p "$PROJ" -f "$WORK/compose.yml" down -v >/dev/null 2>&1
no "down-removed-containers" "$(docker ps -a --format '{{.Names}}' 2>&1)" "${PROJ}-api"
no "down-removed-network"    "$(docker network ls --format '{{.Name}}' 2>&1)" "${PROJ}_appnet"

echo ""
echo "compose scenarios: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
