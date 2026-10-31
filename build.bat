@echo off
rem Builds dinput8.dll (and with "build.bat test <path to empire.retail.dll>" also runs the tests).
rem Needs llvm-mingw (its bin folder on PATH). See README.md.
setlocal
cd /d "%~dp0"
if "%CC%"=="" set "CC=i686-w64-mingw32-clang"
if "%CXX%"=="" set "CXX=i686-w64-mingw32-clang++"
if not exist build mkdir build
set "LIBS=-lkernel32 -lwinmm -ladvapi32 -lpsapi -lbcrypt"

rem rslink.cpp: the add-on's ReShade link (C++, ReShade's add-on API headers in reshade\; no C++ runtime linked)
"%CXX%" -std=c++17 -O2 -Wall -Wextra -isystem reshade -fno-exceptions -fno-rtti -fno-threadsafe-statics -c -o build\rslink.o rslink.cpp || exit /b 1
rem --no-insert-timestamp keeps the build reproducible: the same compiler gives the same file
"%CC%" -O2 -Wall -Wextra -shared -s -static -Wl,--no-insert-timestamp -o build\dinput8.dll heapfix.c pool.c sortfix.c fps.c batchfix.c campfix.c x87fix.c heightcache.c memfast.c loopfetch.c diag.c update.c fileio.c attrcache.c texfix.c logtrim.c uikeep.c pools.c passtime.c gputime.c bridgeprobe.c fgbridge.c fgmotion.c fgpanel.c build\rslink.o dinput8.def %LIBS% -lgdi32 || exit /b 1
echo built build\dinput8.dll

if /i not "%~1"=="test" exit /b 0
if "%~2"=="" (
    echo usage: build.bat test "path\to\empire.retail.dll"
    exit /b 1
)
for %%T in (pool sort batchfix campfix x87fix heightcache memfast loopfetch diag selfupd fileio attrcache texfix) do (
    "%CC%" -O2 -Wall -static -o build\%%T_test.exe tests\%%T_test.c %LIBS% || exit /b 1
)
"%CC%" -O2 -Wall -static -o build\heapfix_test.exe tests\heapfix_test.c pools.c %LIBS% || exit /b 1
for %%T in (heapfix pool sort batchfix campfix x87fix heightcache memfast loopfetch diag selfupd fileio attrcache texfix) do (
    echo == %%T
    build\%%T_test.exe "%~2" || (echo %%T test FAILED & exit /b 1)
)
"%CC%" -O2 -Wall -static -o build\lateload_test.exe tests\lateload_test.c %LIBS% || exit /b 1
echo == lateload
build\lateload_test.exe "%~dp0build\dinput8.dll" "%~2" || (echo lateload test FAILED & exit /b 1)
"%CC%" -O2 -Wall -static -o build\logtrim_test.exe tests\logtrim_test.c logtrim.c %LIBS% || exit /b 1
echo == logtrim
build\logtrim_test.exe || (echo logtrim test FAILED & exit /b 1)
"%CC%" -O2 -Wall -static -o build\uikeep_test.exe tests\uikeep_test.c %LIBS% || exit /b 1
echo == uikeep
build\uikeep_test.exe || (echo uikeep test FAILED & exit /b 1)
rem the small-block heap's checks: a block freed twice or written past its end stops the program (exit 3 or 4)
"%CC%" -O2 -Wall -static -o build\pools_check_test.exe tests\pools_check_test.c pools.c %LIBS% || exit /b 1
for %%C in (double overrun) do (
    echo == pools_check %%C
    build\pools_check_test.exe %%C >nul
    if errorlevel 5 (echo pools_check %%C test FAILED & exit /b 1)
    if not errorlevel 3 (echo pools_check %%C test FAILED & exit /b 1)
)
echo all tests passed
