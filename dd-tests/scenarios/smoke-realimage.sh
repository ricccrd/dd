#!/usr/bin/env bash
# Real-image smoke test — the USER's path, the one that shipped broken.
#
# The matrix + the other scenarios run against CURATED, pre-staged rootfs fixtures
# (DD_IMAGES=poc/images) and statically-linked guests (gcc -static-pie). None of that
# exercises a FRESH PULL of a real, current Docker Hub image whose `/bin/echo`,`/bin/bash`
# load `libc.so.6` through the real glibc dynamic linker (ld.so) — which is exactly where
# `ddcli run ubuntu` died on a fresh Mac ("libc.so.6: failed to map segment from shared object").
#
# This test reproduces that from the user's perspective: for BOTH arches, do a fresh pull of a
# real glibc distro and run a dynamically-linked binary. It needs network (real registry pull),
# so it runs in CI (the .github/workflows/smoke.yml macOS runner), not the offline unit matrix.
#
#   bash dd-tests/scenarios/smoke-realimage.sh
#
# Env: DDCLI (default target/release/ddcli). Build it first: cargo build --release -p dd-cli.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
DDCLI="${DDCLI:-$ROOT/target/release/ddcli}"
[ -x "$DDCLI" ] || { echo "ddcli not built: $DDCLI (run: cargo build --release -p dd-cli)" >&2; exit 1; }

pass=0 fail=0

# run <platform> <image> <marker>
#   Fresh empty DD_IMAGES forces a real Docker Hub pull; runs the real /bin/echo (glibc,
#   dynamically linked -> ld.so maps libc.so.6) and asserts the marker came back.
#   Retries only on transient network/DNS blips so the test fails on the REAL bug, not the wifi.
run() {
  local plat="$1" img="$2" marker="$3" D out rc t
  for t in 1 2 3 4; do
    D="$(mktemp -d)"
    out="$(DD_IMAGES="$D" "$DDCLI" run --platform "$plat" "$img" /bin/echo "$marker" 2>&1)"; rc=$?
    rm -rf "$D"
    echo "$out" | grep -qiE "resolve host|could not resolve|timed out|connection reset|i/o timeout|TLS handshake" || break
    echo "  ..   $plat $img: transient pull error, retry $t"; sleep 8
  done
  if echo "$out" | grep -qF "$marker"; then
    echo "  ok   $plat $img  (fresh pull + glibc dynamic-load ran)"; pass=$((pass+1))
  else
    echo "  FAIL $plat $img  (rc=$rc) — want [$marker], got:"; echo "$out" | tail -10 | sed 's/^/         /'; fail=$((fail+1))
  fi
}

echo "== real-image smoke: fresh pull + run a dynamically-linked glibc binary, both arches =="
# linux/arm64 is the host arch (aarch64 engine); linux/amd64 forces the x86_64 -> arm64 JIT.
# A real distro's /bin/echo is glibc-dynamic, so this is the libc.so.6 / ld.so path end-to-end.
run linux/arm64 ubuntu SMOKE-UBUNTU-ARM64
run linux/amd64 ubuntu SMOKE-UBUNTU-AMD64
run linux/arm64 debian SMOKE-DEBIAN-ARM64
run linux/amd64 debian SMOKE-DEBIAN-AMD64

echo ""
echo "smoke-realimage: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
