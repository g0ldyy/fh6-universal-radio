# Copies the built mod into a Forza Horizon 6 install. Run after build.ps1
# (and fetch-media.ps1 for the radio-station overlay).
#
#   PS> .\scripts\install.ps1 -GameDir "C:\XboxGames\Forza Horizon 6\Content"
#
# Existing files are backed up to *.bak before being overwritten.

param(
    [Parameter(Mandatory = $true)] [string] $GameDir,
    [switch] $SkipMedia
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$mdir = Join-Path $dist "media"

. (Join-Path $PSScriptRoot "package-tools.ps1")

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
$cfg = Join-Path $dataDir "config.toml"
if (-not (Test-Path $cfg)) {
    Copy-Item (Join-Path $dist "fh6-radio\config.toml") $cfg
    Write-Host "  + fh6-radio\config.toml  (seeded from example)" -ForegroundColor Yellow
}

$roonBridgeDist = Join-Path $dist "fh6-radio\tools\roon-bridge"
if (Test-Path $roonBridgeDist) {
    Copy-RoonBridgePackage `
        -SourceDir $roonBridgeDist `
        -DestinationDir (Join-Path $dataDir "tools\roon-bridge") `
        -IncludeNodeModules
    Write-Host "  + fh6-radio\tools\roon-bridge"
}

if (-not $SkipMedia -and (Test-Path $mdir)) {
    Get-ChildItem -Recurse -File $mdir | ForEach-Object {
        $rel = $_.FullName.Substring($mdir.Length + 1)
        Backup-AndCopy $_.FullName (Join-Path $GameDir "media\$rel") | Out-Host
    }
} elseif (-not $SkipMedia) {
    Write-Warning "dist\media\ missing -- radio station overlay not installed. Run scripts\fetch-media.ps1 first."
}

Write-Host "`nDone. Launch the game, set Audio -> Radio DJ = Off, Streamer Mode = On." -ForegroundColor Green
Write-Host "Then open http://localhost:8420 in any browser on this LAN."
