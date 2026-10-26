@echo off
rem Dump the imports of a game DLL (handy when adding a new game).
rem Run this from a "Developer Command Prompt" (needs dumpbin on PATH).
rem Usage: tools\imports.bat path\to\Game.dll
setlocal
if "%~1"=="" echo usage: %~nx0 path\to\Game.dll & exit /b 1
where dumpbin >nul 2>&1
if errorlevel 1 (
    echo dumpbin not found - open a "Developer Command Prompt for VS" and retry.
    exit /b 1
)
dumpbin /imports "%~1"
