#!/usr/bin/env bash
# Assemble a self-contained, ad-hoc-signed dd.app on macOS.
#
# Must run inside the GTK dev shell so the GTK build/runtime data and tools are on PATH:
#   nix develop "path:$PWD/nix" --command tools/bundle.sh
# (the Makefile `app` target does this for you).
#
# Produces build/dd.app with:
#   Contents/MacOS/dd-app                     the GTK GUI
#   Contents/Resources/dd-daemon, ddjit-*     the daemon + JIT engines (allow-jit signed)
#   Contents/Frameworks/*.dylib               the relocated GTK dylib graph (dylibbundler)
#   Contents/Frameworks/gdk-pixbuf-.../       svg+png loaders with a relative loaders.cache
#   Contents/Resources/glib-2.0/schemas/      compiled gschemas
#   Contents/Resources/icons/                 Adwaita + hicolor icon themes
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"   # script lives in dd-gui/package/, repo root is ../..
VERSION="${1:-0.1.0}"
APP="$ROOT/target/dd.app"   # under target/ (gitignored) — kept out of the repo root
C="$APP/Contents"; MACOS="$C/MacOS"; RES="$C/Resources"; FW="$C/Frameworks"
ENT="$ROOT/dd-jit/jit.entitlements"

log() { printf '\033[1;34m[bundle]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[bundle] %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(uname)" = "Darwin" ] || die "must run on macOS"
[ -n "${DD_GTK4:-}" ] || die "run inside the nix dev shell: nix develop \"path:$ROOT/nix\" --command tools/bundle.sh"
command -v dylibbundler >/dev/null || die "dylibbundler not found (nix dev shell)"

# 1. Release GUI binary. The daemon + ddjit-* engines are built OUTSIDE this dev shell (by the
#    Makefile `app` target) using the known-good native toolchain for the JIT's build.rs, since
#    the Nix stdenv clang may not find the macOS SDK headers the JIT C needs.
log "building release dd-app"
( cd "$ROOT" && cargo build --release -p dd-gui )
[ -f "$ROOT/target/release/dd-daemon" ] || die "target/release/dd-daemon missing — run 'cargo build --release -p dd-daemon' first (the Makefile 'app' target does this)"

# 2. Skeleton.
log "laying out bundle skeleton"
chmod -R u+w "$APP" 2>/dev/null || true   # nix-store copies are read-only; make removable
rm -rf "$APP"
mkdir -p "$MACOS" "$RES" "$FW"
cp "$ROOT/target/release/dd-app" "$MACOS/dd-app"
cp "$ROOT/target/release/dd-daemon" "$RES/dd-daemon"
for cli in dd ddcli; do # the CLI (whatever it's named) — installed to PATH from the GUI
  [ -f "$ROOT/target/release/$cli" ] && cp "$ROOT/target/release/$cli" "$RES/$cli"
done
printf 'APPL????' > "$C/PkgInfo"
sed "s/@VERSION@/$VERSION/g" "$ROOT/dd-gui/package/Info.plist.in" > "$C/Info.plist"
[ -f "$ROOT/dd-gui/package/dd-app.icns" ] && cp "$ROOT/dd-gui/package/dd-app.icns" "$RES/dd-app.icns" || true
[ -f "$ROOT/assets/logo.png" ] && cp "$ROOT/assets/logo.png" "$RES/logo.png" || true # onboarding logo
[ -d "$ROOT/assets/images" ] && cp -R "$ROOT/assets/images" "$RES/images" || true # bundled starter images (hello-dd)

# JIT engines, copied next to the daemon (found at runtime via DDJIT_DIR=Resources).
for t in linux_aarch64 linux_x86_64 darwin_aarch64; do
  f="$(find "$ROOT/target/release/build" -name "ddjit-$t" -type f 2>/dev/null | head -1)"
  if [ -n "$f" ]; then cp "$f" "$RES/ddjit-$t"; log "  + ddjit-$t"; else log "  ! ddjit-$t not built (skipping)"; fi
done

# 3. Stage gdk-pixbuf loaders (png from gdk-pixbuf, svg from librsvg) with a RELATIVE cache.
#    They live under Resources/ (NOT Frameworks/) so Frameworks stays a flat set of dylibs —
#    codesign refuses to seal a bundle whose Frameworks holds bare non-code directories. Their
#    own dependency paths are @executable_path-relative, so they still find Frameworks/*.dylib.
log "staging gdk-pixbuf loaders"
PIXVER="$(basename "$(ls -d "$DD_GDK_PIXBUF"/lib/gdk-pixbuf-2.0/*/ | head -1)")"
DEST_LOADERS="$RES/lib/gdk-pixbuf-2.0/$PIXVER/loaders"
mkdir -p "$DEST_LOADERS"
# Copy every gdk-pixbuf loader present (png/jpeg are built into core, so none is required for our
# UI; we ship what exists for completeness) plus librsvg's svg loader if this build provides one.
cp -L "$DD_GDK_PIXBUF"/lib/gdk-pixbuf-2.0/"$PIXVER"/loaders/*.so "$DEST_LOADERS"/ 2>/dev/null || true
find "$DD_LIBRSVG" -name libpixbufloader-svg.so -exec cp -L {} "$DEST_LOADERS"/ \; 2>/dev/null || true

shopt -s nullglob
LOADER_SOS=( "$DEST_LOADERS"/*.so )
shopt -u nullglob

# 4. Relocate the dylib graph: dd-app + each loader .so -> Contents/Frameworks.
log "relocating dylibs (dylibbundler)"
XARGS=( -x "$MACOS/dd-app" )
for so in "${LOADER_SOS[@]}"; do XARGS+=( -x "$so" ); done
dylibbundler -of -cd -b -d "$FW" -p '@executable_path/../Frameworks' "${XARGS[@]}" >/dev/null

# Regenerate loaders.cache with bare-filename (relative) module paths.
if [ ${#LOADER_SOS[@]} -gt 0 ]; then
  GDK_PIXBUF_MODULEDIR="$DEST_LOADERS" gdk-pixbuf-query-loaders "${LOADER_SOS[@]}" \
    | sed -E "s#\"$DEST_LOADERS/#\"#g" > "$DEST_LOADERS/loaders.cache"
else
  : > "$DEST_LOADERS/loaders.cache"
fi

# 5. Compile GSettings schemas (GTK aborts at startup without them).
log "compiling gsettings schemas"
SCHEMA_DEST="$RES/glib-2.0/schemas"
mkdir -p "$SCHEMA_DEST"
cp -L "$DD_GTK4"/share/glib-2.0/schemas/*.xml "$SCHEMA_DEST"/ 2>/dev/null || true
cp -L "$DD_GSETTINGS_SCHEMAS"/share/glib-2.0/schemas/*.xml "$SCHEMA_DEST"/ 2>/dev/null || true
glib-compile-schemas "$SCHEMA_DEST" >/dev/null

# 6. Icon themes (Adwaita symbolic icons used by the toolbar + hicolor fallback).
log "staging icon themes"
mkdir -p "$RES/icons"
cp -RL "$DD_ADWAITA_ICONS"/share/icons/Adwaita "$RES/icons"/ 2>/dev/null || true
cp -RL "$DD_HICOLOR_ICONS"/share/icons/hicolor "$RES/icons"/ 2>/dev/null || true
command -v gtk4-update-icon-cache >/dev/null && gtk4-update-icon-cache -q -f -t "$RES/icons/Adwaita" 2>/dev/null || true

# 7. Fontconfig.
mkdir -p "$RES/fontconfig"
cp "$ROOT/dd-gui/package/fonts.conf" "$RES/fontconfig/fonts.conf"

# 7b. Resolve the libiconv name-collision. nixpkgs ships TWO different libiconv.2.dylib: Apple's (exports
# _iconv, used by glib/gtk) and GNU's (exports _libiconv, used by libidn2/libunistring). dylibbundler
# collapses them into one file, so whichever consumer needs the other's symbols crashes at dyld time.
# Fix: keep Apple's as libiconv.2.dylib, and give the GNU consumers their own libiconv-gnu.2.dylib.
needgnu=0
for d in "$FW"/*.dylib "$FW"/*.so; do [ -f "$d" ] && nm -u "$d" 2>/dev/null | grep -qw _libiconv && { needgnu=1; break; }; done
if [ "$needgnu" = 1 ] && ! nm -gU "$FW/libiconv.2.dylib" 2>/dev/null | grep -qw _libiconv; then
  log "splitting Apple/GNU libiconv (dylibbundler name-collision)"
  # Ensure libiconv.2.dylib is the Apple one (has _iconv) for glib & co.
  if ! nm -gU "$FW/libiconv.2.dylib" 2>/dev/null | grep -qw _iconv; then
    for p in /nix/store/*libiconv*/lib/libiconv.2.dylib; do
      nm -gU "$p" 2>/dev/null | grep -qw _iconv && { cp -f "$p" "$FW/libiconv.2.dylib"; install_name_tool -id @executable_path/../Frameworks/libiconv.2.dylib "$FW/libiconv.2.dylib"; break; }
    done
  fi
  # Bundle a GNU libiconv (has _libiconv) under a distinct name + its libcharset.
  gnu=""; for p in /nix/store/*libiconv*/lib/libiconv.2.dylib; do nm -gU "$p" 2>/dev/null | grep -qw _libiconv && { gnu="$p"; break; }; done
  if [ -n "$gnu" ]; then
    cp -f "$gnu" "$FW/libiconv-gnu.2.dylib"; install_name_tool -id @executable_path/../Frameworks/libiconv-gnu.2.dylib "$FW/libiconv-gnu.2.dylib"
    gc=$(otool -L "$gnu" | awk '/libcharset/{print $1}' | grep '^/nix' | head -1 || true)
    if [ -n "$gc" ]; then
      cp -f "$(dirname "$gnu")/libcharset.1.dylib" "$FW/libcharset-gnu.1.dylib"
      install_name_tool -id @executable_path/../Frameworks/libcharset-gnu.1.dylib "$FW/libcharset-gnu.1.dylib"
      install_name_tool -change "$gc" @executable_path/../Frameworks/libcharset-gnu.1.dylib "$FW/libiconv-gnu.2.dylib"
    fi
    # Repoint every _libiconv consumer to the GNU copy.
    for d in "$FW"/*.dylib "$FW"/*.so; do
      [ -f "$d" ] || continue; [ "$(basename "$d")" = libiconv-gnu.2.dylib ] && continue
      if nm -u "$d" 2>/dev/null | grep -qw _libiconv; then
        ref=$(otool -L "$d" | awk '/Frameworks\/libiconv\.2\.dylib/{print $1}' | head -1)
        [ -n "$ref" ] && install_name_tool -change "$ref" @executable_path/../Frameworks/libiconv-gnu.2.dylib "$d"
      fi
    done
  fi
fi

# 8. Strip + codesign, deepest first (any later edit invalidates a signature).
#    DD_SIGN_ID unset/"-" = ad-hoc (default). A "Developer ID Application: …" identity name turns on real
#    signing with hardened runtime + secure timestamp; DD_SIGN_KEYCHAIN[/_PW] selects the keychain holding it.
#    The JIT engines + daemon keep the allow-jit entitlement ($ENT) so they run under the hardened runtime.
SIGN_ID="${DD_SIGN_ID:--}"
SIGN_FLAGS=""
if [ "$SIGN_ID" != "-" ]; then
  SIGN_FLAGS="--options runtime --timestamp"
  if [ -n "${DD_SIGN_KEYCHAIN:-}" ]; then
    security unlock-keychain ${DD_SIGN_KEYCHAIN_PW:+-p "$DD_SIGN_KEYCHAIN_PW"} "$DD_SIGN_KEYCHAIN" 2>/dev/null || true
    SIGN_FLAGS="$SIGN_FLAGS --keychain $DD_SIGN_KEYCHAIN"
  fi
  log "stripping + signing (Developer ID: $SIGN_ID)"
else
  log "stripping + signing (ad-hoc)"
fi
chmod -R u+w "$APP"   # data copied from the nix store is read-only; codesign needs write access
find "$FW" "$RES/lib" -type f \( -name '*.dylib' -o -name '*.so' \) -print0 2>/dev/null | while IFS= read -r -d '' f; do
  /usr/bin/strip -x "$f" 2>/dev/null || true
  codesign -s "$SIGN_ID" $SIGN_FLAGS -f "$f" >/dev/null 2>&1 || true
done
for b in dd-daemon ddjit-linux_aarch64 ddjit-linux_x86_64 ddjit-darwin_aarch64; do
  [ -f "$RES/$b" ] && codesign -s "$SIGN_ID" $SIGN_FLAGS -f --entitlements "$ENT" "$RES/$b" >/dev/null 2>&1 || true
done
for cli in dd ddcli; do [ -f "$RES/$cli" ] && codesign -s "$SIGN_ID" $SIGN_FLAGS -f "$RES/$cli" >/dev/null 2>&1 || true; done
codesign -s "$SIGN_ID" $SIGN_FLAGS -f "$MACOS/dd-app" >/dev/null 2>&1 || true
codesign -s "$SIGN_ID" $SIGN_FLAGS -f "$APP" >/dev/null 2>&1 || true   # outermost signed last
if [ "$SIGN_ID" != "-" ]; then
  codesign --verify --strict --verbose=2 "$APP" || { echo "[bundle] Developer ID signature verify FAILED" >&2; exit 1; }
  log "signed + verified ($(codesign -dv "$APP" 2>&1 | awk -F= '/^Authority/{print $2; exit}'))"
fi

SIZE="$(du -sh "$APP" | cut -f1)"
log "done -> $APP ($SIZE)"
