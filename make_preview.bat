@echo off
rem Set up the January 2002 preview's characters and stages (and the story's
rem own people) for Showdown and Play As, into mods\Preview: the same as the
rem launcher's Extras page does (tools\mods\setup_preview.py). Needs Python 3
rem (numpy and Pillow are installed if missing) and the preview's disc image:
rem pass its path, drop it onto this file, or put it in "..\PS2 Beta".
rem Then switch them on: play_preview.bat, or the launcher's Extras page.
setlocal
cd /d "%~dp0"
set "PREVIEW=%~1"
if "%PREVIEW%"=="" for %%f in ("%~dp0..\PS2 Beta\*.iso") do set "PREVIEW=%%f"
if "%PREVIEW%"=="" ( echo No preview disc image found in "..\PS2 Beta" - drop it onto this file. & pause & exit /b 1 )
set "GAME=%~dp0..\Red Dead Revolver (USA)"
python "%~dp0tools\mods\setup_preview.py" "%PREVIEW%" "%GAME%" "%~dp0mods\Preview"
if errorlevel 1 ( echo Failed. & pause & exit /b 1 )
echo Done. Start play_preview.bat, or switch them on in the launcher's Extras page.
pause
