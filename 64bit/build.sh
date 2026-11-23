#!/bin/sh
# Builds perffix64_plugin.dll: the engine fix as a plugin for Sol Invictus's 64-bit Attila.
# Install: copy out/perffix64_plugin.dll to <game>/attila64/plugins/ as perffix64.dll.
#
# Needs llvm-mingw (its bin folder on PATH) and the mimalloc 3.5.3 source. See ../README.md.
#   MI=/path/to/mimalloc-3.5.3 ./build.sh
#
# Four files are generated here at every build, from the 32-bit sources in ../32bit and ../shared, so the two sides
# can never drift apart: src/scriptext64.c, src/flagwave64.c, out/gen/fgpanel.c and the bridge. Do not edit those;
# edit the 32-bit file or the matching *_tail.inc and build again.
set -e
P=$(cd "$(dirname "$0")" && pwd)   # 64bit/
R=$(dirname "$P")                  # the repository root
CC=${CC:-x86_64-w64-mingw32-clang}
MI=${MI:-/c/Tools/mimalloc-3.5.3}
# the toolchain's own d3d11.h: the context wrapper's method table must match the header the plugin is built against
D3D11_H=${D3D11_H:-$(dirname "$(command -v "$CC")")/../include/d3d11.h}
[ -f "$MI/src/static.c" ] || { echo "mimalloc 3.5.3 not found at $MI (set MI=...)"; exit 1; }
[ -f "$D3D11_H" ] || { echo "d3d11.h not found at $D3D11_H (set D3D11_H=...)"; exit 1; }
mkdir -p "$P/out/gen"

# -I../32bit -I../shared: the plugin is built from the 32-bit sources where the code is the same on both sides
# (the upscaling bridge, the cloth banners, the self-update, the settings panel) and from shared/version.h, the one
# version number of the one release.
I32="-I$R/32bit -I$R/shared"

"$CC" -O2 -DNDEBUG -I"$MI/include" -c "$MI/src/static.c" -o "$P/out/mimalloc.o"

python -I "$P/tools/gen_scriptext64.py" "$R" > /dev/null   # src/scriptext64.c from 32bit/scriptext.c
python -I "$P/tools/gen_shared64.py" "$R"                  # out/gen: version.h, update.c, fgpanel.c
python -I "$P/tools/gen_flagwave64.py" "$R"                # src/flagwave64.c from src/flagwave.c (Flag Wave's plugin)

# Flag Wave: the cloth banners, built into the plugin rather than shipped as a DLL of its own
FW="-O2 -Wall -Wextra -Werror -Wno-cast-function-type -Wno-missing-field-initializers $I32"
$CC $FW -c "$P/src/flagwave64.c" -o "$P/out/flagwave64.o"
$CC $FW -c "$P/src/flagsim64.c"  -o "$P/out/flagsim.o"     # ../shared/flagsim.c plus fs64_forget_models

# the upscaling add-on's game side: 32bit/bridgeprobe.c (+ its bp_*.inc.c parts), fgbridge.c and fgmotion.c, with the
# changes the 64-bit loader and the Linux game file need
python -I "$P/tools/gen_ctxwrap64.py" "$D3D11_H" "$P/out/gen/ctxwrap64.c"
python -I "$P/tools/gen_bridge64.py" "$R" "$P/out/gen"
BX="-O2 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-but-set-global -Wno-missing-field-initializers -DFSR -DRELEASE -I$P/src $I32"
$CC $BX -c "$P/out/gen/bridge64.c"   -o "$P/out/bridge64.o"
$CC $BX -c "$P/src/fgbridge64.c"     -o "$P/out/fgbridge64.o"
$CC $BX -c "$P/out/gen/fgmotion64.c" -o "$P/out/fgmotion64.o"
$CC -O2 -Wall -Wextra -Werror -c "$P/out/gen/ctxwrap64.c" -o "$P/out/ctxwrap64.o"

SRC="$P/src/perffix64.c $P/src/x87fix64.c $P/src/texfix64.c $P/src/pool64.c $P/src/sortfix64.c $P/src/batchfix64.c
     $P/src/heightcache64.c $P/src/partcull64.c $P/src/uikeep64.c $P/src/loopfetch64.c $P/src/soundguard64.c
     $P/src/d3d64.c $P/src/scriptext64.c $P/src/panel64.c $P/src/uicache64.c $P/src/update64.c $P/src/slots64.c
     $P/src/loader64.c $P/src/crash64.c"
OBJ="$P/out/mimalloc.o $P/out/flagwave64.o $P/out/flagsim.o $P/out/bridge64.o $P/out/fgbridge64.o
     $P/out/fgmotion64.o $P/out/ctxwrap64.o"
$CC -O2 -Wall -Wextra -Werror -shared -I"$P/src" $SRC $OBJ -I"$MI/include" $I32 -DFSR "$P/out/gen/fgpanel.c" \
    -lbcrypt -ladvapi32 -luser32 -lgdi32 -ld3d11 -ldxgi -ldxguid -luuid -o "$P/out/perffix64_plugin.dll"
echo "built $P/out/perffix64_plugin.dll"
