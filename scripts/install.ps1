# Copies the built mod into a Forza Horizon 6 install. Run after build.ps1
#
#   PS> .\scripts\install.ps1 -GameDir "C:\XboxGames\Forza Horizon 6\Content"
#
# Existing files are backed up to *.bak before being overwritten.

param(
    [Parameter(Mandatory = $true)] [string] $GameDir
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"

if (-not (Test-Path (Join-Path $dist "version.dll"))) {
    throw "dist\version.dll not found -- run scripts\build.ps1 first."
}
if (-not (Test-Path $GameDir)) {
    throw "Game directory not found: $GameDir"
}
if (-not (Test-Path (Join-Path $GameDir "forzahorizon6.exe"))) {
    Write-Warning "forzahorizon6.exe not found in $GameDir -- make sure this is the right folder."
}

function Backup-AndCopy([string]$src, [string]$dst) {
    $dstDir = Split-Path -Parent $dst
    if (-not (Test-Path $dstDir)) { New-Item -ItemType Directory -Force -Path $dstDir | Out-Null }
    if (Test-Path $dst) { Copy-Item $dst "$dst.bak" -Force }
    Copy-Item $src $dst -Force
    "  + $($dst.Substring($GameDir.Length + 1))"
}

Backup-AndCopy (Join-Path $dist "version.dll") (Join-Path $GameDir "version.dll") | Out-Host

$dataDir = Join-Path $GameDir "fh6-radio"
if (-not (Test-Path $dataDir)) { New-Item -ItemType Directory -Force -Path $dataDir | Out-Null }
Copy-Item -Recurse -Force (Join-Path $dist "fh6-radio\ui") $dataDir
Copy-Item -Recurse -Force (Join-Path $dist "fh6-radio\assets") (Join-Path $dataDir "assets")
$cfg = Join-Path $dataDir "config.toml"
if (-not (Test-Path $cfg)) {
    Copy-Item (Join-Path $dist "fh6-radio\config.toml") $cfg
    Write-Host "  + fh6-radio\config.toml  (seeded from example)" -ForegroundColor Yellow
}

$workerExe = Join-Path $dist "fh6-radio\fh6-radio-worker.exe"
if (Test-Path $workerExe) {
    Backup-AndCopy $workerExe (Join-Path $dataDir "fh6-radio-worker.exe") | Out-Host
}

Write-Host "`nDone. Launch the game, set Audio -> Radio DJ = Off, Streamer Mode = On." -ForegroundColor Green
Write-Host "Then open http://localhost:8420 in any browser on this LAN."
