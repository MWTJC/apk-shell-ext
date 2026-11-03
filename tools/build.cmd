@echo off
rem Build entry: MSVC env + CMake configure + compile.
rem Usage: tools\build.cmd [Release^|Debug] [--clean]
rem NOTE: keep this file ASCII-only; cmd.exe reads it in the OEM codepage.
setlocal
set VSLANG=1033
set VCVARS=E:\PROGRAMS\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" (
  where vswhere >nul 2>nul
  echo [build] vcvarsall not found at %VCVARS%, please edit tools\build.cmd
  exit /b 1
)
call "%VCVARS%" x64 >nul
if errorlevel 1 (
  echo [build] vcvarsall failed
  exit /b 1
)

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
if "%CONFIG%"=="--clean" (
  set CONFIG=Release
  if exist build rmdir /s /q build
)
if "%2"=="--clean" if exist build rmdir /s /q build

cd /d "%~dp0.."
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 exit /b 1
cmake --build build
if errorlevel 1 exit /b 1
echo [build] done
exit /b 0
