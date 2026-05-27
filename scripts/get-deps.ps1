# Downloads the three single-header dependencies into third_party/.
# Run this once after cloning the project, before configuring CMake.
#
#   PS> .\scripts\get-deps.ps1
#
# Re-runs are safe -- existing files are overwritten.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tp   = Join-Path $root "third_party"

$deps = @(
    @{ Url = "https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp";
       Out = "nlohmann/nlohmann/json.hpp" },
    @{ Url = "https://raw.githubusercontent.com/ToruNiina/toml11/main/single_include/toml.hpp";
       Out = "toml11/toml.hpp" },
    @{ Url = "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h";
       Out = "miniaudio/miniaudio.h" }
)

foreach ($d in $deps) {
    $target = Join-Path $tp $d.Out
    $dir    = Split-Path -Parent $target
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Write-Host "-> $($d.Out)" -ForegroundColor Cyan
    Invoke-WebRequest -UseBasicParsing -Uri $d.Url -OutFile $target
}

Write-Host "`nAll dependencies fetched into $tp." -ForegroundColor Green
