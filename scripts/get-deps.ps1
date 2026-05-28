# Downloads pinned single-header dependencies into third_party/.
# Run this once after cloning the project, before configuring CMake.
#
#   PS> .\scripts\get-deps.ps1
#
# Re-runs are safe -- existing files are overwritten.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tp   = Join-Path $root "third_party"

$deps = @(
    @{ Url = "https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp";
       Out = "nlohmann/nlohmann/json.hpp";
       Sha256 = "AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63" },
    @{ Url = "https://raw.githubusercontent.com/ToruNiina/toml11/v4.4.0/single_include/toml.hpp";
       Out = "toml11/toml.hpp";
       Sha256 = "71A65E312D8375F4FDBCA3B921C6E3D9255058EA964071DD36F674183047D260" },
    @{ Url = "https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h";
       Out = "miniaudio/miniaudio.h";
       Sha256 = "AC7AF4DE748B7E26B777F37E01CEE313A308A7296A3EB080E2906B320CC55C89" },
    @{ Url = "https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.46.0/httplib.h";
       Out = "cpp-httplib/httplib.h";
       Sha256 = "5CABD324CBF421A5F94F26500B9BFC6D94F336BF42C04721925A4B419896C7FB" }
)

foreach ($d in $deps) {
    $target = Join-Path $tp $d.Out
    $dir    = Split-Path -Parent $target
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Write-Host "-> $($d.Out)" -ForegroundColor Cyan
    Invoke-WebRequest -UseBasicParsing -Uri $d.Url -OutFile $target
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $target).Hash
    if ($hash -ne $d.Sha256) {
        Remove-Item -LiteralPath $target -Force
        throw "Checksum mismatch for $($d.Out). Expected $($d.Sha256), got $hash."
    }
}

Write-Host "`nAll dependencies fetched into $tp." -ForegroundColor Green
