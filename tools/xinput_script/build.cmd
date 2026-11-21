@echo off
rem Builds the scripted XInput DLL into bin\runtime\xinput1_4.dll (llvm-mingw toolchain from the Road Rash project).
setlocal
if not defined BNB_TOOLCHAIN set "BNB_TOOLCHAIN=%~dp0..\..\llvm-mingw"
set "PATH=%BNB_TOOLCHAIN%\bin;%PATH%"
set "OUT=%~dp0..\..\bin\runtime\xinput1_4.dll"
clang -O2 -shared -o "%OUT%" "%~dp0xinput_script.c" -Wl,--kill-at -static-libgcc || exit /b 1
echo wrote %OUT%
