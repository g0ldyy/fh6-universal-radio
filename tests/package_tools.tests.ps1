$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root "scripts\package-tools.ps1")

function Assert-Exists([string]$Path, [string]$Message) {
    if (-not (Test-Path $Path)) { throw $Message }
}

function Assert-Missing([string]$Path, [string]$Message) {
    if (Test-Path $Path) { throw $Message }
}

$tmp = Join-Path $root "tmp\package-tools-tests"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Force -Path $tmp | Out-Null

try {
    $src = Join-Path $tmp "source"
    New-Item -ItemType Directory -Force -Path (Join-Path $src "lib") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $src "test") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $src "node_modules\dep") | Out-Null
    Set-Content -Path (Join-Path $src "index.mjs") -Value "import './lib/api-server.mjs';" -Encoding ascii
    Set-Content -Path (Join-Path $src "package.json") -Value "{}" -Encoding ascii
    Set-Content -Path (Join-Path $src "package-lock.json") -Value "{}" -Encoding ascii
    Set-Content -Path (Join-Path $src "README.md") -Value "readme" -Encoding ascii
    Set-Content -Path (Join-Path $src "THIRD_PARTY_NOTICES.md") -Value "notices" -Encoding ascii
    Set-Content -Path (Join-Path $src "lib\api-server.mjs") -Value "export {};" -Encoding ascii
    Set-Content -Path (Join-Path $src "test\api.test.mjs") -Value "test" -Encoding ascii
    Set-Content -Path (Join-Path $src "node_modules\dep\index.js") -Value "module.exports = {};" -Encoding ascii

    $dst = Join-Path $tmp "dist\roon-bridge"
    Copy-RoonBridgePackage -SourceDir $src -DestinationDir $dst

    Assert-Exists (Join-Path $dst "index.mjs") "sidecar entrypoint should be copied"
    Assert-Exists (Join-Path $dst "package.json") "package manifest should be copied"
    Assert-Exists (Join-Path $dst "package-lock.json") "package lock should be copied"
    Assert-Exists (Join-Path $dst "lib\api-server.mjs") "runtime library should be copied"
    Assert-Exists (Join-Path $dst "THIRD_PARTY_NOTICES.md") "license notices should be copied"
    Assert-Missing (Join-Path $dst "test\api.test.mjs") "test files should not be staged"
    Assert-Missing (Join-Path $dst "node_modules\dep\index.js") "node_modules should be optional"

    $withModules = Join-Path $tmp "dist-with-modules\roon-bridge"
    Copy-RoonBridgePackage -SourceDir $src -DestinationDir $withModules -IncludeNodeModules
    Assert-Exists (Join-Path $withModules "node_modules\dep\index.js") "runtime dependencies should be copied when requested"
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

Write-Host "package_tools tests passed"
