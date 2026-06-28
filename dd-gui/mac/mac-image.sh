#!/usr/bin/env bash
# Build a native-arm64 macOS dev-container image for `ddcli mac`, from a nixpkgs toolset.
#
#   dd-gui/mac/mac-image.sh base            # lean userland  -> image name "ddmac-base"
#   dd-gui/mac/mac-image.sh dev             # full dev set   -> image name "ddmac-dev"   (default)
#   dd-gui/mac/mac-image.sh dev <out-dir>   # custom output dir (default: $DD_IMAGES/<name>)
#
# Produces a self-contained dd image directory:
#   <dir>/rootfs/profile/bin   the toolset on the in-jail PATH (symlinks into the packed /nix store)
#   <dir>/rootfs/nix/store     the packed nixpkgs closure (so the image is portable to a nix host)
#   <dir>/rootfs/etc/...       passwd / shells / ssl certs / profile
#   <dir>/dd-image.json        {name, os:darwin, cmd:[bash], env, workdir} -> the daemon registers it
#
# MUST run on macOS (aarch64-darwin) with nix installed (the toolset is realized from `nix/flake.nix`).
# Push with `make mac-push` (tags into huttarichard/ddmac:{base,dev,latest}).
#
# Portability note: the packed binaries reference their dylibs by absolute /nix/store path, resolved
# by dyld against the *host* store. The target machine therefore needs those store paths present —
# trivially true on the build host; on another machine, `nix copy` the closure (or run the build there).
set -euo pipefail

VARIANT="${1:-dev}"
case "$VARIANT" in base|dev) ;; *) echo "usage: $0 [base|dev] [out-dir]" >&2; exit 2;; esac
NAME="ddmac-$VARIANT"

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
DIR="${2:-${DD_IMAGES:-$REPO/target/images}/$NAME}"
ROOT="$DIR/rootfs"

command -v nix >/dev/null || { echo "nix not found — install nix on this Mac first" >&2; exit 1; }
[ "$(uname -s)" = Darwin ] || echo "warning: not macOS — the produced image only runs under darwinjail on macOS" >&2

echo "==> realizing the '$VARIANT' toolset from nix/flake.nix (this can take a while on first build)…"
PROFILE="$(nix build --no-link --print-out-paths "$REPO/nix#mac-$VARIANT")"
echo "    profile: $PROFILE"

echo "==> assembling rootfs at $ROOT"
# The packed nix-store copies are read-only, so make them writable before removing a prior build.
[ -e "$DIR" ] && { chmod -R u+w "$DIR" 2>/dev/null || true; rm -rf "$DIR"; }
mkdir -p "$ROOT"/etc/ssl/certs "$ROOT"/{tmp,root,opt,home,Users,nix/store} "$ROOT/profile"

# Toolset on the in-jail PATH: copy the profile's bin symlinks (they target /nix/store/.../bin/*).
cp -R "$PROFILE/bin" "$ROOT/profile/bin"

# Pack the full closure so the image carries every dylib/runtime it needs.
echo "==> packing nix closure (cp --clone where possible)…"
CP="cp -R"; cp -c /dev/null "$DIR/.cptest" 2>/dev/null && CP="cp -Rc"; rm -f "$DIR/.cptest"
while IFS= read -r p; do
  d="$ROOT/nix/store/$(basename "$p")"
  [ -e "$d" ] || $CP "$p" "$ROOT/nix/store/" 2>/dev/null || cp -R "$p" "$ROOT/nix/store/"
done < <(nix-store -qR "$PROFILE")
echo "    packed: $(du -sh "$ROOT/nix/store" 2>/dev/null | cut -f1)"

# TLS roots so curl/git/openssh work out of the box (cacert is in the closure).
CACERT="$(nix build --no-link --print-out-paths "$REPO/nix#mac-$VARIANT" >/dev/null 2>&1; \
         find "$ROOT/nix/store" -maxdepth 2 -name ca-bundle.crt 2>/dev/null | head -1)"
if [ -n "${CACERT:-}" ]; then
  cp "$CACERT" "$ROOT/etc/ssl/certs/ca-certificates.crt"
  cp "$CACERT" "$ROOT/etc/ssl/cert.pem"   # the path LibreSSL/curl probe by default
fi

# Minimal /etc so a login feels like a real account.
printf 'root:*:0:0:System Administrator:/root:/profile/bin/bash\n' > "$ROOT/etc/passwd"
printf 'wheel:*:0:root\n'                                          > "$ROOT/etc/group"
printf 'maccontainer\n'                                            > "$ROOT/etc/hostname"
printf '/profile/bin/bash\n/profile/bin/zsh\n/profile/bin/fish\n'  > "$ROOT/etc/shells"
cat > "$ROOT/etc/profile" <<'PROF'
export PATH=/profile/bin:/usr/bin:/bin:/usr/sbin:/sbin
export HOME=${HOME:-/root}
export SSL_CERT_FILE=${SSL_CERT_FILE:-/etc/ssl/cert.pem}
export NIX_SSL_CERT_FILE=${NIX_SSL_CERT_FILE:-/etc/ssl/cert.pem}
export LANG=${LANG:-en_US.UTF-8}
export TERM=${TERM:-xterm-256color}
export PS1='\[\e[1;32m\]ddmac\[\e[0m\]:\w\$ '
PROF
cp "$ROOT/etc/profile" "$ROOT/etc/zshenv"

# Image metadata the daemon reads (name/os/cmd/env/workdir round-trip on local discovery; on a pulled
# image the daemon re-derives os:darwin from the Mach-O probe and defaults the shell to bash).
cat > "$DIR/dd-image.json" <<JSON
{"name":"$NAME","os":"darwin","cmd":["bash"],"workdir":"/root",
 "env":["PATH=/profile/bin:/usr/bin:/bin","HOME=/root","SSL_CERT_FILE=/etc/ssl/cert.pem",
        "NIX_SSL_CERT_FILE=/etc/ssl/cert.pem","LANG=en_US.UTF-8","TERM=xterm-256color"]}
JSON

echo "==> done: $NAME"
echo "    tools: $(ls "$ROOT/profile/bin" | wc -l | tr -d ' ') on PATH (/profile/bin)"
echo "    image: $DIR"
echo "    try:   DD_IMAGES=$(dirname "$DIR") ddcli mac      (or: make mac-push to publish)"
