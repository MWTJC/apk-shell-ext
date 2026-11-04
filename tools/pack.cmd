@echo off
rem Packaging entry (M5): build once, then wrap the binaries into one Inno Setup exe.
rem Usage: tools\pack.cmd [--build]
rem   --build   also run tools\build.cmd Release first
rem Notes:
rem   * ISCC path comes from %ISCC% or the default location; edit below if Inno moves.
rem   * Output: out\dist\apk-shell-ext-setup-<version>.exe (single file, LZMA2).
rem Keep this file ASCII-only (cmd.exe reads it in the OEM code page).
setlocal
set ROOT=%~dp0..
set ISCC=%ISCC%
if "%ISCC%"=="" set ISCC=E:\PROGRAMS\Inno Setup 6\ISCC.exe
if not exist "%ISCC%" (
  echo [pack] ISCC.exe not found at "%ISCC%" - set the ISCC environment variable
  exit /b 1
)

if /I "%1"=="--build" (
  call "%ROOT%\tools\build.cmd" Release
  if errorlevel 1 exit /b 1
)

if not exist "%ROOT%\out\x64\apkthumb.dll" (
  echo [pack] out\x64 has no binaries - run tools\build.cmd Release first
  exit /b 1
)
if not exist "%ROOT%\icons\apk.ico" (
  echo [pack] icons\apk.ico missing - run: uv run --with pillow python tools\gen_icons.py
  exit /b 1
)

echo [pack] compiling installer\apk-shell-ext.iss
"%ISCC%" /Q "%ROOT%\installer\apk-shell-ext.iss"
if errorlevel 1 (
  echo [pack] ISCC failed
  exit /b 1
)
echo [pack] done:
dir /b "%ROOT%\out\dist\*.exe"
exit /b 0
