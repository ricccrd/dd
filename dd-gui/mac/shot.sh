#!/usr/bin/env bash
# Headless GUI verification: build dd-app + render one panel to a PNG, no interactive session.
# Runs on macOS in the nix GTK devshell. Usage (from the Mac, e.g. via OrbStack `mac`):
#
#   dd-gui/mac/shot.sh [view] [out.png]      view = home | containers | images | settings
#
# Reads the live daemon (so the shot shows real data). The PNG lands under target-mac/shots/ on the
# shared filesystem so it can be inspected from anywhere.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
VIEW="${1:-home}"
OUT="${2:-$REPO/target-mac/shots/$VIEW.png}"
mkdir -p "$(dirname "$OUT")"
cd "$REPO"
nix develop "path:$REPO/nix" --command bash -lc "
  CARGO_TARGET_DIR=target-mac DD_VERSION=0.0.0-dev cargo build -p dd-gui 2>&1 | tail -2
  GSK_RENDERER=cairo DD_SHOT='$OUT' DD_SHOT_VIEW='$VIEW' DD_SHOT_DELAY_MS=2600 ./target-mac/debug/dd-app 2>&1 | tail -3
"
echo "shot -> $OUT"
