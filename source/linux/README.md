# D2ARM Linux

This is the first native desktop port slice. It adds a Qt desktop launcher, Russian UI strings, an ARM64 AppImage build path, a Linux game shadow, D2Android's existing game mod and D2DX wide-screen renderer patches, and an SDL gamepad profile.

## Current scope

- The launcher expects an existing Diablo II: Lord of Destruction 1.13d folder containing `Game.exe`. It does not download Blizzard installers or game data.
- The launcher uses an ARM64 `box64` executable and an x86_64 `wine64` build with 32-bit Windows game support. These are host dependencies and are not bundled in this AppImage yet.
- The game is started from a private shadow directory under the user's app-data folder. Original game files remain in place; the shadow links them and places D2Android's loader, mod DLL and patched D2DX files alongside them.
- The shipped D2DX patches keep the wide-screen rendering and frame-time fixes from the Android project. The AppImage build downloads D2DX 0.99.529 and checks its pinned SHA-256 before applying the existing binary patches.
- The native SDL gamepad mapping follows the Android defaults where Linux can send the same input: left-stick movement, A attack, X/B/Y/RB select F1–F4, LT switches to the second skill row, LB shows item names, Start menu, Back inventory, left-stick click run/walk, right-stick click weapon swap, right trigger take and D-pad belt keys. This replaces the Android overlay with X11 input injection and is a first pass, not yet the full customizable Android mapping. It needs an X11 or XWayland session.
- The launcher uses Russian strings when the system language is Russian and English otherwise.
- To use Russian game text, point it at an installation that already contains the Russian game data; the game files remain user's own data and are linked into the shadow directory without conversion.
- The AppImage includes the project MIT/LGPL texts and D2DX's README and license files.

## Build on ARM64 Linux

Install a C++17 compiler, CMake, Ninja, Qt 6 Widgets, SDL2, X11/XTest development headers, `i686-w64-mingw32-gcc`, `i686-w64-mingw32-windres`, curl, unzip, Python 3 and `rsvg-convert`. Then install ARM64 builds of `linuxdeploy`, `linuxdeploy-plugin-qt` and `appimagetool` on `PATH` and run:

```sh
./build-appimage.sh
```

The result is `dist/d2arm-linux-aarch64.AppImage`. Pushing to `main` or manually starting the workflow creates a downloadable build artifact. Pushing a `v*` tag also creates a GitHub Release with the AppImage and its SHA-256 file. You can build just the runtime game assets with `../tools/build-linux-assets.sh`.

## Controls

Connect a USB or Bluetooth controller before launching. The gamepad profile sends input to the active X11 window. Keyboard and mouse remain available. Controller remapping, the Android touch overlay, the in-app installer, settings, save management and the rest of the Android UI are not ported in this first slice.
