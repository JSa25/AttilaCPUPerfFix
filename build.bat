@echo off
rem Builds dinput8.dll (and with "build.bat test <path to empire.retail.dll>" also runs the tests).
rem Needs llvm-mingw (its bin folder on PATH) and the mimalloc 3.5.3 source in .\mimalloc
rem (or set MIMALLOC to its folder). See README.md.
setlocal
cd /d "%~dp0"
if "%CC%"=="" set "CC=i686-w64-mingw32-clang"
if "%MIMALLOC%"=="" set "MIMALLOC=mimalloc"
if not exist "%MIMALLOC%\src\static.c" (
    echo mimalloc source not found in "%MIMALLOC%". See README.md.
    exit /b 1
)
if not exist build mkdir build
set "LIBS=-lkernel32 -lwinmm -ladvapi32 -lpsapi -lbcrypt"

"%CC%" -O2 -DNDEBUG -I"%MIMALLOC%\include" -c "%MIMALLOC%\src\static.c" -o build\mimalloc.o || exit /b 1
rem --no-insert-timestamp keeps the build reproducible: the same compiler gives the same file
"%CC%" -O2 -Wall -Wextra -I"%MIMALLOC%\include" -shared -s -static -Wl,--no-insert-timestamp -o build\dinput8.dll heapfix.c pool.c sortfix.c fps.c batchfix.c campfix.c x87fix.c heightcache.c memfast.c loopfetch.c diag.c update.c build\mimalloc.o dinput8.def %LIBS% || exit /b 1
echo built build\dinput8.dll

if /i not "%~1"=="test" exit /b 0
if "%~2"=="" (
    echo usage: build.bat test "path\to\empire.retail.dll"
    exit /b 1
)
for %%T in (pool sort batchfix campfix x87fix heightcache memfast loopfetch diag selfupd) do (
    "%CC%" -O2 -Wall -static -o build\%%T_test.exe tests\%%T_test.c %LIBS% || exit /b 1
)
"%CC%" -O2 -Wall -I"%MIMALLOC%\include" -static -o build\heapfix_test.exe tests\heapfix_test.c build\mimalloc.o %LIBS% || exit /b 1
for %%T in (heapfix pool sort batchfix campfix x87fix heightcache memfast loopfetch diag selfupd) do (
    echo == %%T
    build\%%T_test.exe "%~2" || (echo %%T test FAILED & exit /b 1)
)
"%CC%" -O2 -Wall -static -o build\lateload_test.exe tests\lateload_test.c %LIBS% || exit /b 1
echo == lateload
build\lateload_test.exe "%~dp0build\dinput8.dll" "%~2" || (echo lateload test FAILED & exit /b 1)
echo all tests passed
