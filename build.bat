@echo off
rem Build pcapload.exe (32-bit - required: the game DLLs are 32-bit PE32).
setlocal

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

rem VS 2017+: locate the install that has the C++ x86/x64 build tools.
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars32.bat"
if defined VCVARS goto :have_vc

rem VS 2015 / 2013 fallback (their vcvars32.bat lives in VC\bin).
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio 14.0\VC\bin\vcvars32.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio 14.0\VC\bin\vcvars32.bat"
if defined VCVARS goto :have_vc
if exist "%ProgramFiles(x86)%\Microsoft Visual Studio 12.0\VC\bin\vcvars32.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio 12.0\VC\bin\vcvars32.bat"
if defined VCVARS goto :have_vc

echo Could not find a 32-bit MSVC toolchain ^(vcvars32.bat^).
echo Install Visual Studio with the "Desktop development with C++" workload.
exit /b 1

:have_vc
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

if exist pcapload.exe del pcapload.exe
cl /nologo /W3 /O2 /D_CRT_SECURE_NO_WARNINGS /Fe:pcapload.exe src\main.c src\json.c src\cab.c src\registry.c /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib winmm.lib advapi32.lib
if errorlevel 1 exit /b 1
echo Built pcapload.exe OK.
