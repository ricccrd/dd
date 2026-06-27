#!/usr/bin/env bash
# Build a distributable .dmg from build/dd-app.app. Run after tools/bundle.sh.
#   nix develop "path:$PWD/nix" --command tools/make-dmg.sh   (Makefile `dmg` target)
#
# The .dmg is unsigned/ad-hoc like the app: on first launch users must right-click -> Open,
# or run `xattr -dr com.apple.quarantine /Applications/dd-app.app` (also printed by `dd doctor`).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${1:-0.1.0}"
APP="$ROOT/target/dd-app.app"
ARCH="$(uname -m)"
DIST="$ROOT/target/dist"
OUT="$DIST/dd-$VERSION-$ARCH.dmg"

[ -d "$APP" ] || { echo "missing $APP — run tools/bundle.sh first" >&2; exit 1; }
mkdir -p "$DIST"
rm -f "$OUT" "$DIST"/rw.*.dmg

if command -v create-dmg >/dev/null; then
  # create-dmg returns non-zero if it can't set the (cosmetic) window layout; tolerate that.
  create-dmg \
    --volname "dd $VERSION" \
    --window-pos 200 120 --window-size 640 400 --icon-size 120 \
    --icon "dd-app.app" 160 200 \
    --app-drop-link 480 200 \
    --hide-extension "dd-app.app" \
    --no-internet-enable \
    "$OUT" "$APP" || true
fi

# Fallback (or if create-dmg produced nothing): a plain compressed image with an Applications link.
if [ ! -f "$OUT" ]; then
  STAGE="$(mktemp -d)"
  cp -R "$APP" "$STAGE/"
  ln -s /Applications "$STAGE/Applications"
  hdiutil create -volname "dd $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$OUT" >/dev/null
  rm -rf "$STAGE"
fi

# create-dmg can leave a read-write scratch image behind; tidy it up.
rm -f "$DIST"/rw.*.dmg
# Publish a checksum alongside the dmg (handy for release notes).
( cd "$DIST" && shasum -a 256 "$(basename "$OUT")" > "$(basename "$OUT").sha256" )
echo "[make-dmg] done -> $OUT ($(du -sh "$OUT" | cut -f1))"
