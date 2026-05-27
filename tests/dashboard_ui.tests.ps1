Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = Resolve-Path (Join-Path $PSScriptRoot '..')
$js = (Get-ChildItem -LiteralPath (Join-Path $root 'ui/dist') -Filter '*.js' |
    Sort-Object Name |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
$html = Get-Content -LiteralPath (Join-Path $root 'ui/dist/index.html') -Raw
$css = Get-Content -LiteralPath (Join-Path $root 'ui/dist/styles.css') -Raw
$surface = "$html`n$js"

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

Require-Text $js '["roon", "Roon"' 'settings schema should include a Roon section'
foreach ($field in @(
    'node_path',
    'bridge_path',
    'selected_zone_id',
    'render_loopback_endpoint_id',
    'render_loopback_endpoint_name',
    'control_volume',
    'auto_start_bridge',
    'auto_reconnect',
    'latency_ms',
    'metadata_poll_ms'
)) {
    Require-Text $js $field "Roon settings should include $field"
}

foreach ($route in @(
    '/api/source/roon/setup',
    '/api/source/roon/status',
    '/api/source/roon/zones',
    '/api/source/roon/loopback-endpoints',
    '/api/source/roon/select-zone',
    '/api/source/roon/select-capture-device',
    '/api/source/roon/test-capture',
    '/api/source/roon/reconnect',
    '/api/source/roon/artwork/current'
)) {
    Require-Text $js $route "Dashboard should call $route"
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

foreach ($text in @(
    'Roon Output / Loopback Capture Device',
    'Open Roon download',
    'Open VB-Audio download',
    'Recheck',
    'Use recommended device',
    'Test audio'
)) {
    Require-Text $surface $text "Roon setup panel should include '$text'"
}

foreach ($forbidden in @(
    'winget install',
    'PowerShell',
    'cmd.exe'
)) {
    if ($surface.Contains($forbidden)) {
        throw "Roon setup panel should not expose CLI setup string '$forbidden'"
    }
}

Require-Text $css '.roon-panel' 'styles should include the Roon panel'
Require-Text $js 'renderRoonPanel' 'app should render the Roon setup panel idempotently'
Require-Text $js 'roonNowPlaying' 'app should prefer Roon metadata when Roon is active'
