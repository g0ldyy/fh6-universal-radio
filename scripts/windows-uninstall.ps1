param(
    [string] $GameDir = "",
    [string] $AppDir = (Join-Path $env:LOCALAPPDATA "FH6 Universal Radio"),
    [switch] $RemoveConfig
)

$ErrorActionPreference = "Stop"

function Resolve-GameDir {
    param([string] $Requested, [string] $InstallJson)

    if ($Requested) { return $Requested }
    if (Test-Path $InstallJson) {
        $state = Get-Content $InstallJson -Raw | ConvertFrom-Json
        if ($state.gameDir) { return [string] $state.gameDir }
    }
    throw "GameDir was not provided and no install state was found. Re-run with -GameDir `"C:\path\to\ForzaHorizon6`"."
}

$installJson = Join-Path $AppDir "install.json"
$gameDir = Resolve-GameDir $GameDir $installJson

if (Get-Process -Name "forzahorizon6" -ErrorAction SilentlyContinue) {
    throw "FH6 is running. Close the game before uninstalling the DLL."
}

Write-Host "Uninstalling FH6 Universal Radio" -ForegroundColor Cyan

Get-Process -Name "fh6-radio-companion" -ErrorAction SilentlyContinue | Stop-Process -Force

$dll = Join-Path $gameDir "version.dll"
$dllBak = "$dll.bak"
if (Test-Path $dllBak) {
    Move-Item -Path $dllBak -Destination $dll -Force
    Write-Host "  = version.dll restored from backup"
} elseif (Test-Path $dll) {
    Remove-Item $dll -Force
    Write-Host "  - version.dll"
}

if ($RemoveConfig) {
    $dataDir = Join-Path $gameDir "fh6-radio"
    if (Test-Path $dataDir) {
        Remove-Item $dataDir -Recurse -Force
        Write-Host "  - fh6-radio"
    }
}

$startupLink = Join-Path ([Environment]::GetFolderPath("Startup")) "FH6 Radio Companion.lnk"
if (Test-Path $startupLink) {
    $removeStartupLink = $true
    try {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($startupLink)
        if ($shortcut.TargetPath -and
            -not $shortcut.TargetPath.StartsWith($AppDir, [System.StringComparison]::OrdinalIgnoreCase)) {
            $removeStartupLink = $false
        }
    } catch {
        $removeStartupLink = $false
    }

    if ($removeStartupLink) {
        Remove-Item $startupLink -Force
        Write-Host "  - Startup companion shortcut"
    } else {
        Write-Host "  = Startup companion shortcut preserved"
    }
}

if (Test-Path $AppDir) {
    Remove-Item $AppDir -Recurse -Force
    Write-Host "  - $AppDir"
}

Write-Host "`nDone. VB-CABLE was left installed because it is a separate driver." -ForegroundColor Green
