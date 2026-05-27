$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot

function Require-Text([string]$Text, [string]$Needle, [string]$Message) {
    if (-not $Text.Contains($Needle)) { throw $Message }
}

$wasapi = Get-Content -LiteralPath (Join-Path $root "src/audio/wasapi_loopback_capture.cpp") -Raw
$routes = Get-Content -LiteralPath (Join-Path $root "src/http/roon_routes.cpp") -Raw
$sidecar = Get-Content -LiteralPath (Join-Path $root "src/roon/roon_sidecar_process.cpp") -Raw
$app = Get-Content -LiteralPath (Join-Path $root "ui/dist/app.js") -Raw

foreach ($needle in @(
    "[wasapi]",
    "capture mix format",
    "conversion path",
    "capture is silent"
)) {
    Require-Text $wasapi $needle "WASAPI diagnostics should include $needle"
}

foreach ($needle in @(
    "selected zone",
    "selected capture device",
    "reconnect requested",
    "capture device is silent"
)) {
    Require-Text $routes $needle "Roon HTTP diagnostics should log $needle"
}

Require-Text $sidecar "command=" "Sidecar diagnostics should log the sanitized start command"
Require-Text $sidecar "roon-sidecar.out.log" "Sidecar stdout log file should be named"
Require-Text $sidecar "roon-sidecar.err.log" "Sidecar stderr log file should be named"
Require-Text $app "setupNote" "Web Control should surface actionable setup guidance"

Write-Host "diagnostics logging tests passed"
