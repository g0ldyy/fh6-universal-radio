Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$app = Get-Content -LiteralPath (Join-Path $root 'ui/dist/app.js') -Raw
$html = Get-Content -LiteralPath (Join-Path $root 'ui/dist/index.html') -Raw
$css = Get-Content -LiteralPath (Join-Path $root 'ui/dist/styles.css') -Raw

function Require-Text {
    param(
        [string]$Text,
        [string]$Needle,
        [string]$Message
    )
    if (-not $Text.Contains($Needle)) {
        throw $Message
    }
}

Require-Text $app '["roon", "Roon"' 'settings schema should include a Roon section'
foreach ($field in @(
    'node_path',
    'bridge_path',
    'selected_zone_id',
    'capture_device_id',
    'control_volume',
    'auto_start_bridge',
    'auto_reconnect',
    'latency_ms',
    'metadata_poll_ms'
)) {
    Require-Text $app $field "Roon settings should include $field"
}

foreach ($route in @(
    '/api/source/roon/status',
    '/api/source/roon/zones',
    '/api/source/roon/capture-devices',
    '/api/source/roon/select-zone',
    '/api/source/roon/select-capture-device',
    '/api/source/roon/test-capture',
    '/api/source/roon/reconnect',
    '/api/source/roon/artwork/current'
)) {
    Require-Text $app $route "Dashboard should call $route"
}

foreach ($id in @(
    'roon-setup-card',
    'roon-pairing',
    'roon-zone',
    'roon-capture',
    'roon-reconnect',
    'roon-test-capture',
    'roon-error'
)) {
    Require-Text $html $id "Dashboard markup should include #$id"
}

Require-Text $css '.roon-panel' 'styles should include the Roon panel'
Require-Text $app 'renderRoonPanel' 'app should render the Roon setup panel idempotently'
Require-Text $app 'roonNowPlaying' 'app should prefer Roon metadata when Roon is active'
