Total War: ATTILA - Engine Performance Fix (dinput8.dll)
========================================================

A drop-in DLL that fixes CPU bottlenecks in Attila's engine. It changes no game
files and no saves, so you can remove it at any time.

Created with the help of AI.

Source code: https://github.com/JSa25/AttilaCPUPerfFix

Game version: Attila 1.6.0 (current Steam build). Every patch first checks the
exact game bytes it replaces. If they don't match (a different game version or
another engine mod), that patch turns itself off and the game runs unmodified.


What it fixes
-------------
- Multithreading (the big one): almost every memory allocation in the game goes
  through one shared heap with a single lock. With many threads, they spend
  most of their time waiting on that lock instead of working. In profiling,
  about 85% of the game's CPU time was threads spinning on it, keeping around
  19 cores busy for nothing. The heap is replaced with mimalloc plus a fast
  small-block pool: the game drops to about 3.5 busy cores and FPS more than
  doubles from this fix alone. It also removes the stutter that builds up over
  a long session.
- Thread count: after the lock fix, waking 31 worker threads every frame cost
  the main thread about a third of its time. That's why 8 threads is
  recommended below.
- Render-list sorts (two of them, the second new in version 4): the main
  thread sat idle waiting for worker threads to sort the draw lists. Both sorts
  are now about 11 times faster, with the same result.
- Battle draw batching: the game split identical materials into separate draw
  batches. In big melees this cut batches per frame from about 18,000 to 2,900.
- x87 rounding: about 1,280 slow float-to-int conversions across the engine are
  replaced with modern instructions that give identical results.
- Campaign map: fog tests, terrain height lookups and a curve table are made
  cheaper. Repeated terrain height queries are cached.
- Small memory copies take a fast path.
- Battle render loops (new in version 5): six of the loops that hand units and
  objects to the graphics card now tell the CPU to fetch the next items' data
  ahead of time, so they wait less on memory. About 1.5-3% more fps in big
  battles on the test PC; CPUs with smaller caches may gain more.
- Loading (new in version 6.1): before using a file from the packs, the game
  checks every mod folder for a loose copy of it. With big mods that's
  hundreds of thousands of failed checks per load (1.4 million for a MK1212
  siege). The mod now remembers which folders don't exist and answers those
  checks itself. It watches those folders, and if anything in them changes it
  forgets everything and asks Windows again, so new loose files are still
  found. The unpacked save is also read through a small buffer instead of
  hundreds of thousands of tiny reads.
- Battle loading (new in version 6.1): while a battle loads, the game
  compresses the pictures it uses for distant units on the CPU. That
  compressor is replaced with one that is about 14 times faster and gives
  byte-identical textures.
- Menus (new in version 6.2): opening a province, army or other panel froze
  the game for about a tenth of a second, even without any mods. Each time a
  panel closed, the game threw its pictures away, and the next open loaded the
  same files from disk again (3,000 loads of only about 600 different files in
  45 seconds of clicking through provinces with MK1212). The mod now keeps up
  to 1,024 recently used menu pictures loaded, and also answers the menus'
  checks for loose files that don't exist. Opening a menu now takes about
  19 ms instead of about 100 ms. The first time you open each kind of menu
  after starting the game it still stutters like before, because its
  pictures have to be loaded once. Every open after that is fast.

Every code patch produces bit-identical results to the original code. They
were tested against the game's own code on millions of inputs. (The menu fix
only keeps pictures in memory longer; what you see is the same.)


Measured results (Ryzen 9 9950X3D, RTX 5090, 4K)
-------------------------------------------------
Same save, same camera views, same custom battle. Battle = heaviest 30 s.

                          Campaign view 1  Campaign view 2  End turn  Battle
  Vanilla, 32 threads         16-18 fps        13-15 fps      ~32 s   50.7 fps
  Vanilla, 8 threads             47 fps           41 fps       28 s   54.6 fps
  Mod,     32 threads           130 fps          104 fps       25 s   84.0 fps
  Mod,      8 threads           120 fps*         117 fps       24 s   89.8 fps

  * capped by vsync

Loading, MK1212 with about 25 submods (stopwatch, same save / same siege):

                                    6.0      6.1
  Campaign save, second load        14 s     11-12 s
  Campaign save, first load         17 s     16 s
  Siege battle                      1:17     1:12

Opening a menu, MK1212 campaign (time of the frame the menu opens in,
median of about 50-70 opens of provinces and armies):

  6.1                               about 100 ms
  6.2                               about 19 ms

Your numbers will differ, but the gains come from CPU time saved per frame, so
CPU-bound systems benefit the most. If your battles run at 4K and the graphics
card is at 95-100% load, the card is the limit and the battle fps gain will be
small; the campaign map usually still gains.


Install
-------
1. Copy dinput8.dll into the game folder, next to Attila.exe, for example:
   ...\steamapps\common\Total War Attila\
2. Start the game. A heapfix.log file appears in the same folder and lists
   which fixes are active, plus your game build, system and settings.

Uninstall: delete dinput8.dll (and heapfix.log).


It doesn't show up in the launcher's mod list
---------------------------------------------
That's normal. This mod is a DLL in the game folder, not a .pack mod, so the
launcher and mod managers don't know about it. It's active as long as
dinput8.dll is in the game folder. To check, start the game and open
heapfix.log in the game folder: the STATUS block lists which fixes are on.


Recommended settings
--------------------
Set both of these in this file (back it up first):
  %APPDATA%\The Creative Assembly\Attila\scripts\preferences.script.txt

  number_of_threads 8;
      Use 8 (or your CPU's thread count, if it has fewer). Waking more worker
      threads every frame costs the main thread time, and with this mod 8 is as
      fast as 32 and steadier.

  gfx_video_memory -4000;
      Must be a negative number. This is the community-recommended VRAM setting
      for stability on cards with 4 GB or more.


Turning single fixes off
------------------------
Create an empty file with one of these names in the game folder to disable that
fix (useful if you suspect one of them):

  heapfix_off.txt      memory allocator (the game keeps its own heap)
  pool_off.txt         small-object pool lookup
  sortfix_off.txt      render-list sorts (both)
  batchfix_off.txt     battle draw batching
  campfix_off.txt      campaign map fixes
  x87fix_off.txt       x87 rounding
  heightcache_off.txt  terrain height cache
  memfast_off.txt      small memory copy fast path
  loopfetch_off.txt    battle render loop prefetch
  attrcache_off.txt    loading: missing-folder cache
  fileio_off.txt       loading: buffered save reads
  texfix_off.txt       battle loading: texture compressor
  uikeep_off.txt       menus: keep menu pictures loaded

The STATUS lines in heapfix.log show which fixes are on, and why one is off.


Reporting a problem
-------------------
If the mod doesn't seem to work, or the game crashes, please send heapfix.log
from the game folder (it's a small text file). It shows:
- whether the game loaded the mod at all, and from which folder
- your game build, Windows version, CPU, RAM and the settings above
- other DLLs in the game folder that hook the game (ReShade, DXVK, other mods)
- a STATUS block: which fixes are on, and why one is off
- for a crash: where it happened (EXCEPTION lines), so it can be traced, with a
  plain line saying whether it was in the game's own code or in this mod,
  and another one if it happened while the menu fix was working
Once the log passes 1 MB, the oldest game sessions are removed at the next
start, so it stays small enough to post.
The log only records; it never changes how the game handles a crash. It shows
your Windows user name in folder paths, so blank that out before posting it
publicly if you like.


Compatibility and notes
-----------------------
- Needs a CPU with SSE4.1 (any CPU from roughly 2008 onward).
- NOT compatible with "Attila 32bit Engine Improver" or any other dinput8.dll
  mod. Only one dinput8.dll can be loaded.
- Works with UI mods and normal pack mods.
- Players report it works on Linux with Proton.
- If you use "Attila 4K UI Scale": on its version 2.0 or newer, keep the
  "Vanilla" preset in its F10 menu (the default). On older versions, use its
  "Basic" file, not "All features on". Its heavier settings turn on bigger
  shadows and more 3D units, which cost back much of the FPS this mod gains.
- The fixes watch the game's floating-point mode at runtime. If the game ever
  switches it, the affected patches restore the original code automatically.
- Not fixed: the short freeze during autosave (save compression) and end-turn
  AI time, which is only slightly faster.
- Optional add-on (new in 6.3): "Upscaling and Framegen for Attila" adds NVIDIA
  DLSS upscaling and frame generation on GeForce RTX cards. It is a separate
  download and needs this mod 6.3 or newer. Without it, nothing of it is
  active.


Changelog
---------
Version 6.3: support for the optional "Upscaling and Framegen for Attila"
           add-on (NVIDIA DLSS upscaling and frame generation on RTX
           cards, separate download; AMD FSR support in the works).
           Without the add-on 6.3 works exactly like 6.2.

Version 6.2: no more freeze when opening a menu. Menu pictures stay
           loaded instead of being thrown away and loaded again on every
           open, and the menus' checks for missing loose files are answered
           from the cache. MK1212: opening a province or army 100 -> 19 ms.
           The first open of each kind of menu per game start still
           stutters (its pictures load once).
           Turn it off with uikeep_off.txt.
           Also a clearer heapfix.log. Each game start begins
           with a header line, a "Game version" line says whether your game
           build is supported, and a STATUS block lists every fix as on, part
           or OFF with the reason. Crash records say in plain words whether the
           crash was in the game's own code or in this mod. The log no longer
           fills up with file-reading statistics and keeps itself under about
           1 MB. New off switches for the two oldest fixes: heapfix_off.txt
           and pool_off.txt. SHA-256 of dinput8.dll:
           2be088133543350d5798f137c5c56178fc9f33f7a24ff9bcf6b4a07916a9db47
Version 6.1: faster loading, mostly for big mod setups. Missing loose-file
           folders are remembered (checked against Windows in game: 0
           differences in over 500,000 checks and opens), the save is read
           through a buffer, and the texture compressor used while battles
           load is about 14x faster with identical output (checked in game:
           46,000 calls, 0 differences). MK1212: campaign save load 14 ->
           11-12 s, siege battle load 1:17 -> 1:12. Turn them off with
           attrcache_off.txt, fileio_off.txt and texfix_off.txt.
           Also: when the fast small-block heap fills up in long sessions, it
           now grows in 32 MB pieces placed in the smallest free gaps,
           instead of 128 MB pieces cut out of the big gaps the game needs
           for large blocks (a long MK1212 campaign could crash with "malloc
           ... FAILED" after the heap reached 768 MB). Same speed.
           SHA-256 of dinput8.dll:
           874d240cb90f0746281219ef3394cdd4abb595e96e1a1091b3a9bd3a103ec2aa
Version 6.0: fixes out-of-memory crashes with big mods like MK1212 (battles
           crashing while loading). The mod's memory replacement used up to
           ~600 MB more of the game's 4 GB address space than the game's own
           allocator. Now large blocks stay on the game's allocator and the
           fast small-block heap has a fixed size (512 MB, growing to 768 MB
           only when needed). MK1212 at 4K: lowest free address space 96 MB
           -> 786 MB, no crash. Same fps as 5.5 (same replay, fixed camera:
           103.4 vs 103.5 fps). SHA-256 of dinput8.dll:
           73d695cff0115c579306f092e1973df236e3b4999724116a971d3932a6e828cd
Version 5.5: fixes "nothing patched" for players where an overlay or another
           program loads dinput8.dll before the game's own code (the log
           said empire.retail.dll is not loaded). The mod now waits for the
           game to load and installs then. No other changes.
           SHA-256 of dinput8.dll:
           b03ac511c29aca4323f85d3ea231b7ff8523a4a6b59f14c05be8267f4b5ca15b
Version 5: six battle render loops prefetch the data they need next, about
           1.5-3% more fps in big battles (two replay A/B tests on the test
           PC). Turn it off with loopfetch_off.txt. The self-update now saves
           the file as dinput8.dll (it could end up named DINPUT8.dll, which
           worked the same but looked odd). SHA-256 of dinput8.dll:
           615b0cc84bee1abfc788a6922ed6be3ddb14cab80b9a9cde7dd477745bfdf324
Version 4: a second render-list sort is replaced the same way as the first:
           about 11x faster, same order. In a big battle test this went from
           82.8 to 96.6 fps (two separate battles, so treat it as roughly).
           heapfix.log now records the mod version, game build, system,
           settings and other hook DLLs, and logs crashes (EXCEPTION lines)
           so problems can be traced. Steam Workshop installs now update
           themselves: when Steam downloads a newer version, the next game
           start puts it in the game folder (no more running install.bat
           after each update). SHA-256 of dinput8.dll:
           627459ab39c6c3fc114e9be799bc9d0cb73eb84a71df73aa19f772759517eac9
Version 3: dinput8.dll is now built from the published source code
           (https://github.com/JSa25/AttilaCPUPerfFix), with the
           developer-only measuring code removed. Same fixes, same performance.
           The build is reproducible; SHA-256 of dinput8.dll:
           fb0494a9748f9ce6a17891c7e03cf36f2a1331ac1d0eb752f4b3f3477d62464b
Version 2: readme now explains the multithreading fix. dinput8.dll is unchanged
           from version 1, so there is no need to reinstall it.
Version 1: first release.


Credits and licenses
--------------------
Built with AI assistance (Claude by Anthropic). Tested in game by the author.
mimalloc (c) Microsoft Corporation and Daan Leijen, MIT license; see
LICENSE-mimalloc.txt.
