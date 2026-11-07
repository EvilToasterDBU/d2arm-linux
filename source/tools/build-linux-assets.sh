#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${D2_LINUX_ASSETS_DIR:-$REPO_DIR/linux/assets}"
CACHE_DIR="${D2_CACHE_DIR:-$HOME/.cache/d2android}"
D2DX_URL="https://github.com/bolrog/d2dx/releases/download/0.99.529/D2DX.zip"
D2DX_SHA256="94241eaf97a7eba6800370a65477ed08f91e53da01d18b253b96cf44f506efa9"
D2DX_ZIP="$CACHE_DIR/D2DX-0.99.529.zip"

mkdir -p "$OUT_DIR" "$CACHE_DIR"
mkdir -p "$OUT_DIR/licenses/d2dx"
D2_MOD_OUT="$OUT_DIR/mod" D2_INSTALL_OUT="$OUT_DIR/install" bash "$REPO_DIR/tools/build-mod.sh"

if [ ! -s "$D2DX_ZIP" ] || ! echo "$D2DX_SHA256  $D2DX_ZIP" | sha256sum -c --status; then
  curl --fail --location --retry 3 "$D2DX_URL" --output "$D2DX_ZIP.tmp"
  echo "$D2DX_SHA256  $D2DX_ZIP.tmp" | sha256sum -c --status || { rm -f "$D2DX_ZIP.tmp"; echo "D2DX checksum mismatch" >&2; exit 1; }
  mv "$D2DX_ZIP.tmp" "$D2DX_ZIP"
fi

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
unzip -q -j "$D2DX_ZIP" glide3x.dll -d "$tmp"
unzip -q -o -j "$D2DX_ZIP" README.md 'licenses/*' -d "$OUT_DIR/licenses/d2dx"
python3 -I "$REPO_DIR/tools/binpatch.py" d2dx-glide3x "$tmp/glide3x.dll"
python3 -I "$REPO_DIR/tools/binpatch.py" d2dx-glide3x-frametime "$tmp/glide3x.dll"
cp "$tmp/glide3x.dll" "$OUT_DIR/glide3x.dll"
for layers in 512 1024; do
  cp "$tmp/glide3x.dll" "$OUT_DIR/glide3x-atlas$layers.dll"
  python3 -I "$REPO_DIR/tools/binpatch.py" "d2dx-glide3x-atlas$layers" "$OUT_DIR/glide3x-atlas$layers.dll"
done
cat > "$OUT_DIR/d2dx.cfg" <<'EOF'
# d2arm-linux: D2DX will use the current desktop resolution.
[window]
scale=1
position=[-1,-1]
frameless=false

[game]
size=[-1,-1]
filtering=0

[optouts]
nologo=true
nocompatmodefix=true
EOF
echo "Linux assets written to $OUT_DIR"
