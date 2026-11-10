<#
  regen.ps1 - regenerate src\recomp\gen\ from your own default.xbe.
  Normally run by build.ps1; by hand:
    powershell -ExecutionPolicy Bypass -File tools\regen.ps1 -XboxRecomp ..\xboxrecomp -Game "..\Red Dead Revolver (USA)"
#>
param(
    [Parameter(Mandatory)][string]$XboxRecomp,
    [Parameter(Mandatory)][string]$Game,
    [string[]]$Python = @('py', '-3')   # executable, then its arguments
)
$ErrorActionPreference = 'Stop'

$here = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$xr   = (Resolve-Path $XboxRecomp).Path
$gd   = (Resolve-Path $Game).Path
# Resolve to the program file: PowerShell looks up functions before programs,
# case-insensitively, so a bare 'py' could otherwise hit a script function.
$pyCmd = Get-Command $Python[0] -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $pyCmd) { throw "Python not found: $($Python[0])" }
$pyExe = $pyCmd.Source; $pyArgs = @($Python | Select-Object -Skip 1)

function Invoke-Step([string]$what, [string[]]$argv) {
    Write-Host "    $what"
    & $pyExe @pyArgs @argv
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" }
}

New-Item -ItemType Directory -Force (Join-Path $xr 'game_files') | Out-Null
Copy-Item (Join-Path $gd 'default.xbe') ([IO.Path]::Combine($xr, 'game_files', 'default.xbe')) -Force

Push-Location $xr
try {
    Invoke-Step 'parse XBE'           @('-m', 'tools.xbe_parser', 'game_files/default.xbe', '--json', 'game_files/default_analysis.json')
    Invoke-Step 'disassemble (~10 min)' @('-m', 'tools.disasm', 'game_files/default.xbe')
    Invoke-Step 'identify functions'  @('-m', 'tools.func_id', 'game_files/default.xbe')
    Invoke-Step 'recover ABIs'        @('-m', 'tools.abi_analysis', 'game_files/default.xbe')
    Invoke-Step 'apply names'         @(([IO.Path]::Combine($here, 'tools', 'apply_names.py')), 'tools/disasm/output/functions.json', ([IO.Path]::Combine($here, 'names', 'string_names.json')), ([IO.Path]::Combine($here, 'names', 'extra_functions.json')))
    $gen = [IO.Path]::Combine($here, 'src', 'recomp', 'gen')
    if (Test-Path $gen) { Remove-Item -Recurse -Force $gen }
    New-Item -ItemType Directory -Force $gen | Out-Null
    # src\rdr_hooks.c: its sub_XXXXXXXX_enter functions are called at the
    # top of those functions (entry hooks on directly-called code).
    $hooks = [IO.Path]::Combine($here, 'src', 'rdr_hooks.c')
    Invoke-Step 'lift to C (~3 min)'  @('-m', 'tools.recomp', 'game_files/default.xbe', '--all', '--split', '250',
                               '--game-name', 'Red Dead Revolver', '--gen-dir', $gen,
                               '--exclude-manual', $hooks)
} finally {
    Pop-Location
}
Write-Host "    generated $((Get-ChildItem $gen -Filter *.c).Count) files"
