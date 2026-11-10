<#
  package.ps1 - the ready-to-play folder, assembled after each build.

  Run by build.ps1 (build.bat), or on its own:
    powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-Game <dir>] [-Out <dir>] [-Release <dir>]

  Makes (or refreshes) a folder inside this project:

    Red Dead Recompilation\
      Red Dead Revolver.exe          the game (built from YOUR disc here)
      SDL2.dll                       controller support
      libc++.dll  libunwind.dll ...  (LLVM-MinGW builds: its C++ runtime)
      Game\                          the game's files
      Saves\  Mods\  Logs\  settings.ini
      Tools\mods\                    the mod tools (Extras: the 2002 preview)
      README.txt

  The exe finds everything beside itself (see main.c, player_folder); no
  .bat is needed to play, or to install the game from its disc image.
  Game\ is linked to the extracted disc folder when there is one (a
  directory junction: nothing is copied); otherwise the first start asks
  for the disc image. The project's settings, saves and
  mods are COPIED in once, the first time -- never moved, never overwritten.
  Only the exe and the DLLs are replaced on later builds.
#>
param(
    [string]$Game,
    [string]$Out,
    [string]$Release      # the built exe's folder (build\Release; build-clang\Release for LLVM-MinGW)
)

$ErrorActionPreference = 'Stop'
$Here   = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Parent = Split-Path $Here -Parent
if (-not $Game) { $Game = Join-Path $Parent 'Red Dead Revolver (USA)' }
if (-not $Out)  { $Out  = Join-Path $Here 'Red Dead Recompilation' }

# Earlier builds made the folder beside the project: moved in, with its
# saves, settings and mods (a game still running keeps it where it is).
$OldOut = Join-Path $Parent 'Red Dead Recompilation'
if (($OldOut -ne $Out) -and (Test-Path (Join-Path $OldOut 'Red Dead Revolver.exe')) -and -not (Test-Path $Out)) {
    try {
        Move-Item -Path $OldOut -Destination $Out -ErrorAction Stop
        Write-Host "    moved the Red Dead Recompilation folder into $Here (saves, settings and mods with it)"
    } catch {
        Write-Host "    could not move $OldOut in (is the game running?): it is used where it is this time"
        $Out = $OldOut
    }
}
if (-not $Release) { $Release = Join-Path $Here 'build\Release' }
$Exe = Join-Path $Release 'rdr_recomp.exe'

function Info($text) { Write-Host "    $text" }

if (-not (Test-Path $Exe)) { Write-Host "    not built yet: $Exe"; exit 1 }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# The game and its DLLs: replaced every time (a running game keeps the old).
try {
    Copy-Item -Force $Exe (Join-Path $Out 'Red Dead Revolver.exe')
} catch {
    Info "could not replace Red Dead Revolver.exe (is the game running?)"
}
Get-ChildItem -Path $Release -Filter '*.dll' -ErrorAction SilentlyContinue | ForEach-Object {
    try { Copy-Item -Force $_.FullName (Join-Path $Out $_.Name) } catch {}
}

# No helper .bat files: the exe does it all -- with no game in Game\ it
# asks for the disc image, the launcher (settings) opens at every start
# until switched off, and online co-op is on the desk. The ones earlier
# builds wrote are taken away (only those, by name).
foreach ($old in @('Install Game from ISO.bat', 'Settings.bat', 'Host Online Game.bat', 'Join Online Game.bat')) {
    $f = Join-Path $Out $old
    if (Test-Path $f) { Remove-Item -Force $f; Info "removed $old (the exe does it now)" }
}

# The game's files: linked to the extracted disc when there is one.
$GameOut = Join-Path $Out 'Game'
if ((Test-Path $GameOut) -or ((Test-Path $Game) -and ((Resolve-Path $Game).Path -eq (Join-Path (Resolve-Path $Out).Path 'Game')))) {
    # there already (or the disc was unpacked straight into it)
} else {
    if (Test-Path (Join-Path $Game 'default.xbe')) {
        try { New-Item -ItemType Junction -Path $GameOut -Target $Game | Out-Null } catch {}
        if (Test-Path (Join-Path $GameOut 'default.xbe')) { Info "Game -> $Game" }
        else { Info "could not link Game to $Game; the first start will ask for the disc image" }
    } else {
        Info 'no extracted game found: the first start asks for the disc image'
    }
}

# Settings, saves and mods: copied once (the originals stay where they are).
$ini = Join-Path $Here 'settings.ini'
if ((Test-Path $ini) -and -not (Test-Path (Join-Path $Out 'settings.ini'))) {
    Copy-Item $ini (Join-Path $Out 'settings.ini')
    Info 'settings.ini copied'
}
foreach ($pair in @(@('saves', 'Saves'), @('mods', 'Mods'))) {
    $from = Join-Path $Here $pair[0]
    $to = Join-Path $Out $pair[1]
    if ((Test-Path $from) -and -not (Test-Path $to)) {
        Copy-Item -Recurse $from $to
        Info "$($pair[0]) copied to $($pair[1])"
    }
}
foreach ($d in @('Saves', 'Mods', 'Mods\textures', 'Logs')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $Out $d) | Out-Null
}

# The mod tools (Python): the launcher's Extras page runs them to set up
# the 2002 preview's characters (setup_preview.py). Replaced every time.
$ToolsOut = Join-Path $Out 'Tools\mods'
New-Item -ItemType Directory -Force -Path $ToolsOut | Out-Null
Get-ChildItem -Path (Join-Path $Here 'tools\mods') -Filter '*.py' | ForEach-Object {
    Copy-Item -Force $_.FullName (Join-Path $ToolsOut $_.Name)
}

Set-Content -Encoding ASCII -Path (Join-Path $Out 'README.txt') -Value @'
Red Dead Recompilation - Red Dead Revolver (Xbox) for PC
=========================================================

To play: start "Red Dead Revolver.exe".

The first time, with no game in the Game folder yet, the settings window
opens on an Install page: choose your Red Dead Revolver (USA) Xbox disc
image (.iso) and it is unpacked into Game\. (Dragging the .iso onto the
exe does the same.)

The settings window (the launcher) opens before the game every time until
you switch that off on its About page; hold Shift while starting the game
to open it again.

  F1 (or both sticks clicked)  the in-game menu: settings, cheats
  Online co-op                 on the desk (Y, or click it) once a player
                               is picked, or F1 > Online Co-op
  2002 preview characters      the launcher's Extras page: set up from the
                               preview's disc image, then switch on or off
  Saves\                       your saves
  Mods\                        mods, each in a folder of its own; texture
                               packs in Mods\textures, the same way. The
                               settings window's Mods page switches them
  Logs\last_run.log            the log of the last start (for bug reports)

Red Dead Revolver is (c) Rockstar Games / Take-Two Interactive. This port
is not affiliated with them and contains none of the game: bring your own.
'@

Write-Host "Ready to play: $Out" -ForegroundColor Green
exit 0
