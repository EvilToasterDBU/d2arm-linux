#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$ROOT/.." && pwd)"
BUILD_DIR="${D2_BUILD_DIR:-$ROOT/build/aarch64}"
cd "$ROOT"
ARCH="$(uname -m)"
if [ "$ARCH" != aarch64 ] && [ "$ARCH" != arm64 ]; then
  echo "Build the ARM64 AppImage on an ARM64 Linux host (detected: $ARCH)." >&2
  exit 1
fi

for tool in cmake ninja pkg-config linuxdeploy linuxdeploy-plugin-qt appimagetool rsvg-convert; do
  command -v "$tool" >/dev/null || { echo "Missing build tool: $tool" >&2; exit 1; }
done

if [ "${D2_SKIP_ASSET_BUILD:-0}" = 1 ]; then
  for file in assets/mod/D2Android.exe assets/mod/d2android-mod.dll assets/glide3x.dll assets/d2dx.cfg assets/licenses/d2dx/README.md; do
    [ -s "$ROOT/$file" ] || { echo "Missing Linux asset: $ROOT/$file" >&2; exit 1; }
  done
else
  bash "$REPO_DIR/tools/build-linux-assets.sh"
fi
bash "$REPO_DIR/tools/build-arm-linux-runtime.sh"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON
cmake --build "$BUILD_DIR" --parallel

APPDIR="$ROOT/AppDir"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR"
for runtime_file in \
  "$APPDIR/usr/share/d2arm-linux/assets/runtime/box64/bin/box64" \
  "$APPDIR/usr/share/d2arm-linux/assets/runtime/wine/bin/wine"; do
  [ -x "$runtime_file" ] || { echo "Bundled runtime file is not executable: $runtime_file" >&2; exit 1; }
done
cp "$ROOT/share/AppRun" "$APPDIR/AppRun"
chmod +x "$APPDIR/AppRun"
rsvg-convert -w 256 -h 256 "$ROOT/share/d2arm-linux.svg" -o "$APPDIR/d2arm-linux.png"
ln -sf d2arm-linux.png "$APPDIR/.DirIcon"
chmod +x "$BUILD_DIR/d2arm-linux"

mkdir -p "$ROOT/dist"
export ARCH=aarch64
export LINUXDEPLOY_OUTPUT_APP_NAME="d2arm-linux-aarch64"
export LDAI_OUTPUT="$ROOT/dist/d2arm-linux-aarch64.AppImage"
export LDAI_NO_APPSTREAM=1
linuxdeploy --appdir "$APPDIR" \
  --executable "$BUILD_DIR/d2arm-linux" \
  --desktop-file "$ROOT/share/d2arm-linux.desktop" \
  --icon-file "$ROOT/share/d2arm-linux.svg" \
  --plugin qt --output appimage
echo "Built $ROOT/dist/d2arm-linux-aarch64.AppImage"
