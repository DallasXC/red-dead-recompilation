@echo off
rem Start Red Dead Revolver (recompiled). Builds first if needed.
rem The game folder defaults to "..\Red Dead Revolver (USA)"; pass another as the first argument.
setlocal
cd /d "%~dp0"
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%~dp0..\Red Dead Revolver (USA)"
if not exist "%GAME%\default.xbe" set "GAME=%~dp0Red Dead Recompilation\Game"
set "EXE=%~dp0build\Release\rdr_recomp.exe"
rem (built with build.bat -Compiler clang: the LLVM-MinGW folder)
if not exist "%EXE%" if exist "%~dp0build-clang\Release\rdr_recomp.exe" set "EXE=%~dp0build-clang\Release\rdr_recomp.exe"
if not exist "%EXE%" (
  echo The game has not been built yet - building it now.
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build.ps1" -Game "%GAME%"
  if errorlevel 1 ( echo Build failed. & pause & exit /b 1 )
)
if not exist "%~dp0logs" mkdir "%~dp0logs"
rem Keep the run before this one: a crash log is not lost to the next start.
if exist "%~dp0logs\last_run.log" move /y "%~dp0logs\last_run.log" "%~dp0logs\previous_run.log" >nul
set "SAVES=%~dp0saves"
if not exist "%SAVES%" mkdir "%SAVES%"
if "%RECOMP_SAVE_DIR%"=="" set "RECOMP_SAVE_DIR=%SAVES%"
if "%RDR_SETTINGS%"=="" set "RDR_SETTINGS=%~dp0settings.ini"
rem Mods: a file in mods\ at the same path as one in the game folder is used
rem instead of it (see docs\MODDING.md).
if not exist "%~dp0mods" mkdir "%~dp0mods"
if "%RECOMP_MODS_DIR%"=="" set "RECOMP_MODS_DIR=%~dp0mods"
rem Texture packs: .tga files named by the texture's hash, in mods\textures
rem (subfolders too). See docs\MODDING.md.
if not exist "%~dp0mods\textures" mkdir "%~dp0mods\textures"
if "%RECOMP_TEXTURE_PACK%"=="" set "RECOMP_TEXTURE_PACK=%~dp0mods\textures"
echo Starting Red Dead Revolver... (log: logs\last_run.log)
"%EXE%" "%GAME%" %2 %3 > "%~dp0logs\last_run.log" 2>&1
set RC=%ERRORLEVEL%
if not "%RC%"=="0" (
  echo.
  echo The game exited with code %RC%. The log is in logs\last_run.log
  pause
)
exit /b %RC%
