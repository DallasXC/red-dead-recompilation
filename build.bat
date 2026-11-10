@echo off
rem Build rdr-recomp: installs what is missing, fetches the toolkit,
rem regenerates the C from your disc and compiles. Safe to run again.
rem No extracted disc yet? Drop your disc image (.iso) onto this file.
setlocal
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build.ps1" %*
set RC=%ERRORLEVEL%
echo.
if not "%RC%"=="0" echo Build failed. Scroll up for the error.
if "%RC%"=="0" echo Build finished. Play from the "Red Dead Recompilation" folder inside this one.
pause
exit /b %RC%
