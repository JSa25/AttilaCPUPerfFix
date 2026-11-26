#!/bin/sh
# Total War Mod Manager: a 32-bit exe (it uses the 32-bit game's own steam_api.dll), built with llvm-mingw.
#   ./build.sh  ->  build/TotalWarModManager.exe
# It includes the engine fix's ../32bit/update.c (Workshop updates), which reads ../shared/version.h.
# VERSION_DIR=<folder with a version.h> builds against another version.h (see README.md).
set -e
cd "$(dirname "$0")"
CC=${CC:-i686-w64-mingw32-clang}
WINDRES=${WINDRES:-i686-w64-mingw32-windres}
VERSION_DIR=${VERSION_DIR:-../shared}
mkdir -p build
$WINDRES launcher.rc -O coff -o build/launcher_res.o
# a fixed link timestamp keeps the build reproducible. Built with debug info as build/TotalWarModManager_syms.exe (it
# turns the offsets in a CRASH line of launcher.log into functions and lines); the exe is that file without the debug info
$CC -O2 -g -Wall -Wno-unused-parameter -Wno-unused-function -Wno-sign-compare -Wno-missing-field-initializers \
    -I. -I"$VERSION_DIR" -I../32bit launcher.c build/launcher_res.o -o build/TotalWarModManager_syms.exe \
    -mwindows -luser32 -lgdi32 -lcomctl32 -Wl,-Xlink=-timestamp:1795672727
${STRIP:-llvm-strip} --strip-debug -o build/TotalWarModManager.exe build/TotalWarModManager_syms.exe
ls -la build/TotalWarModManager.exe
