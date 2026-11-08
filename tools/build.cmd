@echo off
rem Build entry: locate MSVC, configure with CMake + Ninja, compile.
rem
rem Usage: tools\build.cmd [Release^|Debug] [x64^|arm64] [--clean]
rem   x64    (default) native build, output out\x64
rem   arm64  cross build on an x64 host via vcvarsall x64_arm64, output out\arm64
rem
rem Toolchain discovery order (no editing needed on a normal machine):
rem   1) %APKSX_VCVARS%                - explicit override, full path of vcvarsall.bat
rem   2) vswhere -latest -products *   - any Visual Studio with the C++ tools for the target
rem   3) Program Files\...\2022 and 2019 common layouts
rem NOTE: keep this file ASCII-only; cmd.exe reads it in the OEM codepage.
rem NOTE: %ProgramFiles(x86)% must never appear inside a ( ... ) block - its parenthesis
rem       closes the block and cmd reports "was unexpected at this time".
setlocal
set VSLANG=1033
rem Capture the script dir before any shift: cmd's shift also moves %0, so %~dp0 stops
rem being the script path afterwards.
set "ROOT=%~dp0.."

set CONFIG=Release
set ARCH=x64
set CLEAN=0

:parse
if "%~1"=="" goto parsed
if /I "%~1"=="Release" (set "CONFIG=Release" & shift & goto parse)
if /I "%~1"=="Debug" (set "CONFIG=Debug" & shift & goto parse)
if /I "%~1"=="x64" (set "ARCH=x64" & shift & goto parse)
if /I "%~1"=="arm64" (set "ARCH=arm64" & shift & goto parse)
if /I "%~1"=="--clean" (set "CLEAN=1" & shift & goto parse)
echo [build] unknown argument: %~1 (expected Release/Debug, x64/arm64, --clean)
exit /b 1
:parsed

rem vcvarsall target, the VS component that must be installed for it, and the CMake build dir
set VCVARS_TARGET=x64
set VS_COMPONENT=Microsoft.VisualStudio.Component.VC.Tools.x86.x64
set BUILD_DIR=build
if /I "%ARCH%"=="arm64" set VCVARS_TARGET=x64_arm64
if /I "%ARCH%"=="arm64" set VS_COMPONENT=Microsoft.VisualStudio.Component.VC.Tools.ARM64
if /I "%ARCH%"=="arm64" set BUILD_DIR=build-arm64

set VCVARS=
if not "%APKSX_VCVARS%"=="" if exist "%APKSX_VCVARS%" set VCVARS=%APKSX_VCVARS%

set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set VSINSTALL=
if "%VCVARS%"=="" if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires %VS_COMPONENT% -property installationPath`) do set VSINSTALL=%%i
if "%VCVARS%"=="" if defined VSINSTALL if exist "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat

rem Fallback: any VS with C++ tools. vcvarsall fails loudly below when the target arch is missing.
if "%VCVARS%"=="" if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set VSINSTALL=%%i
if "%VCVARS%"=="" if defined VSINSTALL if exist "%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%VSINSTALL%\VC\Auxiliary\Build\vcvarsall.bat

if "%VCVARS%"=="" if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
if "%VCVARS%"=="" if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat
if "%VCVARS%"=="" if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat
if "%VCVARS%"=="" if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" set VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat

if "%VCVARS%"=="" call :no_msvc
if "%VCVARS%"=="" exit /b 1

echo [build] arch=%ARCH% config=%CONFIG%
echo [build] vcvarsall: %VCVARS% %VCVARS_TARGET%
call "%VCVARS%" %VCVARS_TARGET% >nul
if errorlevel 1 (
  echo [build] vcvarsall %VCVARS_TARGET% failed
  echo [build]   arm64 needs the "MSVC v143+ - C++ ARM64 build tools" component
  echo [build]   ^(Visual Studio Installer - Individual components^)
  exit /b 1
)

cd /d "%ROOT%"
if "%CLEAN%"=="1" if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"

cmake -G Ninja -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=%CONFIG% -DAPKSX_ARCH=%ARCH%
if errorlevel 1 exit /b 1
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1
echo [build] done: out\%ARCH%
exit /b 0

:no_msvc
echo [build] no MSVC found. Install "Desktop development with C++", or point APKSX_VCVARS
echo [build] at vcvarsall.bat, for example:
echo [build]   set APKSX_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
exit /b 1
