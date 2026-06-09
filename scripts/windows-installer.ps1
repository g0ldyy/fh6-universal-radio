param(
    [string] $GameDir = "",
    [string] $AppDir = (Join-Path $env:LOCALAPPDATA "FH6 Universal Radio"),
    [switch] $NoCompanion,
    [switch] $NoStartupShortcut,
    [switch] $NoStartCompanion,
    [switch] $InstallVBCable
)

$ErrorActionPreference = "Stop"
$packageRoot = $PSScriptRoot
$appName = "FH6 Universal Radio"
$companionExe = "fh6-radio-companion.exe"

function Resolve-GameDir {
    param([string] $Requested)

    if ($Requested) { return $Requested }

    $candidates = @(
        "E:\SteamLibrary\steamapps\common\ForzaHorizon6",
        "C:\Program Files (x86)\Steam\steamapps\common\ForzaHorizon6",
        "C:\XboxGames\Forza Horizon 6\Content"
    )

    foreach ($candidate in $candidates) {
        if (Test-Path (Join-Path $candidate "forzahorizon6.exe")) {
            return $candidate
        }
    }

    throw "GameDir was not provided and FH6 was not found in the common install locations. Re-run with -GameDir `"C:\path\to\ForzaHorizon6`"."
}

function Stop-Companion {
    Get-Process -Name "fh6-radio-companion" -ErrorAction SilentlyContinue | Stop-Process -Force
}

function Test-VBCableInstalled {
    try {
        $devices = Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
            Where-Object { $_.Name -match "VB-Audio Virtual Cable|CABLE Input|CABLE Output" }
        return [bool]$devices
    } catch {
        return $false
    }
}

function Backup-AndCopy {
    param([string] $Source, [string] $Dest, [string] $Root, [switch] $SkipBackup)

    $destDir = Split-Path -Parent $Dest
    if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Force -Path $destDir | Out-Null }
    if ((Test-Path $Dest) -and -not $SkipBackup -and -not (Test-Path "$Dest.bak")) {
        Copy-Item $Dest "$Dest.bak" -Force
    }
    Copy-Item $Source $Dest -Force
    Write-Host "  + $($Dest.Substring($Root.Length + 1))"
}

function Write-Shortcut {
    param([string] $Path, [string] $Target, [string] $WorkingDirectory)

    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($Path)
    $shortcut.TargetPath = $Target
    $shortcut.WorkingDirectory = $WorkingDirectory
    $shortcut.Description = "Routes VB-CABLE to the default output while FH6 is closed."
    $shortcut.Save()
}

$gameDir = Resolve-GameDir $GameDir
if (-not (Test-Path (Join-Path $gameDir "forzahorizon6.exe"))) {
    throw "forzahorizon6.exe not found in $gameDir"
}
if (Get-Process -Name "forzahorizon6" -ErrorAction SilentlyContinue) {
    throw "FH6 is running. Close the game before installing or updating the DLL."
}
if ($NoCompanion -and $InstallVBCable) {
    throw "-InstallVBCable requires the companion app files. Remove -NoCompanion or run Install-VBCable.ps1 separately."
}
if (-not (Test-Path (Join-Path $packageRoot "version.dll"))) {
    throw "version.dll not found next to this installer."
}
if (-not (Test-Path (Join-Path $packageRoot "fh6-radio\ui"))) {
    throw "fh6-radio\ui not found next to this installer."
}

Write-Host "Installing $appName" -ForegroundColor Cyan
Write-Host "Game: $gameDir"
Write-Host "App:  $AppDir"

Stop-Companion

$gameDataDir = Join-Path $gameDir "fh6-radio"
$existingInstall = Test-Path (Join-Path $gameDataDir "config.toml")
Backup-AndCopy `
    (Join-Path $packageRoot "version.dll") `
    (Join-Path $gameDir "version.dll") `
    $gameDir `
    -SkipBackup:$existingInstall

if (-not (Test-Path $gameDataDir)) { New-Item -ItemType Directory -Force -Path $gameDataDir | Out-Null }
Copy-Item -Recurse -Force (Join-Path $packageRoot "fh6-radio\ui") $gameDataDir

$gameConfig = Join-Path $gameDataDir "config.toml"
if (-not (Test-Path $gameConfig)) {
    Copy-Item (Join-Path $packageRoot "config.example.toml") $gameConfig
    Write-Host "  + fh6-radio\config.toml"
} else {
    Write-Host "  = fh6-radio\config.toml preserved"
}

if (-not $NoCompanion) {
    if (-not (Test-Path (Join-Path $packageRoot $companionExe))) {
        throw "$companionExe not found next to this installer."
    }

    if (-not (Test-Path $AppDir)) { New-Item -ItemType Directory -Force -Path $AppDir | Out-Null }
    Copy-Item (Join-Path $packageRoot $companionExe) $AppDir -Force
    Copy-Item (Join-Path $packageRoot "Uninstall-FH6UniversalRadio.ps1") $AppDir -Force
    Copy-Item (Join-Path $packageRoot "Install-VBCable.ps1") $AppDir -Force
    Copy-Item (Join-Path $packageRoot "INSTALL.txt") $AppDir -Force
    if (Test-Path (Join-Path $packageRoot "vbcable")) {
        Copy-Item -Recurse -Force (Join-Path $packageRoot "vbcable") $AppDir
    }
    if (Test-Path (Join-Path $packageRoot "package-manifest.json")) {
        Copy-Item (Join-Path $packageRoot "package-manifest.json") $AppDir -Force
    }

    $manifest = @{
        installedAt = (Get-Date).ToString("o")
        gameDir = $gameDir
        appDir = $AppDir
        companion = -not $NoCompanion
    } | ConvertTo-Json
    Set-Content -LiteralPath (Join-Path $AppDir "install.json") -Value $manifest

    if (-not $NoStartupShortcut) {
        $startup = [Environment]::GetFolderPath("Startup")
        Write-Shortcut `
            -Path (Join-Path $startup "FH6 Radio Companion.lnk") `
            -Target (Join-Path $AppDir $companionExe) `
            -WorkingDirectory $AppDir
        Write-Host "  + Startup companion shortcut"
    }

    if (-not $NoStartCompanion) {
        Start-Process -FilePath (Join-Path $AppDir $companionExe) -WorkingDirectory $AppDir -WindowStyle Hidden
        Write-Host "  + Companion started"
    }
}

if ($InstallVBCable) {
    if (Test-VBCableInstalled) {
        Write-Host "  = VB-CABLE already installed"
    } else {
        $vbCableInstaller = Join-Path $AppDir "Install-VBCable.ps1"
        if (-not (Test-Path $vbCableInstaller)) {
            throw "Install-VBCable.ps1 was not installed to $AppDir"
        }
        & $vbCableInstaller
    }
}

Write-Host "`nDone. Re-run this installer later to update the DLL, dashboard, and companion." -ForegroundColor Green
Write-Host "Launch FH6, set Audio -> Radio DJ = Off, Streamer Mode = On, then open http://localhost:8420."
