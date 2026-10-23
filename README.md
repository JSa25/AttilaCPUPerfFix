# Attila CPU Performance Fix

Source code for the Total War: ATTILA CPU performance mod: a `dinput8.dll` that the game loads at
startup and that patches the engine in memory. No game files are changed.

Download the ready-made DLL:
- Nexus Mods: https://www.nexusmods.com/totalwarattila/mods/168
- Steam Workshop: https://steamcommunity.com/sharedfiles/filedetails/?id=3824249154 (after subscribing, run `install.bat` in the mod's folder)

Created with the help of AI (Claude by Anthropic). Tested in game by JGdaCaesar.

## What each file does

| File | Fix |
|---|---|
| `heapfix.c` | Entry point (`DllMain`), dinput8 forwarding, `heapfix.log`, and the main fix: replaces the game's single-lock dlmalloc heap with mimalloc (blocks up to 16 KB) plus a private Windows heap (larger blocks) |
| `pool.c` | Faster free lookup for the engine's small-object pools |
| `sortfix.c` | The two render-list sorts (by +0x6C/+0x68 and +0x4C/+0x48): key extraction plus radix sort, same order as the game's sorts |
| `batchfix.c` | Battle draw batching: items with the same material and equal variant values share a batch |
| `campfix.c` | Campaign map: four x87 rounding-mode blocks (fog of war, terrain height, curve table) |
| `x87fix.c` | The same x87 rounding idiom at the ~1,280 other sites in the engine (`fisttp` / SSE4.1 `roundss`) |
| `heightcache.c` | Cache for repeated terrain height queries on the main thread |
| `memfast.c` | Fast path for the engine's memcpy on copies of 64 bytes or less |
| `fps.c` | Hook on the per-frame function that runs the rounding-mode safety checks of `campfix.c` and `x87fix.c` |
| `diag.c` | Diagnostics in `heapfix.log`: mod version, game build, system, settings, other hook DLLs, and crash records (a vectored exception handler that only logs and never handles the exception) |
| `dinput8.def` | The exports a `dinput8.dll` must have |

Every patch first compares the game's bytes at its address with what it expects, and skips itself
(and says so in `heapfix.log`) if they differ. So on another game version it does nothing.

## Building

You need:
- [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the release used: 20261006, UCRT, x86_64 host), with its `bin` folder on `PATH`
- the [mimalloc](https://github.com/microsoft/mimalloc) **v3.5.3** source, in a folder named `mimalloc` next to `build.bat` (or set `MIMALLOC` to its path)

Then run:

```
build.bat
```

The DLL lands in `build\dinput8.dll`. The build is reproducible: with the same llvm-mingw and
mimalloc versions you should get exactly the same file as the one on Nexus. Compare the SHA-256
(`certutil -hashfile build\dinput8.dll SHA256`) with the one given for each release.

### Automated builds

Every commit is also built on GitHub's own machines by
[.github/workflows/build.yml](.github/workflows/build.yml), with the compiler and mimalloc
downloaded from their official releases (checksums pinned). Open the latest run under **Actions**:
its summary shows the SHA-256 of the DLL, which matches the Nexus download for the same version,
and the DLL itself can be downloaded there. Builds also carry a signed build attestation
(`gh attestation verify dinput8.dll -R JSa25/AttilaCPUPerfFix`).

## Tests

The tests run the game's own code next to the replacements on millions of inputs and check the
results are identical (`diag_test` instead checks the crash records on deliberate faults). They read
your copy of the game's `empire.retail.dll` (it is not included here):

```
build.bat test "C:\...\steamapps\common\Total War Attila\empire.retail.dll"
```

## License

See [LICENSE](LICENSE). mimalloc is (c) Microsoft Corporation and Daan Leijen, MIT license
([third_party/mimalloc-LICENSE.txt](third_party/mimalloc-LICENSE.txt)).
