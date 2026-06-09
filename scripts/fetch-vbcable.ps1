param(
    [string] $OutDir = "third_party\vbcable",
    [string] $Url = "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root $OutDir
$zip = Join-Path $out "VBCABLE_Driver_Pack45.zip"

New-Item -ItemType Directory -Force -Path $out | Out-Null

Write-Host "Downloading VB-CABLE from VB-Audio..." -ForegroundColor Cyan
Write-Host "  $Url"
Invoke-WebRequest -Uri $Url -OutFile $zip

$item = Get-Item $zip
if ($item.Length -lt 500KB) {
    throw "Downloaded file is unexpectedly small: $($item.Length) bytes"
}

Write-Host "Saved $zip ($([Math]::Round($item.Length / 1MB, 2)) MB)" -ForegroundColor Green
Write-Host "VB-CABLE is donationware from VB-Audio. Keep attribution and installer consent visible when redistributing."
