@echo off
rem Run the game once with diagnostics: records the emulated audio to
rem logs\audio.wav, logs controller/keyboard input once a second, and
rem every 2 s what each game thread last asked the kernel for.
rem Play as far as you like (Enter = Start, Z = A), then close the window.
rem Send logs\last_run.log (and audio.wav if asked).
setlocal
cd /d "%~dp0"
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%~dp0..\Red Dead Revolver (USA)"
set "EXE=%~dp0build\Release\rdr_recomp.exe"
rem (built with build.bat -Compiler clang: the LLVM-MinGW folder)
if not exist "%EXE%" if exist "%~dp0build-clang\Release\rdr_recomp.exe" set "EXE=%~dp0build-clang\Release\rdr_recomp.exe"
if not exist "%EXE%" ( echo Build first: run build.bat & pause & exit /b 1 )
if not exist "%~dp0logs" mkdir "%~dp0logs"
rem Keep the run before this one: a crash log is not lost to the next start.
if exist "%~dp0logs\last_run.log" move /y "%~dp0logs\last_run.log" "%~dp0logs\previous_run.log" >nul
if not exist "%~dp0saves" mkdir "%~dp0saves"
set "RECOMP_SAVE_DIR=%~dp0saves"
if "%RDR_SETTINGS%"=="" set "RDR_SETTINGS=%~dp0settings.ini"
rem Mods: a file in mods\ at the same path as one in the game folder is used
rem instead of it (see docs\MODDING.md).
if not exist "%~dp0mods" mkdir "%~dp0mods"
if "%RECOMP_MODS_DIR%"=="" set "RECOMP_MODS_DIR=%~dp0mods"
set "RECOMP_AUDIO_WAV=%~dp0logs\audio.wav"
set "RECOMP_AUDIO_WAV_SECS=120"
set "RECOMP_INPUT_DIAG=1"
set "RECOMP_KEY_TRACE=1"
set "RECOMP_USB_STATS=1"
set "RECOMP_THREAD_DUMP=1"
echo Diagnostic run. Play as far as you like, then close the window.
"%EXE%" "%GAME%" > "%~dp0logs\last_run.log" 2>&1
echo Done. Logs are in the logs folder.
pause
