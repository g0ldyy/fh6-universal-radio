$ErrorActionPreference = "Stop"

function Copy-DirectoryContents {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDir,
        [Parameter(Mandatory = $true)][string]$DestinationDir
    )

    if (-not (Test-Path $SourceDir)) { throw "Source directory not found: $SourceDir" }
    New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null
    Get-ChildItem -LiteralPath $SourceDir -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $DestinationDir $_.Name) `
            -Recurse -Force
    }
}

function Copy-RoonBridgePackage {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDir,
        [Parameter(Mandatory = $true)][string]$DestinationDir,
        [switch]$IncludeNodeModules
    )

    foreach ($required in @("index.mjs", "package.json", "package-lock.json", "lib")) {
        if (-not (Test-Path (Join-Path $SourceDir $required))) {
            throw "Roon sidecar package is missing $required under $SourceDir."
        }
    }

    New-Item -ItemType Directory -Force -Path $DestinationDir | Out-Null
    foreach ($file in @("index.mjs", "package.json", "package-lock.json", "README.md",
            "THIRD_PARTY_NOTICES.md")) {
        $src = Join-Path $SourceDir $file
        if (Test-Path $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $DestinationDir $file) -Force
        }
    }

    Copy-DirectoryContents `
        -SourceDir (Join-Path $SourceDir "lib") `
        -DestinationDir (Join-Path $DestinationDir "lib")

    if (Test-Path (Join-Path $SourceDir "vendor")) {
        Copy-DirectoryContents `
            -SourceDir (Join-Path $SourceDir "vendor") `
            -DestinationDir (Join-Path $DestinationDir "vendor")
    }

    if ($IncludeNodeModules -and (Test-Path (Join-Path $SourceDir "node_modules"))) {
        Copy-DirectoryContents `
            -SourceDir (Join-Path $SourceDir "node_modules") `
            -DestinationDir (Join-Path $DestinationDir "node_modules")
    }
}

function Install-RoonBridgeRuntimeDependencies {
    param([Parameter(Mandatory = $true)][string]$BridgeDir)

    $npm = Get-Command npm.cmd -ErrorAction SilentlyContinue
    if (-not $npm) { $npm = Get-Command npm -ErrorAction SilentlyContinue }
    if (-not $npm) { throw "npm not found; install Node.js/npm to package the Roon sidecar." }

    Push-Location $BridgeDir
    try {
        & $npm.Source ci --omit=dev --ignore-scripts --no-audit --no-fund | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "npm ci failed while staging the Roon sidecar." }
    } finally {
        Pop-Location
    }
}
