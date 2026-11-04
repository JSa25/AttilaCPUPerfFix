#!/usr/bin/env bash
# Linux counterpart of build.bat; llvm-mingw 20261006 UCRT.
set -euo pipefail
cd -- "$(dirname -- "$0")"
: "${LLVM_MINGW:?Set LLVM_MINGW to llvm-mingw 20261006 UCRT root}"
cc="$LLVM_MINGW/bin/i686-w64-mingw32-clang"
cxx="$LLVM_MINGW/bin/i686-w64-mingw32-clang++"
"$cc" --version | head -n 1
mkdir -p build/include
# ReShade uses Windows casing; llvm-mingw Linux headers use windows.h.
printf '#include <windows.h>\n' > build/include/Windows.h
"$cxx" -std=c++17 -O2 -Wall -Wextra -Ibuild/include -isystem reshade -fno-exceptions -fno-rtti -fno-threadsafe-statics -c -o build/rslink.o rslink.cpp
"$LLVM_MINGW/bin/i686-w64-mingw32-windres" -O coff -o build/dinput8_res.o dinput8.rc
"$cc" -O2 -Wall -Wextra -shared -static -Wl,-Xlink=-timestamp:1793660000 -o build/dinput8_6.10.5_B.dll heapfix.c pool.c sortfix.c fps.c batchfix.c campfix.c x87fix.c heightcache.c memfast.c loopfetch.c diag.c update.c fileio.c attrcache.c texfix.c logtrim.c uikeep.c pools.c passtime.c gputime.c bridgeprobe.c fgbridge.c fgmotion.c fgpanel.c build/rslink.o build/dinput8_res.o dinput8.def -lkernel32 -lwinmm -ladvapi32 -lpsapi -lbcrypt -lgdi32
cp build/dinput8_6.10.5_B.dll build/dinput8.dll
sha256sum build/dinput8.dll
