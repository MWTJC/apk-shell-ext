@echo off
rem Print import table of the built binaries (run inside vcvars).
setlocal
call "E:\PROGRAMS\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
cd /d "%~dp0.."
dumpbin /nologo /dependents "out\x64\apkthumb.dll"
echo ---EXPORTS---
dumpbin /nologo /exports "out\x64\apkthumb.dll" | findstr /i "DllGetClassObject DllCanUnloadNow"
echo ---SECTIONS---
dumpbin /nologo /headers "out\x64\apkthumb.dll" | findstr /i "guard cf"
