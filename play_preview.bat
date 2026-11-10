@echo off
rem Red Dead Revolver with the January 2002 preview's characters and stages
rem switched on for this start (mods\Preview, made by make_preview.bat or the
rem launcher's Extras page), whatever settings.ini says.
setlocal
if not exist "%~dp0mods\Preview\swat.dat" ( echo Set up the preview characters first: make_preview.bat & pause & exit /b 1 )
set "RDR_PREVIEW=1"
call "%~dp0play.bat" %*
