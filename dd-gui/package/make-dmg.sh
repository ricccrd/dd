#!/usr/bin/env bash
# Build a distributable .dmg from build/dd.app. Run after tools/bundle.sh.
#   nix develop "path:$PWD/nix" --command tools/make-dmg.sh   (Makefile `dmg` target)
#
# The .dmg is unsigned/ad-hoc like the app: on first launch users must right-click -> Open,
# or run `xattr -dr com.apple.quarantine /Applications/dd.app` (also printed by `dd doctor`).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"   # script lives in dd-gui/package/, repo root is ../..
VERSION="${1:-0.1.0}"
APP="$ROOT/target/dd.app"
ARCH="$(uname -m)"
DIST="$ROOT/target/dist"
OUT="$DIST/dd.dmg"

[ -d "$APP" ] || { echo "missing $APP — run tools/bundle.sh first" >&2; exit 1; }
mkdir -p "$DIST"
rm -f "$OUT" "$DIST"/rw.*.dmg

if command -v create-dmg >/dev/null; then
  # create-dmg returns non-zero if it can't set the (cosmetic) window layout; tolerate that.
  create-dmg \
    --volname "dd $VERSION" \
    --window-pos 200 120 --window-size 640 400 --icon-size 120 \
    --icon "dd.app" 160 200 \
    --app-drop-link 480 200 \
    --hide-extension "dd.app" \
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

# Sign + notarize + staple when notarization creds are configured — either a notarytool keychain profile
# (DD_NOTARY_PROFILE, e.g. "dd-notary", local) OR inline App Store Connect creds (DD_NOTARY_APPLE_ID +
# DD_NOTARY_TEAM_ID + DD_NOTARY_PW, used by CI from secrets). The app inside must already be Developer
# ID-signed with hardened runtime (tools/bundle.sh + DD_SIGN_ID).
if [ -n "${DD_NOTARY_PROFILE:-}" ] || [ -n "${DD_NOTARY_APPLE_ID:-}" ]; then
  if [ -n "${DD_SIGN_ID:-}" ] && [ "${DD_SIGN_ID}" != "-" ]; then
    KCFLAG=""
    [ -n "${DD_SIGN_KEYCHAIN:-}" ] && { security unlock-keychain ${DD_SIGN_KEYCHAIN_PW:+-p "$DD_SIGN_KEYCHAIN_PW"} "$DD_SIGN_KEYCHAIN" 2>/dev/null || true; KCFLAG="--keychain $DD_SIGN_KEYCHAIN"; }
    codesign -s "$DD_SIGN_ID" --timestamp $KCFLAG -f "$OUT"
  fi
  if [ -n "${DD_NOTARY_APPLE_ID:-}" ]; then
    NAUTH=(--apple-id "$DD_NOTARY_APPLE_ID" --team-id "$DD_NOTARY_TEAM_ID" --password "$DD_NOTARY_PW")
  else
    NAUTH=(--keychain-profile "$DD_NOTARY_PROFILE")
  fi
  echo "[make-dmg] notarizing $OUT — Apple round-trip, ~1-5 min..."
  # Apple's /usr/bin/xcrun, NOT the nix-shell xcrun (which can't resolve notarytool/stapler).
  # Note: notarytool uploads to Apple S3 and rejects >15-min clock skew (RequestTimeTooSkewed) — keep the host clock synced.
  /usr/bin/xcrun notarytool submit "$OUT" "${NAUTH[@]}" --wait
  /usr/bin/xcrun stapler staple "$OUT"
  /usr/bin/xcrun stapler validate "$OUT"
  echo "[make-dmg] notarized + stapled"
fi

# Publish a checksum alongside the dmg (handy for release notes).
( cd "$DIST" && shasum -a 256 "$(basename "$OUT")" > "$(basename "$OUT").sha256" )
echo "[make-dmg] done -> $OUT ($(du -sh "$OUT" | cut -f1))"
