param(
    [string] $GameDir = "E:\SteamLibrary\steamapps\common\ForzaHorizon6",
    [switch] $SkipBuild
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$iss = Join-Path $root "installer\inno\FH6UniversalRadio.iss"

function Find-InnoCompiler {
    $cmd = Get-Command iscc -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    foreach ($path in @(
        "${env:LOCALAPPDATA}\Programs\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
    )) {
        if (Test-Path $path) { return $path }
    }

    throw @"
Inno Setup compiler was not found.

Install Inno Setup 6, then re-run:
  .\scripts\build-installer.ps1

With winget:
  winget install JRSoftware.InnoSetup
"@
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") | Out-Host
}

& (Join-Path $PSScriptRoot "package-windows.ps1") -GameDir $GameDir | Out-Host

$iscc = Find-InnoCompiler
Write-Host "Using Inno Setup: $iscc" -ForegroundColor DarkGray
& $iscc $iss | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Inno Setup build failed" }

$setup = Join-Path $root "package\FH6UniversalRadioSetup.exe"
if (-not (Test-Path $setup)) {
    throw "Installer was expected at $setup but was not found."
}

Write-Host "`nBuilt installer: $setup" -ForegroundColor Green
