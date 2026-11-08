#!/usr/bin/env bash







set -euo pipefail
cd "$(dirname "$0")/.."
CC=${CC:-i686-w64-mingw32-gcc}
OUT=${D2_MOD_OUT:-winlator/app/src/main/assets/d2/mod}
mkdir -p "$OUT"
FLAGS=(-O2 -s -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type -static-libgcc -fno-asynchronous-unwind-tables)
if [[ "${GITHUB_REF_NAME:-}" == v* ]]; then
  D2_VERSION="${GITHUB_REF_NAME#v}"
else
  D2_VERSION="$(sed -nE 's/^project\(D2ArmLinux VERSION ([0-9.]+).*/\1/p' linux/CMakeLists.txt | head -n1)"
fi
FLAGS+=("-DD2_VERSION_W=L\"${D2_VERSION}\"")
"$CC" "${FLAGS[@]}" -shared -o "$OUT/d2android-mod.dll" mod/d2mod.c -lws2_32
"$CC" "${FLAGS[@]}" -mwindows -o "$OUT/D2Android.exe" mod/loader.c
INSTALL=${D2_INSTALL_OUT:-winlator/app/src/main/assets/d2/install}
mkdir -p "$INSTALL"

"$CC" "${FLAGS[@]}" -mwindows -o "$INSTALL/d2install.exe" tools/installer/d2install.c -luser32

RES=$(mktemp -d)
"${WINDRES:-i686-w64-mingw32-windres}" tools/installer/placeholder.rc -O coff -o "$RES/placeholder.res"
"$CC" "${FLAGS[@]}" -mwindows -o "$INSTALL/placeholder-game.exe" tools/installer/placeholder.c "$RES/placeholder.res"
rm -rf "$RES"
ls -l "$OUT" "$INSTALL"
