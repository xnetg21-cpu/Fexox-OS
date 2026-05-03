@echo off
setlocal enabledelayedexpansion

if exist make.exe (
    set MAKE=make.exe
) else if exist mingw32-make.exe (
    set MAKE=mingw32-make.exe
) else if exist gmake.exe (
    set MAKE=gmake.exe
) else (
    echo ERROR: make not found. Install GNU make or use MSYS2.
    exit /b 1
)

echo [BUILD] Running %MAKE% all
%MAKE% all
if errorlevel 1 (
    echo ERROR: Build failed.
    exit /b 1
)

echo [BUILD] Build succeeded.
if exist kernel.iso (
    echo [BUILD] kernel.iso created.
) else (
    echo WARNING: kernel.iso was not generated.
)

echo [INFO] You can run the ISO in VirtualBox or QEMU.
endlocal
