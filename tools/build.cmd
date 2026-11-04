@echo off
rem Build entry: locate MSVC, configure with CMake + Ninja, compile.
rem
rem Usage: tools\build.cmd [Release^|Debug] [--clean]
rem
rem Toolchain discovery order (no editing needed on a normal machine):
rem   1) %APKSX_VCVARS%                - explicit override, full path of vcvarsall.bat
rem   2) vswhere -latest -products *   - any Visual Studio with the C++ x64 workload
rem   3) Program Files\...\2022 and 2019 common layouts
rem NOTE: keep this file ASCII-only; cmd.exe reads it in the OEM codepage.
rem NOTE: %ProgramFiles(x86)% must never appear inside a ( ... ) block - its parenthesis
rem       closes the block and cmd reports "was unexpected at this time".
setlocal
set VSLANG=1033

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
if /I "%CONFIG%"=="--clean" set CONFIG=Release
set CLEAN=0
if /I "%1"=="--clean" set CLEAN=1
if /I "%2"=="--clean" set CLEAN=1

set VCVARS=
if not "%APKSX_VCVARS%"=="" if exist "%APKSX_VCVARS%" set VCVARS=%APKSX_VCVARS%

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VSINSTALL=
if "%VCVARS%"=="" if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSINSTALL=%%i
if "%VCVARS%"=="" if defined VSINSTALL if exist "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat

if "%VCVARS%"=="" if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
if "%VCVARS%"=="" if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat
if "%VCVARS%"=="" if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat
if "%VCVARS%"=="" if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat

if "%VCVARS%"=="" call :no_msvc
if "%VCVARS%"=="" exit /b 1

echo [build] vcvarsall: %VCVARS%
call "%VCVARS%" x64 >nul
if errorlevel 1 (
  echo [build] vcvarsall failed
  exit /b 1
)

cd /d "%~dp0.."
if "%CLEAN%"=="1" if exist build rmdir /s /q build

cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=%CONFIG%
if errorlevel 1 exit /b 1
cmake --build build
if errorlevel 1 exit /b 1
echo [build] done
exit /b 0

:no_msvc
echo [build] no MSVC found. Install "Desktop development with C++", or point APKSX_VCVARS
echo [build] at vcvarsall.bat, for example:
echo [build]   set APKSX_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
exit /b 1
