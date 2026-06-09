param(
    [string] $ZipPath = (Join-Path $PSScriptRoot "vbcable\VBCABLE_Driver_Pack45.zip")
)

$ErrorActionPreference = "Stop"

function Test-VBCableInstalled {
    try {
        $devices = Get-CimInstance Win32_PnPEntity -ErrorAction Stop |
            Where-Object { $_.Name -match "VB-Audio Virtual Cable|CABLE Input|CABLE Output" }
        return [bool]$devices
    } catch {
        return $false
    }
}

if (Test-VBCableInstalled) {
    Write-Host "VB-CABLE appears to already be installed." -ForegroundColor Green
    exit 0
}

if (-not (Test-Path $ZipPath)) {
    Write-Warning "VB-CABLE package was not found at $ZipPath"
    Write-Host "Opening the official VB-Audio download page instead."
    Start-Process "https://vb-audio.com/Cable/"
    exit 1
}

$work = Join-Path $env:TEMP "FH6UniversalRadio\VBCABLE"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force -Path $work | Out-Null

Expand-Archive -LiteralPath $ZipPath -DestinationPath $work -Force

$setup = Get-ChildItem -Path $work -Recurse -Filter "VBCABLE_Setup_x64.exe" -File |
    Select-Object -First 1
if (-not $setup) {
    $setup = Get-ChildItem -Path $work -Recurse -Filter "VBCABLE_Setup.exe" -File |
        Select-Object -First 1
}
if (-not $setup) {
    throw "Could not find the VB-CABLE setup executable after extracting $ZipPath"
}

$sig = Get-AuthenticodeSignature -FilePath $setup.FullName
if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notmatch "VB-Audio|Vincent Burel") {
    throw "Refusing to run unsigned/untrusted VB-CABLE setup: $($setup.FullName)"
}

Write-Host "Launching VB-CABLE setup from VB-Audio." -ForegroundColor Cyan
Write-Host "Choose Install Driver in the VB-CABLE window, then reboot Windows when prompted."
$proc = Start-Process -FilePath $setup.FullName -Verb RunAs -Wait -PassThru
if ($proc.ExitCode -ne 0) {
    throw "VB-CABLE setup exited with code $($proc.ExitCode)"
}

Write-Host "VB-CABLE setup closed. Reboot Windows if the installer requested it." -ForegroundColor Green
