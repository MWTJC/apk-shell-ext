@echo off
rem Packaging entry: sync the installer's version from src\version.h, then wrap the binaries
rem into one Inno Setup exe.
rem
rem Usage: tools\pack.cmd [x64^|arm64] [--build]
rem   x64    (default) pack out\x64  -> apk-shell-ext-setup-<ver>-x64.exe
rem   arm64  pack out\arm64 -> apk-shell-ext-setup-<ver>-arm64.exe
rem   --build   also run tools\build.cmd Release <arch> first
rem
rem Notes:
rem   * One installer per architecture (ADR-0010); the file name carries the arch.
rem   * The version lives only in src\version.h; sync-version.ps1 copies it into the .iss
rem     (run it with -Check to verify without writing).
rem   * ISCC lookup: %ISCC% -> where ISCC.exe -> Program Files\(x86)\Inno Setup 6 -> this machine's path.
rem   * Output: out\dist\apk-shell-ext-setup-<version>-<arch>.exe (LZMA2).
rem Keep this file ASCII-only (cmd.exe reads it in the OEM code page).
setlocal
set ROOT=%~dp0..
set ARCH=x64
set DO_BUILD=0

:parse
if "%~1"=="" goto parsed
if /I "%~1"=="x64" (set "ARCH=x64" & shift & goto parse)
if /I "%~1"=="arm64" (set "ARCH=arm64" & shift & goto parse)
if /I "%~1"=="--build" (set "DO_BUILD=1" & shift & goto parse)
echo [pack] unknown argument: %~1 (expected x64/arm64, --build)
exit /b 1
:parsed

set ISCC_DEFINE=
if /I "%ARCH%"=="arm64" set ISCC_DEFINE=/DBUILD_ARM64

if "%DO_BUILD%"=="1" (
  call "%ROOT%\tools\build.cmd" Release %ARCH%
  if errorlevel 1 exit /b 1
)

if not exist "%ROOT%\out\%ARCH%\apkthumb.dll" (
  echo [pack] out\%ARCH% has no binaries - run tools\build.cmd Release %ARCH% first
  exit /b 1
)
if not exist "%ROOT%\icons\apk.ico" (
  echo [pack] icons\apk.ico missing - run: uv run --with pillow python tools\gen_icons.py
  exit /b 1
)

echo [pack] syncing installer version from src\version.h
powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\tools\sync-version.ps1"
if errorlevel 1 (
  echo [pack] version sync failed
  exit /b 1
)

set ISCC=%ISCC%
if "%ISCC%"=="" for /f "delims=" %%i in ('where ISCC.exe 2^>nul') do set ISCC=%%i
if "%ISCC%"=="" if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe
if "%ISCC%"=="" if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe
if "%ISCC%"=="" if exist "E:\PROGRAMS\Inno Setup 6\ISCC.exe" set ISCC=E:\PROGRAMS\Inno Setup 6\ISCC.exe
if "%ISCC%"=="" (
  echo [pack] ISCC.exe not found - install Inno Setup 6 or set the ISCC environment variable
  exit /b 1
)

echo [pack] compiling installer\apk-shell-ext.iss (arch=%ARCH%)
"%ISCC%" /Q %ISCC_DEFINE% "%ROOT%\installer\apk-shell-ext.iss"
if errorlevel 1 (
  echo [pack] ISCC failed
  exit /b 1
)
echo [pack] done:
dir /b "%ROOT%\out\dist\apk-shell-ext-setup-*-%ARCH%.exe"
exit /b 0
