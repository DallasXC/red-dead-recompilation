# update.ps1 - update this project to a newer release and rebuild the game.
#
# Started by the launcher's "Update now" (About page) when a newer release is
# out, in a window of its own, after the game has closed:
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\update.ps1 -Tag v1.2.0 -WaitPid <the game's>
#
# 1. waits for the game to close (it is being replaced);
# 2. downloads the release's source zip from GitHub (over HTTPS, from
#    github.com/DallasXC/red-dead-recompilation only) and checks that it is
#    this project at that version;
# 3. copies its files over the project's: only the project's own files are
#    replaced (the play folder's settings, saves, mods and logs, the build
#    tools in deps\ and your game files are not in the zip, so they stay),
#    and files a release has dropped (tools\removed_files.txt) are deleted;
# 4. runs the usual build (tools\build.ps1 -Yes: the compiler you chose
#    before, nothing asked) and starts the game again when it is done.
#
# Everything is logged to "Red Dead Recompilation\Logs\update.log". If the
# build fails, the new files stay: run build.bat to try again.
param(
    [Parameter(Mandatory = $true)][string]$Tag,
    [int]$WaitPid = 0,
    [switch]$NoStart
)
$ErrorActionPreference = 'Stop'
$Repo = 'DallasXC/red-dead-recompilation'
$Here = Split-Path -Parent $PSScriptRoot
$Play = Join-Path $Here 'Red Dead Recompilation'
$Logs = Join-Path $Play 'Logs'
New-Item -ItemType Directory -Force -Path $Logs | Out-Null
try { Start-Transcript -Path (Join-Path $Logs 'update.log') -Force | Out-Null } catch { }
$Host.UI.RawUI.WindowTitle = "Red Dead Recompilation: updating to $Tag"

function Stop-Update([string]$why) {
    Write-Host ''
    Write-Host "    The update stopped: $why" -ForegroundColor Red
    Write-Host '    Nothing has been built. Your game, settings and saves are as they were.'
    try { Stop-Transcript | Out-Null } catch { }
    Read-Host '    Press Enter to close'
    exit 1
}

Write-Host ''
Write-Host "    Red Dead Recompilation: updating to $Tag" -ForegroundColor Green
Write-Host "    project: $Here"

if ($Tag -notmatch '^v?\d+(\.\d+){1,3}$') { Stop-Update "'$Tag' is not a release." }
$Version = $Tag.TrimStart('v')
if (-not (Test-Path (Join-Path $Here 'build.bat')) -or -not (Test-Path (Join-Path $Here 'tools\build.ps1'))) {
    Stop-Update "this is not the project folder ($Here)."
}

# 1. the game closes first: its exe is about to be replaced
if ($WaitPid) {
    Write-Host '    waiting for the game to close...'
    $p = Get-Process -Id $WaitPid -ErrorAction SilentlyContinue
    if ($p -and -not $p.WaitForExit(60000)) { Stop-Update 'the game did not close.' }
}

# 2. the release's source zip
$Work = Join-Path $Here 'deps\update'
if (Test-Path $Work) { Remove-Item -Recurse -Force $Work }
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Zip = Join-Path $Work "$Tag.zip"
$Url = "https://github.com/$Repo/archive/refs/tags/$Tag.zip"
Write-Host "    downloading $Url"
try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $Url -OutFile $Zip -UseBasicParsing
} catch { Stop-Update "the download failed ($($_.Exception.Message))." }
try { Expand-Archive -Path $Zip -DestinationPath (Join-Path $Work 'src') -Force }
catch { Stop-Update "the download is not a readable zip ($($_.Exception.Message))." }
$Top = @(Get-ChildItem -Path (Join-Path $Work 'src') -Directory)
if ($Top.Count -ne 1) { Stop-Update 'the download does not look like this project.' }
$New = $Top[0].FullName
foreach ($f in 'build.bat', 'tools\build.ps1', 'VERSION', 'src\main.c') {
    if (-not (Test-Path (Join-Path $New $f))) { Stop-Update "the download has no $($f): it is not this project." }
}
$got = (Get-Content (Join-Path $New 'VERSION') | Select-Object -First 1).Trim()
if ($got -ne $Version) { Stop-Update "the download says it is $got, not $Version." }
Write-Host "    downloaded $Version"

# 3. the new files over the old (robocopy: 0-7 are success)
Write-Host '    copying the new files into the project...'
robocopy $New $Here /E /R:2 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
if ($LASTEXITCODE -ge 8) { Stop-Update "the files could not be copied (robocopy $LASTEXITCODE)." }
$Removed = Join-Path $Here 'tools\removed_files.txt'
if (Test-Path $Removed) {
    foreach ($line in Get-Content $Removed) {
        $rel = $line.Trim()
        if (-not $rel -or $rel.StartsWith('#') -or $rel.Contains('..') -or [IO.Path]::IsPathRooted($rel)) { continue }
        $path = Join-Path $Here $rel
        if (Test-Path $path -PathType Leaf) {
            Remove-Item -Force $path
            Write-Host "    removed $rel (no longer part of the project)"
        }
    }
}
Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue

# 4. the build, as build.bat does it, with nothing asked
Write-Host ''
Write-Host '    building: a few minutes, up to 30 when the game code is regenerated.' -ForegroundColor Green
Write-Host ''
$ps = Join-Path $PSHOME 'powershell.exe'
& $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Here 'tools\build.ps1') -Yes
if ($LASTEXITCODE -ne 0) {
    Write-Host ''
    Write-Host "    The new files ($Version) are in, but the build failed (above)." -ForegroundColor Red
    Write-Host '    Run build.bat to try again; send Logs\update.log if it fails again.'
    try { Stop-Transcript | Out-Null } catch { }
    Read-Host '    Press Enter to close'
    exit 1
}

Write-Host ''
Write-Host "    Updated to $Version." -ForegroundColor Green
try { Stop-Transcript | Out-Null } catch { }
$Exe = Join-Path $Play 'Red Dead Revolver.exe'
if (-not $NoStart -and (Test-Path $Exe)) {
    Start-Process -FilePath $Exe -WorkingDirectory $Play
} else {
    Read-Host '    Press Enter to close'
}
exit 0
