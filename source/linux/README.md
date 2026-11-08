# D2ARM Linux

This is the first native desktop port slice. It adds a Qt desktop launcher, Russian UI strings, an ARM64 AppImage build path, a Linux game shadow, D2Android's existing game mod and D2DX wide-screen renderer patches, and an SDL gamepad profile.

## Current scope

- The launcher expects an existing Diablo II: Lord of Destruction 1.13d folder containing `Game.exe`. It does not download Blizzard installers or game data.
- The AppImage bundles ARM64 Box64 and x86_64 Wine 10 WoW64, so the console does not need to download or install either runtime. Wine for ARM64 does exist, but this x86 Windows game is packaged with x86_64 Wine WoW64 running through Box64. The launcher sets Windows 7 mode for the game and its loader; this avoids a critical-section failure in Diablo II 1.13d with newer Wine defaults. It also suppresses unused Mono/Gecko setup prompts on the first launch.
- The game is started from a private shadow directory under the user's app-data folder. Original game files remain in place; the shadow links them and places D2Android's loader, mod DLL and patched D2DX files alongside them.
- The shipped D2DX patches keep the wide-screen rendering and frame-time fixes from the Android project. The AppImage build downloads D2DX 0.99.529 and checks its pinned SHA-256 before applying the existing binary patches.
- The launcher can adjust the in-game interface size by selecting a D2DX render height (480p, 540p or 600p); automatic mode leaves resolution selection to D2DX. The aspect ratio follows the current desktop, so widescreen rendering is retained.
- The SDL gamepad uses the mod's direct movement, targeting and potion commands. Left stick moves; right stick biases targeting; A attacks/interacts; LT+A casts; LT+Y opens the native quick skill picker (S). LB drinks a healing potion and RB drinks mana through the mod's potion selection/refill logic. RT+A/B/X/Y opens inventory/character/quests/skill tree. Back opens inventory, LT+Back toggles the map, Start opens the game menu, and stick clicks toggle run/walk or weapon swap. D-pad moves the panel cursor or uses belt slots outside panels; LT+D-pad gives potions to the mercenary. Targeting only includes visible units: enemies have highest priority, then objects within a short isometric interaction radius (approximately one metre), then NPCs. Cursor geometry will be refined from annotated game screenshots. X11/XWayland is required for pointer and keyboard events.
- The experimental PD2-style controller profile adds adjustable stick deadzone and pointer sensitivity, aim-assist and Quick Cast switches, automatic Xbox/PlayStation button prompt selection, and last-input switching between controller and keyboard/mouse. When installed, the in-game prompts render the original PD2 Xbox/PlayStation button sprites through Diablo II's native DC6 renderer. The sprite files are user-provided from `pd2assets.mpq` and belong in the launcher's `pd2-controller-assets` data folder, named `Controls_Xbox_20.dc6` and `Controls_Dualshock_20.dc6`; the launcher copies them into the game shadow at each start. These renderer exports use Diablo II 1.13d ordinals. Without those files, the prompt falls back to text. Quick Cast immediately casts a bound skill; with it off, the button selects the skill for a later action. These controls are stored in launcher settings. The profile is being iterated in the `experiment/pd2-controller-parity` branch; the repository does not redistribute Project Diablo 2's binaries or art assets.
- The launcher uses Russian strings when the system language is Russian and English otherwise. You can navigate it with the D-pad or left stick, A to confirm, B to cancel, and Start to launch. Back closes an open dialog.
- To use Russian game text, point it at an installation that already contains the Russian game data; the game files remain user's own data and are linked into the shadow directory without conversion.
- The AppImage includes the project MIT/LGPL texts and D2DX's README and license files.

## Build on ARM64 Linux

Install a C++17 compiler, CMake, Ninja, Git, Qt 6 Widgets, SDL2, X11/XTest development headers, `i686-w64-mingw32-gcc`, `i686-w64-mingw32-windres`, curl, tar, unzip, Python 3 and `rsvg-convert`. The build downloads pinned Box64 and Wine WoW64 sources/packages and includes the runtime in the AppImage. Then install ARM64 builds of `linuxdeploy`, `linuxdeploy-plugin-qt` and `appimagetool` on `PATH` and run:

```sh
./build-appimage.sh
```

The result is `dist/d2arm-linux-aarch64.AppImage`. Pushing to `main` or manually starting the workflow creates a downloadable build artifact. Pushing a `v*` tag also creates a GitHub Release with the AppImage and its SHA-256 file. You can build the game assets with `../tools/build-linux-assets.sh` and the offline Box64 + Wine runtime with `../tools/build-arm-linux-runtime.sh`.

## Controls

Connect a USB or Bluetooth controller before launching. The gamepad profile sends input to the active X11 window. Keyboard and mouse remain available. Controller remapping, the Android touch overlay, the in-app installer, settings, save management and the rest of the Android UI are not ported in this first slice.
