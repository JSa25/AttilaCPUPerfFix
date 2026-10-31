# Attila Engine Performance Fix

Source code for the Total War: ATTILA engine performance mod (Attila Engine Performance Fix, called Attila CPU Performance Fix up to 6.2): a `dinput8.dll` that the game loads at
startup and that patches the engine in memory. No game files are changed.

Download the ready-made DLL:
- Nexus Mods: https://www.nexusmods.com/totalwarattila/mods/168
- Steam Workshop: https://steamcommunity.com/sharedfiles/filedetails/?id=3824249154 (after subscribing, run `install.bat` in the mod's folder)

Created with the help of AI (Claude by Anthropic). Tested in game by JGdaCaesar.

## Upscaling and Framegen add-on (6.3)

From 6.3 the DLL also contains the game side of the separate **Upscaling and Framegen for Attila** add-on (NVIDIA
DLSS upscaling and frame generation on GeForce RTX cards). It does nothing unless the add-on's `FrameBridge` folder,
with its 64-bit helper, is in the game folder. The helper's source is in its own repository:
[JSa25/TotalWarUpscalingFramegen](https://github.com/JSa25/TotalWarUpscalingFramegen).

## What each file does

| File | Fix |
|---|---|
| `heapfix.c` | Entry point (`DllMain`, or a DLL load notification when something loaded `dinput8.dll` before the game DLL), dinput8 forwarding, `heapfix.log`, and the main fix: replaces the game's single-lock dlmalloc heap for blocks up to 16 KB; larger blocks stay on the game's dlmalloc |
| `pools.c`, `dlmalloc/` | The heap for those blocks: 8 dlmalloc 2.8.6 heaps with the game's own settings (64 KB growth, 2 MB trim, nothing reserved up front), each with its own lock, and a small per-thread cache, so threads do not wait on one lock but the address space is used like the game's own heap |
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
| `dinput8.def` | The exports a `dinput8.dll` must have |
| `bridgeprobe.c` | Upscaling add-on (6.3), only with the add-on installed: finds the camera, the main depth buffer and the point where the interface starts; shrinks and jitters the 3D scene's viewports for DLSS; gives pixel shaders a corrected camera block at the lower presets; reads and saves `FrameBridge\settings.txt` |
| `fgbridge.c`, `fgb_proto.h` | Upscaling add-on: shares the frame's image, depth and motion vectors with the 64-bit helper (textures, fences and a small shared block, by name), starts the helper, puts the upscaled image back before the interface is drawn, and hands the finished frame over at Present. NVIDIA GPUs only |
| `fgmotion.c`, `motionvs.h` | Upscaling add-on: motion vectors for soldiers, horses and campaign armies (the game's skinning repeated for this frame and the previous one) |
| `fgpanel.c` | Upscaling add-on: the F11 settings panel |
| `fgb_motion_api.h` | Upscaling add-on: `fgb_motion_vectors`, exported for other mods - this frame's motion vectors on the game's D3D11 device (the struct and what the values mean) |
| `passtime.c`, `gputime.c` | Upscaling add-on: the hook on the render stage driver the bridge runs on, and the hook on the game's `GetProcAddress` import (D3D11 device and DXGI factory) |

Every patch first compares the game's bytes at its address with what it expects, and skips itself
(and says so in `heapfix.log`) if they differ. So on another game version it does nothing.

## Building

You need:
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the release used: 20261006, UCRT, x86_64 host), with its `bin` folder on `PATH`

Then run:

```
build.bat
```

The DLL lands in `build\dinput8.dll`. The build is reproducible: with the same llvm-mingw
version you should get exactly the same file as the one on Nexus. Compare the SHA-256
(`certutil -hashfile build\dinput8.dll SHA256`) with the one given for each release.

### Automated builds

Every commit is also built on GitHub's own machines by
[.github/workflows/build.yml](.github/workflows/build.yml), with the compiler
downloaded from its official release (checksum pinned). Open the latest run under **Actions**:
its summary shows the SHA-256 of the DLL, which matches the Nexus download for the same version,
and the DLL itself can be downloaded there. Builds also carry a signed build attestation
(`gh attestation verify dinput8.dll -R JSa25/AttilaCPUPerfFix`).

## Tests

The tests run the game's own code next to the replacements on millions of inputs and check the
results are identical (`diag_test` instead checks the crash records on deliberate faults; `lateload_test` loads the built DLL before the game DLL, as an overlay can, and checks every fix still installs from Windows' load notification; `loopfetch_test` runs each prefetch stub over arrays that end at a no-access page, with stale pointers in the slots a loop never reaches, and checks every register matches the original instructions; `fileio_test` and `attrcache_test` compare against plain Windows calls on a temporary folder tree while files and folders are created, renamed and deleted, and `attrcache_test` also runs with the folder watches ignored to show it catches a stale cache; `uikeep_test` runs the menu fix against a stand-in for the game's picture table: kept, reused, in use, deleted meanwhile, oldest deleted). They read
your copy of the game's `empire.retail.dll` (it is not included here):

```
build.bat test "C:\...\steamapps\common\Total War Attila\empire.retail.dll"
```

## License

See [LICENSE](LICENSE). `dlmalloc/malloc-2.8.6.c` is dlmalloc by Doug Lea, released to the public domain
([CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/)), unchanged.
