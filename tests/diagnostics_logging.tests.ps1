$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot

function Require-Text([string]$Text, [string]$Needle, [string]$Message) {
    if (-not $Text.Contains($Needle)) { throw $Message }
}

$wasapi = Get-Content -LiteralPath (Join-Path $root "src/audio/wasapi_loopback_capture.cpp") -Raw
$routes = Get-Content -LiteralPath (Join-Path $root "src/http/roon_routes.cpp") -Raw
$sidecar = Get-Content -LiteralPath (Join-Path $root "src/roon/roon_sidecar_process.cpp") -Raw
$js = (Get-ChildItem -LiteralPath (Join-Path $root "ui/dist") -Filter "*.js" |
    Sort-Object Name |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"

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
    "selected loopback endpoint",
    "reconnect requested",
    "capture device is silent",
    "kCaptureSignalWait"
)) {
    Require-Text $routes $needle "Roon HTTP diagnostics should log $needle"
}

if ($routes.Contains("milliseconds{1500}")) {
    throw "Roon test-capture should not block the single-threaded dashboard for 1500ms"
}

Require-Text $sidecar "command=" "Sidecar diagnostics should log the sanitized start command"
Require-Text $sidecar "roon-sidecar.out.log" "Sidecar stdout log file should be named"
Require-Text $sidecar "roon-sidecar.err.log" "Sidecar stderr log file should be named"
Require-Text $js "setupNote" "Web Control should surface actionable setup guidance"

Write-Host "diagnostics logging tests passed"
