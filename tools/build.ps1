<#
  build.ps1 - one-step build of rdr-recomp on Windows.

  Run through build.bat (double-click, or drop your disc image on it), or:
    powershell -ExecutionPolicy Bypass -File tools\build.ps1 [-Game <dir>] [-Iso <image>] [-Clean] [-Yes]
                                                     [-Compiler clang]

  What it does, skipping anything already done:
    0. Without an extracted disc, unpacks your disc image (.iso) into
       Red Dead Recompilation\Game\ inside this folder: the one given, one
       found next to the project, or one you pick in a window.
    1. Checks for Git, Python 3 and Visual Studio 2022's C++ tools; offers to
       install whatever is missing with winget.
    2. Installs the Python package the toolkit needs (capstone).
    3. Clones xboxrecomp into deps\ here at the pinned version and applies
       patches\xboxrecomp-posix-x86_64-mmio.patch.
    4. Regenerates src\recomp\gen\ from YOUR default.xbe (only when the XBE or
       the toolkit changed).
    5. Fetches SDL2 (controller support beyond Xbox pads), then configures
       and compiles with CMake + MSVC.

  -Compiler clang (build.bat -Compiler clang): LLVM-MinGW instead of Visual
  Studio -- Clang and the MinGW-w64 headers and libraries in one folder
  (https://github.com/mstorsjo/llvm-mingw, about 180 MB to download, no
  installer, no admin), with CMake and Ninja from pip. Its build goes to
  build-clang\. The first build asks which compiler to use (Visual Studio
  is the default) and remembers the answer in deps\compiler.txt; it is
  offered too when Visual Studio is missing and not wanted.
  Its generated code is optimised fully (-O2: measured 4-8 % less CPU per
  frame, about 40 % more compile time).

  -FastCode: the same full optimisation (/O2) with Visual Studio too: the
  compile takes longer. Builds with and without it play online together.

  Everything it makes stays inside this folder:
    Red Dead Recompilation\              the game, ready to play
    Red Dead Recompilation\Game\        the game's files (unpacked from the disc)
    deps\xboxrecomp\  deps\SDL2-...\     the toolkit and SDL2
    build\  src\recomp\gen\              the compile
  An extracted disc beside this folder (..\Red Dead Revolver (USA)\, or any
  folder there with default.xbe and swat.dat) is used where it is, and a
  toolkit clone or play folder an earlier build made beside it is used (the
  play folder is moved in).
#>
param(
    [string]$Game,
    [string]$Iso,
    [switch]$Clean,
    [switch]$Yes,
    [switch]$AnyGame,
    [switch]$FastCode,
    [ValidateSet('', 'msvc', 'clang')]
    [string]$Compiler = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$XbrCommit = 'f0f8a461496a4f23d9b0aeaa284b8d70ba7ab82b'   # xboxrecomp v0.16.0
$XbrUrl    = 'https://github.com/sp00nznet/xboxrecomp.git'

$Here   = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Parent = Split-Path $Here -Parent
$Play   = Join-Path $Here 'Red Dead Recompilation'    # the ready-to-play folder (package.ps1)
$Deps   = Join-Path $Here 'deps'
# The toolkit: inside, unless an earlier build cloned it beside the project.
$Xbr    = Join-Path $Deps 'xboxrecomp'
if (-not (Test-Path (Join-Path $Xbr '.git')) -and (Test-Path (Join-Path $Parent 'xboxrecomp\.git'))) {
    $Xbr = Join-Path $Parent 'xboxrecomp'
}
$Gen    = Join-Path $Here 'src\recomp\gen'
$Build  = Join-Path $Here 'build'
$Patch  = Join-Path $Here 'patches\xboxrecomp-posix-x86_64-mmio.patch'
# (a disc image dropped on build.bat arrives as the first argument)
if ($Game -and -not $Iso -and $Game -match '\.iso$') { $Iso = $Game; $Game = $null }
if (-not $Game) {
    # the extracted disc beside the project (earlier layouts), else the
    # play folder's own Game\ (where a disc image is unpacked now)
    $Game = Join-Path $Parent 'Red Dead Revolver (USA)'
    if (-not (Test-Path (Join-Path $Game 'default.xbe'))) {
        $Game = Join-Path $Play 'Game'
        $oldPlayGame = Join-Path $Parent 'Red Dead Recompilation\Game'
        if (-not (Test-Path (Join-Path $Game 'default.xbe')) -and (Test-Path (Join-Path $oldPlayGame 'default.xbe'))) {
            $Game = $oldPlayGame
        }
    }
}

function Step($text) { Write-Host ''; Write-Host "==> $text" -ForegroundColor Cyan }
function Info($text) { Write-Host "    $text" }
function Fail($text) { Write-Host ''; Write-Host "ERROR: $text" -ForegroundColor Red; exit 1 }

# What each zip the build downloads must be (SHA-256). One that is not
# exactly this file is deleted and the build stops, rather than building
# with a damaged or swapped download. Checked against the publisher's own
# list (CMake) and against copies fetched on another network and PC
# (CMake, SDL2 both, LLVM-MinGW). The installers (Git, Python, Visual
# Studio) are checked by their publisher's signature instead (Download-Run),
# and winget checks its own downloads against its manifests.
$Sha256 = @{
    'cmake-3.31.6-windows-x86_64.zip'     = 'D163CD3AB4959B0A53FA8988F2DDBD2E6C501658201E6A154386BAD9DBE4F836'
    'ninja-win.zip'                        = 'F550FEC705B6D6FF58F2DB3C374C2277A37691678D6ABA463ADCBB129108467A'
    'SDL2-devel-2.30.9-VC.zip'            = '8C91D91E5BCB997D062EC2B553C53832EBF95654D4AA35E8C02A954D4CE752AE'
    'SDL2-devel-2.30.9-mingw.zip'         = '492ABBC78BBCAD000D224CC56A200938F8BE09A4A9AAE253DEFA6035CBF68D9D'
    'llvm-mingw-20250826-ucrt-x86_64.zip' = '51C347BF5DEAD6C4383FF16FA9BAEE362DACCEB70ECDA9448D2289C8E158AF61'
}
function Test-Download([string]$file, [string]$name) {
    $want = $Sha256[$name]
    if (-not $want) { Fail "no SHA-256 is listed for $name in build.ps1" }
    $got = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash
    if ($got -ne $want) {
        Remove-Item -LiteralPath $file -ErrorAction SilentlyContinue
        Fail ("$name did not download as the file the build expects (its SHA-256 is $got, not " +
              "$want). Run build.bat again; if it happens again, the download is not the published one: please report it.")
    }
    Info "$name`: SHA-256 checked"
}

function Ask($question) {
    if ($Yes) { return $true }
    $a = Read-Host "$question [Y/n]"
    return ($a -eq '' -or $a -match '^[Yy]')
}

function Refresh-Path {
    $m = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $u = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$m;$u"
}

function Have($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

# Run a native command only for its exit code ($LASTEXITCODE), discarding its
# output. Windows PowerShell 5.1 turns a native command's redirected stderr
# into error records, which stop the script under ErrorActionPreference=Stop
# -- so "is capstone installed?" (a traceback when it is not) would end the
# build instead of answering no.
function Probe([string]$exe, [string[]]$argv) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $exe @argv 2>&1 | Out-Null } finally { $ErrorActionPreference = $prev }
}

# Native commands do not throw on failure in Windows PowerShell 5.1.
function Run([string]$exe, [string[]]$argv) {
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { Fail "'$exe $($argv -join ' ')' failed (exit code $LASTEXITCODE)" }
}

function Find-Winget {
    if (Have 'winget') { return 'winget' }
    $w = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
    if (Test-Path $w) { return $w }
    return $null
}

# Download an official installer into %TEMP% and run it silently, once
# Windows says it is signed by its publisher ($publisher, in its
# certificate's name).
function Download-Run($name, $url, $argList, $publisher) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $file = Join-Path $env:TEMP ($url.Split('/')[-1])
    Info "downloading $name from $url"
    try { Invoke-WebRequest -Uri $url -OutFile $file -UseBasicParsing }
    catch { Fail "could not download $name ($($_.Exception.Message))" }
    $sig = Get-AuthenticodeSignature -FilePath $file
    if ($sig.Status -ne 'Valid' -or -not $sig.SignerCertificate -or
        $sig.SignerCertificate.Subject -notmatch [regex]::Escape("CN=$publisher")) {
        Remove-Item $file -ErrorAction SilentlyContinue
        Fail "the $name installer is not signed by $publisher (Windows says: $($sig.Status)); it was not run."
    }
    Info "$name installer: signed by $publisher"
    Info "installing $name (a Windows permission prompt may appear)"
    $p = Start-Process -FilePath $file -ArgumentList $argList -Wait -PassThru
    # 3010: success, reboot recommended (Visual Studio)
    if ($p.ExitCode -ne 0 -and $p.ExitCode -ne 3010) {
        Fail "the $name installer failed (exit code $($p.ExitCode))"
    }
    Remove-Item $file -ErrorAction SilentlyContinue
    Refresh-Path
}

# Install with winget when the PC has it, otherwise straight from the
# vendor's site. $direct is a scriptblock that does the download install.
function Install-Tool($name, $wingetId, $wingetExtra, [scriptblock]$direct) {
    $wg = Find-Winget
    if ($wg) {
        $argv = @('install', '--id', $wingetId, '-e', '--accept-package-agreements',
                  '--accept-source-agreements') + $wingetExtra
        Info "winget $($argv -join ' ')"
        & $wg @argv
        if ($LASTEXITCODE -eq 0) { Refresh-Path; return }
        Info "winget could not install $name (exit code $LASTEXITCODE); downloading it instead"
    }
    & $direct
}

function Install-Git {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    try {
        $rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/git-for-windows/git/releases/latest' `
            -Headers @{ 'User-Agent' = 'rdr-recomp-build' } -UseBasicParsing
    } catch { Fail "could not look up the latest Git for Windows release ($($_.Exception.Message))" }
    $asset = $rel.assets | Where-Object { $_.name -match '^Git-[0-9.]+-64-bit\.exe$' } | Select-Object -First 1
    if (-not $asset) { Fail 'could not find the Git for Windows 64-bit installer in the latest release' }
    Download-Run 'Git' $asset.browser_download_url '/VERYSILENT /NORESTART /SUPPRESSMSGBOXES' 'Johannes Schindelin'
}

function Install-Python {
    # Per-user install: no admin prompt. Adds python and the py launcher to PATH.
    Download-Run 'Python 3.12' 'https://www.python.org/ftp/python/3.12.7/python-3.12.7-amd64.exe' `
        '/quiet InstallAllUsers=0 PrependPath=1 Include_launcher=1 InstallLauncherAllUsers=0 Include_test=0' `
        'Python Software Foundation'
}

function Install-VSBuildTools {
    Download-Run 'Visual Studio 2022 Build Tools' 'https://aka.ms/vs/17/release/vs_BuildTools.exe' `
        '--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621' `
        'Microsoft Corporation'
}

function Find-Git {
    if (Have 'git') { return $true }
    foreach ($d in @("$env:ProgramFiles\Git\cmd", "$env:LOCALAPPDATA\Programs\Git\cmd")) {
        if (Test-Path (Join-Path $d 'git.exe')) { $env:Path = "$d;$env:Path"; return $true }
    }
    return $false
}

function Find-VS {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $p = & $vswhere -latest -products * -version '[17.0,18.0)' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($p) { return ($p | Select-Object -First 1) }
    return $null
}

function Find-Python {
    if (Have 'py') {
        Probe 'py' @('-3', '-c', 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)')
        if ($LASTEXITCODE -eq 0) { return @('py', '-3') }
    }
    # python.exe on PATH, then the per-user install location. (A Microsoft
    # Store "python" alias that is not really installed fails the check.)
    $cands = @()
    if (Have 'python') { $cands += 'python' }
    $cands += Get-ChildItem (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python3*\python.exe') `
        -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | ForEach-Object { $_.FullName }
    foreach ($c in $cands) {
        Probe $c @('-c', 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)')
        if ($LASTEXITCODE -eq 0) { return @($c) }
    }
    return $null
}

Write-Host 'Red Dead Revolver recomp - build' -ForegroundColor White
Info "project : $Here"
Info "game    : $Game"
Info "toolkit : $Xbr"

# -- 0. The game ----------------------------------------------------
# The game: the extracted disc ($Game), else an extracted copy found
# beside the project (any folder there with default.xbe and swat.dat), else
# a disc image to unpack (after the prerequisites: Python unpacks it) --
# given, the only .iso beside the project, or picked in a window, where
# the extracted game's default.xbe can be picked instead.
# Red Dead Revolver's title ID (5454001A) in a default.xbe: the
# certificate's address at +0x118, less the base at +0x104, then +8.
function Test-RdrXbe($xbe) {
    try {
        $fs = [System.IO.File]::OpenRead($xbe)
        try {
            $buf = New-Object byte[] 0x2000
            $n = $fs.Read($buf, 0, $buf.Length)
            if ($n -lt 0x200 -or [System.Text.Encoding]::ASCII.GetString($buf, 0, 4) -ne 'XBEH') { return $false }
            $base = [BitConverter]::ToUInt32($buf, 0x104)
            $cert = [BitConverter]::ToUInt32($buf, 0x118) - $base
            if ($cert -lt 0 -or $cert + 12 -gt $n) { return $false }
            return ([BitConverter]::ToUInt32($buf, [int]$cert + 8) -eq 0x5454001A)
        } finally { $fs.Close() }
    } catch { return $false }
}
# An Xbox disc image: "MICROSOFT*XBOX*MEDIA" at sector 32 of its game
# partition, which a full dump puts after the video partition (the offsets
# tools\extract_xiso.py tries). A PlayStation 2 image -- the 2002 preview,
# kept beside the project for the launcher's Extras -- is not one.
function Test-XboxIso($iso) {
    try {
        $fs = [System.IO.File]::OpenRead($iso)
        try {
            foreach ($off in @(0, 0x18300000, 0x0FD90000, 0x02080000)) {
                if ($fs.Length -lt $off + 0x10000 + 20) { continue }
                $null = $fs.Seek($off + 0x10000, 'Begin')
                $buf = New-Object byte[] 20
                $null = $fs.Read($buf, 0, 20)
                if ([System.Text.Encoding]::ASCII.GetString($buf) -eq 'MICROSOFT*XBOX*MEDIA') { return $true }
            }
        } finally { $fs.Close() }
    } catch { }
    return $false
}
# An extracted copy of the game: a folder with default.xbe (Red Dead
# Revolver's) and swat.dat, beside the project or inside it.
function Find-Extracted {
    $dirs = @(Get-ChildItem -Path $Parent, $Here -Directory -ErrorAction SilentlyContinue |
        Where-Object { (Test-Path (Join-Path $_.FullName 'default.xbe')) -and (Test-Path (Join-Path $_.FullName 'swat.dat')) -and
                       (Test-RdrXbe (Join-Path $_.FullName 'default.xbe')) } |
        Sort-Object FullName -Unique)
    foreach ($d in @($Here, $Parent)) {     # (or the disc's files in the folder itself)
        if ((Test-Path (Join-Path $d 'default.xbe')) -and (Test-Path (Join-Path $d 'swat.dat')) -and
            (Test-RdrXbe (Join-Path $d 'default.xbe'))) { $dirs += Get-Item $d }
    }
    if ($dirs.Count -eq 1) { return $dirs[0].FullName }
    if ($dirs.Count -gt 1) {
        Info ("more than one copy of the game found: " + (($dirs | ForEach-Object { $_.FullName }) -join ', ') + '; using the first')
        return $dirs[0].FullName
    }
    return $null
}
$Xbe = Join-Path $Game 'default.xbe'
if (-not (Test-Path $Xbe) -and -not $Iso) {
    $other = Find-Extracted
    if ($other) {
        $Game = $other
        $Xbe = Join-Path $Game 'default.xbe'
        Info "game    : $Game (found beside the project)"
    }
}
if (-not (Test-Path $Xbe)) {
    if (-not $Iso) {
        $all = @(Get-ChildItem -Path $Parent, $Here, $Game -Filter '*.iso' -File -ErrorAction SilentlyContinue |
            Sort-Object FullName -Unique)
        $found = @($all | Where-Object { Test-XboxIso $_.FullName })
        foreach ($f in $all) {
            if (-not ($found | Where-Object { $_.FullName -eq $f.FullName })) {
                Info "skipped : $($f.Name) (not an Xbox disc image)"
            }
        }
        if ($found.Count -eq 1) { $Iso = $found[0].FullName }
    }
    if (-not $Iso) {
        Write-Host ''
        Write-Host "    No extracted disc in '$Game'."
        Write-Host '    In the window that opens, pick your Red Dead Revolver (USA) Xbox disc image (.iso),'
        Write-Host '    or, if the disc is already extracted somewhere, the default.xbe in that folder.'
        try {
            Add-Type -AssemblyName System.Windows.Forms
            $dlg = New-Object System.Windows.Forms.OpenFileDialog
            $dlg.Title = 'Red Dead Revolver (USA): the disc image (.iso), or default.xbe in the extracted folder'
            $dlg.Filter = 'Disc image or extracted game (*.iso; default.xbe)|*.iso;default.xbe|Xbox disc images (*.iso)|*.iso|The extracted game (default.xbe)|default.xbe|All files (*.*)|*.*'
            $dlg.InitialDirectory = $Parent
            $owner = New-Object System.Windows.Forms.Form -Property @{ TopMost = $true }   # (in front of the console)
            if ($dlg.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) {
                if ($dlg.FileName -match '\.xbe$') {
                    $Game = Split-Path $dlg.FileName -Parent
                    $Xbe = Join-Path $Game 'default.xbe'
                } else {
                    $Iso = $dlg.FileName
                }
            }
            $owner.Dispose()
        } catch { }
    }
}
if (-not (Test-Path $Xbe)) {
    if (-not $Iso) {
        Fail ("No game found. Put the extracted disc in '$Game' (or pass its folder: build.bat " +
              "-Game <folder>), put your disc image (.iso) next to this folder, or drop the .iso " +
              "or the extracted folder onto build.bat.")
    }
    if (-not (Test-Path $Iso)) { Fail "No such disc image: $Iso" }
    $Iso = (Resolve-Path $Iso).Path
    if (-not (Test-XboxIso $Iso)) {
        Fail ("$(Split-Path $Iso -Leaf) is not an Xbox disc image. The build needs Red Dead Revolver (USA) " +
              "for the Xbox: its disc image, or the extracted disc (a folder with default.xbe and swat.dat) " +
              "next to this folder. (The 2002 PlayStation 2 preview is set up later, from the launcher's Extras page.)")
    }
    Info "disc    : $Iso (unpacked once the tools are ready)"
} else {
    if (-not (Test-Path (Join-Path $Game 'swat.dat'))) {
        Fail "'$Game' has default.xbe but not the rest of the disc (swat.dat, bik\...): extract the whole disc."
    }
    Info "game    : $Game"
}

# -- 1. Prerequisites -----------------------------------------------
Step 'Checking prerequisites'

if (-not (Find-Git)) {
    if (Ask 'Git is not installed. Install it now?') {
        Install-Tool 'Git' 'Git.Git' @() { Install-Git }
    }
    if (-not (Find-Git)) { Fail 'Git is required. Install it from https://git-scm.com/download/win and run this again.' }
}
Info 'Git: ok'

$Py = Find-Python
if (-not $Py) {
    if (Ask 'Python 3.10+ is not installed. Install Python 3.12 now?') {
        Install-Tool 'Python' 'Python.Python.3.12' @() { Install-Python }
        $Py = Find-Python
    }
    if (-not $Py) { Fail 'Python 3.10+ is required. Install it from https://www.python.org/downloads/ and run this again.' }
}
$PyExe = (Get-Command $Py[0] -CommandType Application | Select-Object -First 1).Source
$PyArgs = @($Py | Select-Object -Skip 1)
$Py = @($PyExe) + $PyArgs
Info "Python: $($Py -join ' ')"

# -- 1b. The game from its disc image ---------------------------------
if (-not (Test-Path $Xbe)) {
    Step 'Unpacking your disc image'
    & $PyExe @PyArgs -I (Join-Path $Here 'tools\extract_xiso.py') $Iso $Game --check-title 5454001A
    if ($LASTEXITCODE -eq 2) { Fail 'That disc image is not Red Dead Revolver (USA) for the Xbox.' }
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $Xbe)) { Fail 'The disc image could not be unpacked (see above).' }
    Info "game    : $Game"
}

# The build the port was made for: Red Dead Revolver (USA), version 1. Its
# hooks and fixes are tied to that default.xbe's code; another revision or
# region shares the title ID but not the addresses. (-AnyGame: development.)
$KnownXbe = @{ 'AC00C86BFFC8DD906128D7F9B7B6A51E77C57BCE66AA862156BCA1FBE3D698E0' = 'USA, version 1' }
if (-not $AnyGame) {
    $xh = (Get-FileHash $Xbe -Algorithm SHA256).Hash
    if (-not $KnownXbe.ContainsKey($xh)) {
        Fail ("This default.xbe is not the build the port was made for (Red Dead Revolver, USA, version 1; " +
              "its SHA-256 is $xh). Another version would run with the port's fixes in the wrong places.")
    }
    Info "game build: $($KnownXbe[$xh])"
}

function Find-PipTool([string]$module, [string]$spec, [string]$exe) {
    # A tool pip installs for this Python (CMake, Ninja): its exe, or $null.
    # Looked for where its own module says, then in Python's Scripts folders
    # (pip puts a launcher there). No double quotes in what Python is given:
    # Windows PowerShell 5 drops them on the way to a program.
    Probe $PyExe ($PyArgs + @('-c', "import $module"))
    if ($LASTEXITCODE -ne 0) {
        Info "installing $module for Python (once)"
        Probe $PyExe ($PyArgs + @('-m', 'pip', 'install', '--user', '--quiet', $spec))
    }
    $exprs = @("import $module, os; print(os.path.join(getattr($module, 'CMAKE_BIN_DIR', getattr($module, 'BIN_DIR', '')), '$exe'))",
               "import sysconfig, os; print(os.path.join(sysconfig.get_path('scripts', 'nt_user'), '$exe'))",
               "import sysconfig, os; print(os.path.join(sysconfig.get_path('scripts'), '$exe'))")
    foreach ($e in $exprs) {
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try { $out = & $PyExe ($PyArgs + @('-c', $e)) 2>$null } finally { $ErrorActionPreference = $prev }
        $last = $out | Select-Object -Last 1
        if ($last -and (Test-Path $last)) { return $last }
    }
    return $null
}

function Get-ZipTool([string]$name, [string]$url, [string]$folder, [string]$exeRel) {
    # A tool's official zip from its GitHub releases, unpacked into deps\
    # (when pip's copy cannot be found): its exe, or $null.
    $dir = Join-Path $Deps $folder
    $exe = Join-Path $dir $exeRel
    if (Test-Path $exe) { return $exe }
    try {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        $zip = Join-Path $Deps "$folder.zip"
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Info "downloading $name"
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip
        Test-Download $zip $url.Split('/')[-1]
        & tar.exe -xf $zip -C $dir
        if ($LASTEXITCODE -ne 0) { Expand-Archive -Path $zip -DestinationPath $dir -Force }
        Remove-Item $zip -ErrorAction SilentlyContinue
    } catch {
        Info "could not get $name ($($_.Exception.Message))"
        return $null
    }
    if (Test-Path $exe) { return $exe }
    return $null
}

# Which compiler: asked on the first build (user: "it does not give me the
# option of default or clang"), then remembered in deps\compiler.txt; given
# on the command line (-Compiler msvc / clang) it is used and remembered.
# With -Yes and nothing remembered: Visual Studio, as before.
$ChoiceFile = Join-Path $Here 'deps\compiler.txt'
if ($Compiler) {
    New-Item -ItemType Directory -Force -Path (Split-Path $ChoiceFile) | Out-Null
    Set-Content -Path $ChoiceFile -Value $Compiler -Encoding ascii
} elseif (Test-Path $ChoiceFile) {
    $c = (Get-Content $ChoiceFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ($c) { $c = $c.Trim() }
    if ($c -eq 'msvc' -or $c -eq 'clang') {
        $Compiler = $c
        $other = if ($c -eq 'clang') { 'msvc' } else { 'clang' }
        Info "compiler: $(if ($c -eq 'clang') { 'Clang (LLVM-MinGW)' } else { 'Visual Studio' }), as chosen before (build.bat -Compiler $other to change)"
    }
} elseif ((Test-Path (Join-Path $Here 'build\CMakeCache.txt')) -or (Test-Path (Join-Path $Here 'build-clang\CMakeCache.txt'))) {
    # built before this asked: the compiler that built it, without asking
    $Compiler = if (Test-Path (Join-Path $Here 'build\CMakeCache.txt')) { 'msvc' } else { 'clang' }
    New-Item -ItemType Directory -Force -Path (Split-Path $ChoiceFile) | Out-Null
    Set-Content -Path $ChoiceFile -Value $Compiler -Encoding ascii
} elseif (-not $Yes) {
    $vsHere = [bool](Find-VS)
    Write-Host ''
    Write-Host '    Which compiler should build the game?'
    Write-Host "      1. Visual Studio 2022 (the default; $(if ($vsHere) { 'already installed' } else { 'about 2 GB to download, 3-4 GB on disk, needs admin' }))"
    Write-Host '      2. Clang (LLVM-MinGW: about 180 MB to download, 600 MB on disk, no installer, no admin;'
    Write-Host '         slightly faster game code, newer here and less tested)'
    $a = Read-Host '    Choose 1 or 2 [1]'
    $Compiler = if ($a -match '^\s*2') { 'clang' } else { 'msvc' }
    New-Item -ItemType Directory -Force -Path (Split-Path $ChoiceFile) | Out-Null
    Set-Content -Path $ChoiceFile -Value $Compiler -Encoding ascii
    Info "compiler: $(if ($Compiler -eq 'clang') { 'Clang (LLVM-MinGW)' } else { 'Visual Studio' }) (remembered; build.bat -Compiler msvc or clang to change)"
}

$VS = $null
if ($Compiler -ne 'clang') { $VS = Find-VS }
if (-not $VS -and $Compiler -ne 'clang') {
    # Only what the build uses: the C++ compiler (MSVC, x64) and the Windows
    # SDK -- not the whole C++ workload with its recommended extras (CMake
    # comes from pip below, ATL, test tools, sanitizers, older compilers),
    # and --nocache: the installer's package cache is not kept.
    Write-Host '    Visual Studio 2022 C++ build tools were not found (the compiler and the Windows SDK only:'
    Write-Host '    about 2 GB to download, needs admin).'
    if (Ask 'Install the Visual Studio 2022 C++ compiler and Windows SDK now?') {
        Install-Tool 'Visual Studio Build Tools' 'Microsoft.VisualStudio.2022.BuildTools' @('--override',
            '--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.22621') `
            { Install-VSBuildTools }
        $VS = Find-VS
    } elseif (Ask 'Build with LLVM-MinGW instead (about 180 MB, no installer, no admin; newer here and less tested)?') {
        $Compiler = 'clang'
        Set-Content -Path $ChoiceFile -Value $Compiler -Encoding ascii -ErrorAction SilentlyContinue
    }
    if (-not $VS -and $Compiler -ne 'clang') { Fail 'Visual Studio 2022 with "Desktop development with C++" is required (or run build.bat -Compiler clang).' }
}

$LlvmVer = '20250826'
$Llvm = $null
if ($Compiler -eq 'clang') {
    # LLVM-MinGW: Clang, LLD and the MinGW-w64 Windows headers and
    # libraries, one zip unpacked into deps\
    $Llvm = Join-Path $Deps "llvm-mingw-$LlvmVer-ucrt-x86_64"
    if (-not (Test-Path (Join-Path $Llvm 'bin\x86_64-w64-mingw32-clang.exe'))) {
        try {
            New-Item -ItemType Directory -Force -Path $Deps | Out-Null
            $zip = Join-Path $Deps "llvm-mingw-$LlvmVer-ucrt-x86_64.zip"
            [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
            Info "downloading LLVM-MinGW $LlvmVer (about 180 MB)"
            Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/mstorsjo/llvm-mingw/releases/download/$LlvmVer/llvm-mingw-$LlvmVer-ucrt-x86_64.zip" -OutFile $zip
            Test-Download $zip "llvm-mingw-$LlvmVer-ucrt-x86_64.zip"
            Info 'unpacking'
            # (Windows' own tar reads zips, and far faster than Expand-Archive)
            & tar.exe -xf $zip -C $Deps
            if ($LASTEXITCODE -ne 0) { Expand-Archive -Path $zip -DestinationPath $Deps -Force }
            Remove-Item $zip -ErrorAction SilentlyContinue
        } catch {
            Fail "could not get LLVM-MinGW: $($_.Exception.Message)"
        }
        if (-not (Test-Path (Join-Path $Llvm 'bin\x86_64-w64-mingw32-clang.exe'))) { Fail "LLVM-MinGW did not unpack to $Llvm" }
    }
    Info "LLVM-MinGW: $Llvm"
    $Build = Join-Path $Here 'build-clang'
} else {
    Info "Visual Studio: $VS"
}

$CMake = $null
if ($VS) { $CMake = Join-Path $VS 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }
if (-not $CMake -or -not (Test-Path $CMake)) {
    if (Have 'cmake') { $CMake = (Get-Command cmake).Source }
    else {
        # (the slim Visual Studio install has no CMake: Python's, ~50 MB,
        # else Kitware's own zip)
        $CMake = Find-PipTool 'cmake' 'cmake==3.31.6' 'cmake.exe'
        if (-not $CMake) {
            $CMake = Get-ZipTool 'CMake 3.31.6 (about 45 MB)' `
                'https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-windows-x86_64.zip' `
                'cmake' 'cmake-3.31.6-windows-x86_64\bin\cmake.exe'
        }
        if (-not $CMake) { Fail 'CMake not found, and neither pip nor a download could get it. Install CMake from https://cmake.org/download/ and run this again.' }
    }
}
Info "CMake: $CMake"
$Ninja = $null
if ($Compiler -eq 'clang') {
    if (Have 'ninja') { $Ninja = (Get-Command ninja).Source }
    else { $Ninja = Find-PipTool 'ninja' 'ninja==1.13.0' 'ninja.exe' }
    if (-not $Ninja) {
        $Ninja = Get-ZipTool 'Ninja 1.12.1' 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip' `
            'ninja' 'ninja.exe'
    }
    if (-not $Ninja) { Fail 'Ninja not found, and neither pip nor a download could get it.' }
    Info "Ninja: $Ninja"
}

Step 'Python packages'
# capstone 5 (the generated code here was made with 5.0.9; 4 and 6 read
# instructions differently): 5.0.9 installed when there is no 5.
Probe $PyExe ($PyArgs + @('-c', 'import capstone; assert capstone.CS_API_MAJOR == 5'))
if ($LASTEXITCODE -ne 0) {
    Run $PyExe ($PyArgs + @('-m', 'pip', 'install', '--user', '--quiet', 'capstone==5.0.9'))
}
Info 'capstone: ok'

# -- 2. Toolkit -----------------------------------------------------
Step 'xboxrecomp toolkit'
if (-not (Test-Path (Join-Path $Xbr '.git'))) {
    # LF line endings: the patch is LF, and Git for Windows would otherwise
    # check the sources out as CRLF and the patch would not apply.
    Run 'git' @('clone', '--quiet', '-c', 'core.autocrlf=false', $XbrUrl, $Xbr)
}
Run 'git' @('-C', $Xbr, 'config', 'core.autocrlf', 'false')
$head = (& git -C $Xbr rev-parse HEAD).Trim()
if ($head -ne $XbrCommit) {
    Info "checking out v0.16.0 ($($XbrCommit.Substring(0, 7)))"
    Probe 'git' @('-C', $Xbr, 'cat-file', '-e', "$XbrCommit^{commit}")
    if ($LASTEXITCODE -ne 0) { Run 'git' @('-C', $Xbr, 'fetch', '--quiet', 'origin') }
    Run 'git' @('-C', $Xbr, '-c', 'advice.detachedHead=false', 'checkout', '--quiet', $XbrCommit)
}
# The patch: x86-64 POSIX MMIO (inert on Windows) and the single-guest-CPU
# scheduling fixes Bink's movie reader needs (every host). The toolkit folder
# is managed by this script: when the patch is not applied as-is (an older
# version of it, or CRLF line endings from an earlier clone), the folder is
# reset to v0.16.0 and the current patch applied.
Probe 'git' @('-C', $Xbr, 'apply', '--reverse', '--check', $Patch)
if ($LASTEXITCODE -eq 0) {
    Info 'patch already applied'
} else {
    Probe 'git' @('-C', $Xbr, 'apply', '--check', $Patch)
    if ($LASTEXITCODE -ne 0) {
        Info 'resetting the toolkit to v0.16.0 for the current patch'
        # Keep what is in the folder now (an older patch, or changes of your
        # own) as a patch file next to it before the reset, so nothing is
        # lost: git apply that file to get it back.
        $saved = Join-Path (Split-Path -Parent $Xbr) ("xboxrecomp-before-reset-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.patch')
        $prevEa = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try { $diff = & git -C $Xbr diff --binary HEAD 2>$null } finally { $ErrorActionPreference = $prevEa }
        if ($diff) {
            [IO.File]::WriteAllText($saved, (($diff -join "`n") + "`n"))
            Info "the toolkit's changes before the reset are kept in $saved"
        }
        Run 'git' @('-C', $Xbr, 'rm', '-r', '-q', '--cached', '.')
        Run 'git' @('-C', $Xbr, 'reset', '-q', '--hard', $XbrCommit)
        # A reset keeps files an earlier patch created (git does not track
        # them), and the patch will not create a file that already exists.
        # Remove the ones this patch adds.
        $plines = Get-Content $Patch
        for ($i = 0; $i -lt $plines.Count - 1; $i++) {
            if ($plines[$i] -match '^diff --git a/(\S+) b/') {
                $added = $Matches[1]
                if ($plines[$i + 1] -match '^new file mode') {
                    $ap = Join-Path $Xbr $added
                    if (Test-Path $ap) {
                        Info "removing $added (left by an earlier patch)"
                        Remove-Item -Force $ap
                    }
                }
            }
        }
        Probe 'git' @('-C', $Xbr, 'apply', '--check', $Patch)
    }
    if ($LASTEXITCODE -ne 0) {
        Fail "The toolkit patch does not apply to '$Xbr' even after a reset. Delete that folder and run this again."
    }
    Run 'git' @('-C', $Xbr, 'apply', $Patch)
    Info 'patch applied'
}

# -- 3. Generated code ----------------------------------------------
Step 'Generated code (from your default.xbe)'
$xbeHash  = (Get-FileHash $Xbe -Algorithm SHA256).Hash
$namesHash = (Get-FileHash (Join-Path $Here 'names\string_names.json') -Algorithm SHA256).Hash
# The generated code depends on the XBE, the toolkit version, the names, the
# extra function entries, and the generator revision: bumped by hand when the
# patch changes the lifter (gen2: jump tables past 256 entries). Code made by
# an older combination is redone.
$GenRev = 'gen2'
$extraHash = (Get-FileHash (Join-Path $Here 'names\extra_functions.json') -Algorithm SHA256).Hash.Substring(0, 12)
$GenRev = "$GenRev-$extraHash"
# The hooks in src\rdr_hooks.c are compiled into the generated code: entry
# hooks (sub_X_enter) and replacements (sub_X, the original kept as
# sub_X_gen). A different set of hooked or replaced functions means
# generating it again (what they do does not).
$hookNames = (Select-String -Path (Join-Path $Here 'src\rdr_hooks.c') -Pattern '^void (sub_[0-9A-Fa-f]{8}(_enter)?)\(void\)' -AllMatches |
              ForEach-Object { $_.Matches[0].Groups[1].Value } | Sort-Object) -join ','
if ($hookNames) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $hookHash = -join ($sha.ComputeHash([Text.Encoding]::ASCII.GetBytes($hookNames))[0..5] | ForEach-Object { $_.ToString('x2') })
    $GenRev = "$GenRev-h$hookHash"
}
$stampWant = "$xbeHash $XbrCommit $GenRev $namesHash"
$stampFile = Join-Path $Gen '.stamp'
$fresh = $false
if (Test-Path $stampFile) {
    $have = (Get-Content $stampFile -Raw).Trim().Split(' ')
    $fresh = ($have.Count -eq 4 -and $have[0] -eq $xbeHash -and $have[1] -eq $XbrCommit -and $have[2] -eq $GenRev -and $have[3] -eq $namesHash)
}

if ($Clean -or -not $fresh -or -not (Get-ChildItem $Gen -Filter 'recomp_*.c' -ErrorAction SilentlyContinue)) {
    Info 'regenerating - about 15 minutes, most of it the disassembler'
    $regen = Join-Path $Here 'tools\regen.ps1'
    try { & $regen -XboxRecomp $Xbr -Game $Game -Python $Py }
    catch { Fail "regeneration failed: $($_.Exception.Message)" }
    Set-Content -Path $stampFile -Value $stampWant -Encoding ASCII
} else {
    Info 'up to date'
}

# -- 4. SDL2, for controllers XInput cannot see --------------------
# PlayStation, Switch and most other pads are not XInput devices; SDL2's
# game controller support reads them (and Xbox pads too). The official
# prebuilt package, unpacked beside the toolkit. Without it the game still
# builds, with Xbox-compatible pads only.
Step 'SDL2 (controller support)'
$SdlVer = '2.30.9'
if (-not (Test-Path (Join-Path $Deps "SDL2-$SdlVer")) -and (Test-Path (Join-Path $Parent "deps\SDL2-$SdlVer"))) {
    $Deps = Join-Path $Parent 'deps'                   # an earlier build's, beside the project
}
# (the MinGW build takes SDL2's MinGW package, in deps\mingw\)
$SdlFlavour = 'VC'
$SdlHome = $Deps
if ($Compiler -eq 'clang') { $SdlFlavour = 'mingw'; $SdlHome = Join-Path $Deps 'mingw' }
$SdlDir = Join-Path $SdlHome "SDL2-$SdlVer"
$SdlCmake = Join-Path $SdlDir 'cmake'
if (-not (Test-Path (Join-Path $SdlCmake 'sdl2-config.cmake'))) {
    try {
        New-Item -ItemType Directory -Force -Path $SdlHome | Out-Null
        $zip = Join-Path $SdlHome "SDL2-devel-$SdlVer-$SdlFlavour.zip"
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Info "downloading SDL2 $SdlVer"
        Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/libsdl-org/SDL/releases/download/release-$SdlVer/SDL2-devel-$SdlVer-$SdlFlavour.zip" -OutFile $zip
        Test-Download $zip "SDL2-devel-$SdlVer-$SdlFlavour.zip"
        Expand-Archive -Path $zip -DestinationPath $SdlHome -Force
        Remove-Item $zip -ErrorAction SilentlyContinue
    } catch {
        Info "could not get SDL2 ($($_.Exception.Message)); building with Xbox-compatible controllers only"
    }
}
if ($Compiler -eq 'clang') {
    # Paths with forward slashes: CMake writes the tools' paths into its own
    # files as they are given, where a backslash is an escape ("Invalid
    # character escape '\G'" -- user, F:\Games\...).
    function Fwd([string]$p) { return ($p -replace '\\', '/') }
    $cmakeArgs = @('-S', (Fwd $Here), '-B', (Fwd $Build), '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$(Fwd $Ninja)",
                   "-DCMAKE_C_COMPILER=$(Fwd (Join-Path $Llvm 'bin\x86_64-w64-mingw32-clang.exe'))",
                   "-DCMAKE_CXX_COMPILER=$(Fwd (Join-Path $Llvm 'bin\x86_64-w64-mingw32-clang++.exe'))",
                   "-DCMAKE_RC_COMPILER=$(Fwd (Join-Path $Llvm 'bin\x86_64-w64-mingw32-windres.exe'))",
                   '-DCMAKE_BUILD_TYPE=Release', "-DXBOXRECOMP_DIR=$(Fwd $Xbr)", '-DRDR_GEN_O2=ON')
    # a configure that stopped half way (no build.ninja) keeps what it
    # found: started again from nothing
    if ((Test-Path (Join-Path $Build 'CMakeCache.txt')) -and -not (Test-Path (Join-Path $Build 'build.ninja'))) {
        Remove-Item -Force (Join-Path $Build 'CMakeCache.txt')
        Remove-Item -Recurse -Force (Join-Path $Build 'CMakeFiles') -ErrorAction SilentlyContinue
    }
} else {
    $cmakeArgs = @('-S', $Here, '-B', $Build, '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DXBOXRECOMP_DIR=$Xbr",
                   "-DRDR_GEN_O2=$(if ($FastCode) { 'ON' } else { 'OFF' })")
}
if (Test-Path (Join-Path $SdlCmake 'sdl2-config.cmake')) {
    Info 'present'
    $cmakeArgs += "-DSDL2_DIR=$($SdlCmake -replace '\\', '/')"
}

# -- 5. Compile -----------------------------------------------------
Step 'Compiling (10-30 minutes the first time)'
if ($Clean -and (Test-Path $Build)) { Remove-Item -Recurse -Force $Build }
Run $CMake $cmakeArgs
Run $CMake @('--build', $Build, '--config', 'Release', '--parallel')

$Exe = Join-Path $Build 'Release\rdr_recomp.exe'
if (-not (Test-Path $Exe)) { Fail "the build finished but $Exe is missing" }

Write-Host ''
Write-Host "Built: $Exe" -ForegroundColor Green

# -- 6. The ready-to-play folder ------------------------------------
Step 'Ready-to-play folder'
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package.ps1') -Game $Game -Out $Play -Release (Join-Path $Build 'Release')
Write-Host 'Start "Red Dead Revolver.exe" in the Red Dead Recompilation folder inside this one.'
exit 0
