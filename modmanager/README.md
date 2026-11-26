# Total War Mod Manager

Ships with the engine fix from 7.6 (Steam Workshop). It takes the place of CA's launcher at `launcher\launcher.exe`
(CA's is kept as `launcher_ca.exe`), so Steam's Play button opens it: one window for normal 32-bit Total War: ATTILA
and Sol Invictus's 64-bit Attila, with a mod list for each (or one for both), presets, search, drag to reorder, export
and import with Workshop links, a list code for multiplayer, and CA's launcher one click away.

## Files
- `launcher.c`: paths, mod lists, presets, Steam (through the game's own `steam_api.dll`), Workshop updates, Play.
- `host_ui.inc`: questions, installing 64-bit Attila, Play. `launcher_swap.inc`: install and uninstall (`--install`,
  `--uninstall`, used by the engine fix's `install.bat` / `uninstall.bat`).
- `picker_*.inc`: the window (drawing, lists, packs, input, background work, test hooks).
- `launcher.rc`, `launcher.manifest`, `launcher.ico` (a gold T, drawn for this program).
- The engine fix side is `32bit/launcher_hook.c`: it puts the Mod Manager back after Steam's "Verify files" and reads
  `[mod] launcher=` in `heapfix.ini` (1 = Total War Mod Manager, 2 = CA's launcher).

## Building
With [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) 20261006 (UCRT, x86_64 host) on `PATH`, in this folder:

```
./build.sh
```

The exe lands in `build/TotalWarModManager.exe`. The build is reproducible. The 7.6 release's exe
(`TotalWarModManager.exe.sha256`) was built against version 7.5's `version.h` (`MOD_VERSION_NUM 750`; the number only
sets a marker string inside the exe, the update check reads the installed DLL's own version). To get exactly that file,
build with a `version.h` that says 750:

```
VERSION_DIR=/path/to/folder_with_750_version.h ./build.sh
```

## License
Covered by the [LICENSE](../LICENSE) at the top of this repository, except the portions listed in
[SOL_INVICTUS_MIT.txt](SOL_INVICTUS_MIT.txt), which come from Sol Invictus's 64-bit Attila and keep its MIT license.
