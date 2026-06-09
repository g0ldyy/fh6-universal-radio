# Creates a Windows package staging folder. If the VB-CABLE driver package has
# been fetched into third_party\vbcable, it is staged for an explicit installer
# consent step.

param(
    [string] $OutDir = "package\windows",
    [string] $GameDir = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$out = Join-Path $root $OutDir
$rootFull = [System.IO.Path]::GetFullPath($root)
$outFull = [System.IO.Path]::GetFullPath($out)
$driveRoot = [System.IO.Path]::GetPathRoot($outFull)

if ([string]::IsNullOrWhiteSpace($OutDir) -or
    $outFull -eq $rootFull -or
    $outFull -eq $driveRoot -or
    -not $outFull.StartsWith($rootFull + [System.IO.Path]::DirectorySeparatorChar,
        [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to stage package into unsafe OutDir: '$OutDir'"
}

if (-not (Test-Path (Join-Path $dist "version.dll"))) {
    throw "dist\version.dll not found -- run scripts\build.ps1 first."
}

if (Test-Path $outFull) { Remove-Item -Recurse -Force -LiteralPath $outFull }
New-Item -ItemType Directory -Force -Path $outFull | Out-Null

Copy-Item (Join-Path $dist "version.dll") $out
if (Test-Path (Join-Path $dist "fh6-radio-companion.exe")) {
    Copy-Item (Join-Path $dist "fh6-radio-companion.exe") $out
}
Copy-Item -Recurse (Join-Path $dist "fh6-radio") $out
if (Test-Path (Join-Path $dist "media")) {
    Copy-Item -Recurse (Join-Path $dist "media") $out
}
Copy-Item (Join-Path $root "README.md") $out
Copy-Item (Join-Path $root "config.example.toml") $out
Copy-Item (Join-Path $root "docs\windows-packaging.md") $out
Copy-Item (Join-Path $root "scripts\windows-installer.ps1") (Join-Path $out "Install-FH6UniversalRadio.ps1")
Copy-Item (Join-Path $root "scripts\windows-uninstall.ps1") (Join-Path $out "Uninstall-FH6UniversalRadio.ps1")
Copy-Item (Join-Path $root "scripts\install-vbcable.ps1") (Join-Path $out "Install-VBCable.ps1")

$vbCableZip = Join-Path $root "third_party\vbcable\VBCABLE_Driver_Pack45.zip"
if (Test-Path $vbCableZip) {
    $vbCableOut = Join-Path $out "vbcable"
    New-Item -ItemType Directory -Force -Path $vbCableOut | Out-Null
    Copy-Item $vbCableZip $vbCableOut -Force
}

$gitSha = ""
try {
    $gitSha = (& git -C $root rev-parse --short HEAD 2>$null)
} catch {
    $gitSha = ""
}

$manifest = @{
    name = "FH6 Universal Radio"
    packageVersion = "dev"
    gitSha = $gitSha
    packagedAt = (Get-Date).ToString("o")
    components = @(
        "version.dll",
        "fh6-radio",
        "media",
        "fh6-radio-companion.exe",
        "Install-VBCable.ps1",
        "vbcable"
    )
} | ConvertTo-Json
Set-Content -LiteralPath (Join-Path $out "package-manifest.json") -Value $manifest

$note = @"
FH6 Universal Radio Windows package.

Install or update:
  .\Install-FH6UniversalRadio.ps1 -GameDir "$GameDir"

Uninstall:
  .\Uninstall-FH6UniversalRadio.ps1

Apple Music cable mode expects VB-CABLE. If vbcable\VBCABLE_Driver_Pack45.zip
is present, run .\Install-VBCable.ps1 or use the EXE installer option to launch
the VB-CABLE setup.

Apple Music audio routing:
  1. Install VB-CABLE, then reboot Windows if the VB-CABLE setup asks for it.
  2. Open Apple Music and start playback once.
  3. Open Windows Settings > System > Sound > Volume mixer.
  4. Find Apple Music in the Apps list.
  5. Set Apple Music Output device to CABLE Input (VB-Audio Virtual Cable).
  6. Leave Input device as Default unless you have a specific reason to change it.
  7. Start FH6 Radio Companion. When FH6 is closed it monitors CABLE Output to
     your default speakers/headphones. When FH6 is running it releases the cable
     so the in-game radio can consume it.
  8. Launch FH6 and use the Apple Music source at http://localhost:8420.

Troubleshooting:
  - If Apple Music is silent outside FH6, make sure FH6 Radio Companion is
    running and your default Windows output is your headphones/speakers.
  - If FH6 is silent, confirm Apple Music output is CABLE Input and the radio
    config capture device is CABLE Output.
  - If CABLE Input or CABLE Output is missing, rerun the VB-CABLE setup as
    administrator and reboot.
"@

Set-Content -LiteralPath (Join-Path $out "INSTALL.txt") -Value $note
Write-Host "Staged Windows package at $out" -ForegroundColor Green
