#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LINUX_DIR="$(cd "$SCRIPT_DIR/../linux" && pwd)"
ASSET_DIR="$LINUX_DIR/assets/runtime"
WORK_DIR="${D2_RUNTIME_BUILD_DIR:-$LINUX_DIR/build/runtime}"
BOX64_VERSION="v0.4.4"
WINE_VERSION="10.0"
WINE_ARCHIVE="wine-${WINE_VERSION}-amd64-wow64.tar.xz"
WINE_SHA256="aeebbbf239e548f0136f1cd72a2e109dd9a572a8f703da3c09fa40d74d5c255f"

for tool in cmake git curl tar sha256sum; do
  command -v "$tool" >/dev/null || { echo "Missing runtime build tool: $tool" >&2; exit 1; }
done

rm -rf "$WORK_DIR" "$ASSET_DIR"
mkdir -p "$WORK_DIR" "$ASSET_DIR/box64" "$ASSET_DIR/wine"

git clone --depth 1 --branch "$BOX64_VERSION" https://github.com/ptitSeb/box64.git "$WORK_DIR/box64-src"
cmake -S "$WORK_DIR/box64-src" -B "$WORK_DIR/box64-build" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/ -DARM64=ON -DARM_DYNAREC=ON -DNO_CONF_INSTALL=ON
cmake --build "$WORK_DIR/box64-build" --parallel
DESTDIR="$ASSET_DIR/box64" cmake --install "$WORK_DIR/box64-build"

curl -fL "https://github.com/Kron4ek/Wine-Builds/releases/download/${WINE_VERSION}/${WINE_ARCHIVE}" \
  -o "$WORK_DIR/$WINE_ARCHIVE"
echo "$WINE_SHA256  $WORK_DIR/$WINE_ARCHIVE" | sha256sum --check --status || {
  echo "Wine archive SHA-256 mismatch" >&2
  exit 1
}
mkdir -p "$WORK_DIR/wine-unpack"
tar -xJf "$WORK_DIR/$WINE_ARCHIVE" -C "$WORK_DIR/wine-unpack"
WINE_BIN="$(find "$WORK_DIR/wine-unpack" -path '*/bin/wine' -print -quit)"
[ -n "$WINE_BIN" ] || { echo "Wine WoW64 archive does not contain bin/wine" >&2; exit 1; }
WINE_ROOT="$(dirname "$(dirname "$WINE_BIN")")"
cp -a "$WINE_ROOT/." "$ASSET_DIR/wine/"
cp "$WORK_DIR/box64-src/LICENSE" "$ASSET_DIR/BOX64-LICENSE.txt"
cat > "$ASSET_DIR/THIRD-PARTY.txt" <<EOF
Bundled offline runtime for d2arm-linux

Box64 $BOX64_VERSION: https://github.com/ptitSeb/box64
License: MIT; see BOX64-LICENSE.txt.

Wine $WINE_VERSION x86_64 WoW64 build: https://github.com/Kron4ek/Wine-Builds
The Wine build is distributed under Wine's LGPL license. Source and build details:
https://github.com/Kron4ek/Wine-Builds/tree/master

The AppImage bundles these runtimes so the target ARM64 device does not need to
download or install Box64 or Wine. Box64 still uses the host ARM64 Linux ABI.
EOF

[ -x "$ASSET_DIR/box64/bin/box64" ] || { echo "Box64 binary missing after install" >&2; exit 1; }
[ -x "$ASSET_DIR/wine/bin/wine" ] || { echo "Wine binary missing after extraction" >&2; exit 1; }
echo "Bundled Box64 $BOX64_VERSION and Wine WoW64 $WINE_VERSION in $ASSET_DIR"
