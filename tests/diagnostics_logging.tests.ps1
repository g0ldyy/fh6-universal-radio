$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot

function Assert-Text([string]$Text, [string]$Needle, [string]$Message) {
    if (-not $Text.Contains($Needle)) { throw $Message }
}

$wasapi = Get-Content -LiteralPath (Join-Path $root "src/audio/wasapi_loopback_capture.cpp") -Raw
$http = Get-Content -LiteralPath (Join-Path $root "src/http/http_server.cpp") -Raw
$routes = Get-Content -LiteralPath (Join-Path $root "src/http/roon_routes.cpp") -Raw
$sidecar = Get-Content -LiteralPath (Join-Path $root "src/roon/roon_sidecar_process.cpp") -Raw
$deps = Get-Content -LiteralPath (Join-Path $root "scripts/get-deps.ps1") -Raw
$js = (Get-ChildItem -LiteralPath (Join-Path $root "ui/dist") -Filter "*.js" |
    Sort-Object Name |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"

foreach ($needle in @(
    "[wasapi]",
    "capture mix format",
    "conversion path",
    "capture is silent"
)) {
    Assert-Text $wasapi $needle "WASAPI diagnostics should include $needle"
}

Assert-Text $wasapi "kStartReadyTimeout" "WASAPI capture startup should use a bounded ready wait"
Assert-Text $wasapi "wait_for(kStartReadyTimeout)" "WASAPI capture start should not block indefinitely"
Assert-Text $wasapi "catch (const std::exception& e)" "WASAPI worker should convert exceptions into capture errors"
Assert-Text $wasapi "ReleaseBufferGuard" "WASAPI worker should release packets through an RAII guard"

Assert-Text $http "INADDR_LOOPBACK" "Dashboard HTTP server should bind to loopback by default"
if ($http.Contains("INADDR_ANY")) {
    throw "Dashboard HTTP server should not bind to every LAN interface by default"
}
if ($http.Contains("Access-Control-Allow-Origin: *")) {
    throw "Dashboard HTTP server should not allow arbitrary cross-origin callers"
}

foreach ($needle in @(
    "selected zone",
    "selected loopback endpoint",
    "reconnect requested",
    "capture device is silent",
    "kCaptureSignalWait"
)) {
    Assert-Text $routes $needle "Roon HTTP diagnostics should log $needle"
}

if ($routes.Contains("milliseconds{1500}")) {
    throw "Roon test-capture should not block the single-threaded dashboard for 1500ms"
}

Assert-Text $sidecar "command=" "Sidecar diagnostics should log the sanitized start command"
Assert-Text $sidecar "roon-sidecar.out.log" "Sidecar stdout log file should be named"
Assert-Text $sidecar "roon-sidecar.err.log" "Sidecar stderr log file should be named"
Assert-Text $deps "cpp-httplib" "Dependency bootstrap should fetch cpp-httplib for clean checkouts"
Assert-Text $deps "Get-FileHash" "Dependency bootstrap should verify downloaded headers"
foreach ($mutable in @("/develop/", "/main/", "/master/")) {
    if ($deps.Contains($mutable)) { throw "Dependency bootstrap should use pinned immutable URLs, not $mutable" }
}
Assert-Text $js "setupNote" "Web Control should surface actionable setup guidance"

Write-Host "diagnostics logging tests passed"
