# Attila Engine Performance Fix

Source code for the Total War: ATTILA engine performance mod (Attila Engine Performance Fix, called Attila CPU Performance Fix up to 6.2). It patches the engine in memory at startup. No game files are changed.

Download the ready-made files:
- Nexus Mods: https://www.nexusmods.com/totalwarattila/mods/168
- Steam Workshop: https://steamcommunity.com/sharedfiles/filedetails/?id=3824249154 (after subscribing, run `install.bat` in the mod's folder)

Created with the help of AI (Claude by Anthropic). Tested in game by JGdaCaesar.

## The two builds

The mod ships as **one release with one version number** for both of these:

| Folder | Builds | For |
|---|---|---|
| [`32bit/`](32bit) | `dinput8.dll` | the normal 32-bit game, which loads it at startup |
| [`64bit/`](64bit) | `perffix64_plugin.dll` | [Sol Invictus's 64-bit Attila](https://github.com/Sol-Invictus2/64-bit-Attila), as a plugin |
| [`shared/`](shared) | — | the files both sides build |
| [`modmanager/`](modmanager) | `TotalWarModManager.exe` | Total War Mod Manager (from 7.6): Steam's Play button opens it, for both versions |

`shared/version.h` is that one version number: `64bit/src/version64.h` is nothing but an `#include` of it, so a
release can never go out with the two halves numbered differently. The rest of `shared/` is the cloth-banner code
(`flagsim.c`, `fwpack.c`, `flagvs.h`, `flagvs_bc.h`, `fw_kind.h`), which the 32-bit DLL includes as source and the
64-bit plugin compiles through `64bit/src/flagsim64.c`. Both builds find it with `-I../shared`.

The two sides do **not** share an allocator: the 32-bit DLL carries dlmalloc 2.8.6 (`32bit/dlmalloc/`), the 64-bit
plugin uses mimalloc 3.5.3, which its build downloads.

## Upscaling and Framegen add-on (6.3)

From 6.3 both builds also contain the game side of the separate **Upscaling and Framegen for Attila** add-on (NVIDIA
DLSS upscaling and frame generation on GeForce RTX cards; from 6.10.0 also AMD FSR upscaling and frame generation, with
AMD's reactive mask for transparent effects, on any other card). It does nothing unless the add-on's `FrameBridge` folder,
with its 64-bit helper, is in the game folder. The helper's source is in its own repository:
[JSa25/TotalWarUpscalingFramegen](https://github.com/JSa25/TotalWarUpscalingFramegen).

## 32bit/ — what each file does

| File | Fix |
|---|---|
| `heapfix.c` | Entry point (`DllMain`, or a DLL load notification when something loaded `dinput8.dll` before the game DLL), dinput8 forwarding, `heapfix.log`, and the main fix: replaces the game's single-lock dlmalloc heap for blocks up to 16 KB; larger blocks stay on the game's dlmalloc |
| `pools.c`, `dlmalloc/` | The heap for those blocks: 8 dlmalloc 2.8.6 heaps with the game's own settings (64 KB growth, nothing reserved up front; since 6.10.0 memory goes back to Windows on a free only past 64 MB unused at the top, plus the game's own trim), each with its own lock, and a small per-thread cache, so threads do not wait on one lock but the address space is used like the game's own heap |
| `pool.c` | Faster free lookup for the engine's small-object pools |
| `sortfix.c` | The two render-list sorts (by +0x6C/+0x68 and +0x4C/+0x48): key extraction plus radix sort, same order as the game's sorts |
| `batchfix.c` | Battle draw batching: items with the same material and equal variant values share a batch |
| `campfix.c` | Campaign map: four x87 rounding-mode blocks (fog of war, terrain height, curve table) |
| `x87fix.c` | The same x87 rounding idiom at the ~1,280 other sites in the engine (`fisttp` / SSE4.1 `roundss`) |
| `heightcache.c` | Cache for repeated terrain height queries on the main thread |
| `memfast.c` | Fast path for the engine's memcpy on copies of 64 bytes or less |
| `loopfetch.c` | Six battle render loops prefetch the field the loop reads next, four items ahead (prefetch only, never a load through an item pointer) |
| `fileio.c` | Loading: `.pack` files opened without `FILE_FLAG_NO_BUFFERING`; small reads of read-only files in the game's user folder (the unpacked save) served from a 64 KB buffer, file position kept exactly |
| `attrcache.c` | Loading and menus: cache of missing loose-file folders behind the game's `GetFileAttributesW` and read-only `CreateFileW` calls (including its CRT `stat`: `FILE_READ_ATTRIBUTES` with backup semantics, which the menus use). Every folder above a cached name is watched (`FindFirstChangeNotification`); any change clears the cache. Answers and last-error values are the same as Windows' |
| `texfix.c` | Battle loading: the DXT1 compressor for unit imposters (FUN_118e8390 / FUN_1192eee0) redone with SSE2, byte-identical output |
| `uikeep.c` | Menus: the deletion step of the UI picture queue (FUN_1174cca0) keeps a picture loaded instead of deleting it and remembers its name, up to 1,024; the game's own lookup finds it on the next open. Past 1,024 the oldest still-unused one is deleted the game's way. Pictures are found again by name, never by a stored pointer |
| `fps.c` | Hook on the per-frame function that runs the rounding-mode safety checks of `campfix.c` and `x87fix.c` |
| `diag.c` | Diagnostics in `heapfix.log`: mod version, game build and whether it is supported, system, settings, other hook DLLs, and crash records with a plain line saying whose code it was (a vectored exception handler that only logs and never handles the exception) |
| `logtrim.c` | Keeps `heapfix.log` under about 1 MB: at game start, past 1 MB the oldest whole sessions are dropped |
| `memwatch.c` | Crash logging (7.0): the game's MapViewOfFile / VirtualAlloc imports; a failed file map is logged with the free address space, the heaps are trimmed and the map tried once more; a line when the largest free block falls below 512 / 256 / 128 / 64 MB |
| `plugins.c` | Plugins (7.0): every `plugins\*.dll` in the game folder, loaded in name order once the fixes are in (the point where a hook-slot DLL such as `xinput1_3.dll` loads), each logged; `[mod] plugins=0` in `heapfix.ini` switches it off |
| `partcull.c` | Unit detail cull (7.1): a soldier's small parts (belts, quivers, bridles, crests) are not drawn once they would be fewer than a set number of pixels tall - 16 for bodies and 9 for heads at the default "light". Off / light / normal / strong from the F8 panel or `[tuning] culling=` in `heapfix.ini`; `partcull_off.txt` leaves it out entirely |
| `uicache.c` | Battle UI cache (7.1): the unit cards (`cards_panel`) are drawn into two textures and shown from them instead of being walked and drawn every frame. Redrawn about 15 times a second, for 250 ms when a key or mouse button goes down and again when it comes up (holding one - a camera drag, a drag-select, Space - does not keep them redrawing), while the cursor is over the panel, and when the panel moves. `[fixes] battle_ui_cache=0` or `uicache_off.txt` switches it off |
| `soundguard.c` | Huge battles: a sound the game's full sound pool gave no buffer for is skipped instead of crashing. `[fixes] sound_crash_guard=0` or `soundguard_off.txt` switches it off |
| `overlay.c` | The F8 panel (Culling, What's new) and the pop-ups drawn on the game's own frames, for when the upscaling add-on is not installed; with the add-on, its bridge draws them instead (`bridgeprobe.c`) |
| `scriptext.c` | Script extender: with a `lua_ext\` folder in the game folder, its `*.lua` files run in each new script environment of the game's own Lua 5.1, plus a console window (Ctrl+`); off without the folder |
| `menuver.c` | The engine fix's version under the game's own on the main menu |
| `dinput8.def` | The exports a `dinput8.dll` must have |
| `bridgeprobe.c` | Upscaling add-on (6.3), only with the add-on installed: finds the camera, the main depth buffer and the point where the interface starts; shrinks and jitters the 3D scene's viewports for DLSS; gives pixel shaders a corrected camera block at the lower presets; reads and saves `FrameBridge\settings.txt` |
| `fgbridge.c`, `fgb_proto.h` | Upscaling add-on: shares the frame's image, depth and motion vectors with the 64-bit helper (textures, fences and a small shared block, by name), starts the helper, puts the upscaled image back before the interface is drawn, and hands the finished frame over at Present. With AMD FSR (6.10.0) also the hole fill of the motion vectors and the reactive mask (the transparent effects' change to the scene, and moving soldiers) |
| `fgmotion.c`, `motionvs.h`, `windvs.h` | Upscaling add-on: motion vectors for soldiers, horses and campaign armies (the game's skinning repeated for this frame and the previous one) and, from 6.10.0, for swaying trees and grass (the game's wind math at this frame's and the previous frame's time) |
| `fgpanel.c` | Upscaling add-on: the settings panel (F8) (with the FPS counter, the upscaler / frame generation choice and the recommendation for this PC) |
| `fgprobe.h` | Upscaling add-on: the bridge cost probe (`fgbridge.c`), driven by the sweep's `probe=` key |
| `fgb_motion_api.h` | Upscaling add-on: `fgb_motion_vectors`, exported for other mods - this frame's motion vectors on the game's D3D11 device (the struct and what the values mean) |
| `passtime.c` | Upscaling add-on: the hook on the render stage driver the bridge runs on |
| `fwvs_gen.c` | Compiles the two flag vertex shaders to the bytecode in `../shared/flagvs_bc.h`. Run by hand, not by the build: doing it at startup cost 264 ms of every player's game and needed `d3dcompiler_47.dll` |
| `rslink.cpp`, `reshade/` | Upscaling add-on (6.9): with ReShade in the game folder, registers as a ReShade add-on and runs ReShade's effects at the point where the interface starts (after DLSS), with the scene depth and the camera's near / far for ReShade's depth; the glue in `bridgeprobe.c` presents from under ReShade's swapchain wrapper. C++ without the C++ runtime; `reshade/` holds ReShade 6.8.0's add-on API headers, unchanged |

`../shared/flagsim.c`, `flagsim.h`, `flagvs.h` hold the cloth banners' GPU simulation, included as source by
`bridgeprobe.c` rather than linked, so it also builds standalone - and so the 64-bit plugin builds the same code.

Every patch first compares the game's bytes at its address with what it expects, and skips itself
(and says so in `heapfix.log`) if they differ. So on another game version it does nothing.

## 64bit/ — the plugin

The same fixes for the 64-bit game. `64bit/src/*64.c` are the ports (x87, textures, pools, the render-list sorts,
batching, the height cache, the detail cull, the UI picture keep and cache, the prefetches, the sound guard, the
script extender, the F8 panel, the self-update, crash logging and the loader glue).

Four files are **generated at every build** from the 32-bit sources, so the two sides cannot drift apart. They are not
in this repository; the generator and its inputs are:

| Generated | By | From |
|---|---|---|
| `src/scriptext64.c` | `tools/gen_scriptext64.py` | `32bit/scriptext.c` + `src/scriptext64_{head,tail}.inc` |
| `src/flagwave64.c` | `tools/gen_flagwave64.py` | `src/flagwave.c` (Flag Wave's own plugin source) + `src/flagwave64_tail.inc` |
| `out/gen/bridge64.c`, `fgmotion64.c` | `tools/gen_bridge64.py` | `32bit/bridgeprobe.c` + its `bp_*.inc.c` parts, `32bit/fgmotion.c` |
| `out/gen/{version.h,update.c,fgpanel.c}` | `tools/gen_shared64.py` | `shared/version.h`, `32bit/update.c`, `32bit/fgpanel.c` |

`out/gen/ctxwrap64.c` is generated too, from the toolchain's own `d3d11.h`, so the wrapper's method table always
matches the header the plugin is built against.

`src/x87_sites.h` is the one generated file that *is* committed: it is made from a table read out of the game's own
engine, which is not published, the same reason `shared/flagvs_bc.h` is committed on the 32-bit side.

`tools/gen_sol_sites.py` reads a checkout of
[Sol-Invictus2/64-bit-Attila](https://github.com/Sol-Invictus2/64-bit-Attila) and writes the loader's patch
sites for the mock-loader test. **The build does not need it.** Its output is not committed here for the same reason
the other generated files are not: it is made from one version of the loader, so a committed copy would go stale
without anyone noticing.

## Building

You need [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the release used: 20261006, UCRT, x86_64 host), with
its `bin` folder on `PATH`.

**32-bit** — in `32bit/`:

```
build.bat
```

The DLL lands in `32bit\build\dinput8.dll`. This build is reproducible: with the same llvm-mingw version you get
exactly the same file as the one on Nexus. Compare the SHA-256
(`certutil -hashfile build\dinput8.dll SHA256`) with `32bit\dinput8.dll.sha256`.

**64-bit** — you also need the [mimalloc 3.5.3](https://github.com/microsoft/mimalloc/releases/tag/v3.5.3) source and
Python. Then, in `64bit/`:

```
MI=/path/to/mimalloc-3.5.3 ./build.sh
```

The plugin lands in `64bit/out/perffix64_plugin.dll`; copy it to `<game>/attila64/plugins/` as `perffix64.dll`. This
build is not byte-reproducible - the linker writes a timestamp into the file.

### Automated builds

Every commit is built on GitHub's own machines, with the compiler downloaded from its official release (checksum
pinned): [build.yml](.github/workflows/build.yml) for the 32-bit DLL and
[build64.yml](.github/workflows/build64.yml) for the 64-bit plugin. Open the latest run under **Actions**: its summary
shows the SHA-256, and the file itself can be downloaded there. Builds also carry a signed build attestation
(`gh attestation verify dinput8.dll -R JSa25/AttilaCPUPerfFix`).

## Tests

The tests run the game's own code next to the replacements on millions of inputs and check the
results are identical (`diag_test` instead checks the crash records on deliberate faults; `lateload_test` loads the built DLL before the game DLL, as an overlay can, and checks every fix still installs from Windows' load notification; `loopfetch_test` runs each prefetch stub over arrays that end at a no-access page, with stale pointers in the slots a loop never reaches, and checks every register matches the original instructions; `fileio_test` and `attrcache_test` compare against plain Windows calls on a temporary folder tree while files and folders are created, renamed and deleted, and `attrcache_test` also runs with the folder watches ignored to show it catches a stale cache; `uikeep_test` runs the menu fix against a stand-in for the game's picture table: kept, reused, in use, deleted meanwhile, oldest deleted). They read
your copy of the game's `empire.retail.dll` (it is not included here), and run from `32bit/`:

```
build.bat test "C:\...\steamapps\common\Total War Attila\empire.retail.dll"
```

## License

See [LICENSE](LICENSE). `32bit/dlmalloc/malloc-2.8.6.c` is dlmalloc by Doug Lea, released to the public domain
([CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/)), unchanged. `32bit/reshade/*.hpp` are ReShade's add-on API
headers by Patrick Mours (BSD-3-Clause OR MIT, as stated in each file), unchanged. mimalloc (MIT) is not in this
repository; the 64-bit build downloads it. Sol Invictus's
[64-bit Attila](https://github.com/Sol-Invictus2/64-bit-Attila) is MIT; no file of his is included here. Portions of `modmanager/` come from it and keep its MIT license: see
[modmanager/SOL_INVICTUS_MIT.txt](modmanager/SOL_INVICTUS_MIT.txt).
