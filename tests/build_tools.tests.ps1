$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root "scripts\build-tools.ps1")

function Assert-Equal($Actual, $Expected, [string]$Message) {
    if ($Actual -ne $Expected) {
        throw "$Message Expected '$Expected', got '$Actual'."
    }
}

function Assert-Contains([object[]]$Items, $Expected, [string]$Message) {
    if ($Items -notcontains $Expected) {
        throw "$Message Expected list to contain '$Expected'."
    }
}

function New-Candidate([string]$Path, [string[]]$Generators) {
    [pscustomobject]@{
        Path       = $Path
        Generators = $Generators
    }
}

$pathCMake = New-Candidate "C:\Program Files\CMake\bin\cmake.exe" @(
    "Visual Studio 17 2022",
    "NMake Makefiles"
)
$vsCMake = New-Candidate "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" @(
    "Visual Studio 18 2026",
    "Visual Studio 17 2022"
)

$selection = Select-CMakeForVisualStudio `
    -Candidates @($pathCMake, $vsCMake) `
    -VisualStudioMajorVersions @(18)

Assert-Equal $selection.CMakePath $vsCMake.Path "VS2026 selection should use a CMake that supports the installed VS generator."
Assert-Equal $selection.Generator "Visual Studio 18 2026" "VS2026 selection should choose the VS2026 generator."

$vs2022Selection = Select-CMakeForVisualStudio `
    -Candidates @($pathCMake) `
    -VisualStudioMajorVersions @(17)

Assert-Equal $vs2022Selection.CMakePath $pathCMake.Path "VS2022 selection should keep working with a compatible PATH CMake."
Assert-Equal $vs2022Selection.Generator "Visual Studio 17 2022" "VS2022 selection should choose the VS2022 generator."

$configureArgs = New-CMakeConfigureArguments `
    -SourceDir "C:\repo" `
    -BuildDir "C:\repo\build" `
    -Generator "Visual Studio 18 2026"

Assert-Contains $configureArgs "-G" "Configure args should pass an explicit generator."
Assert-Contains $configureArgs "Visual Studio 18 2026" "Configure args should include the selected generator."
Assert-Contains $configureArgs "-A" "Configure args should include an architecture for Visual Studio generators."
Assert-Contains $configureArgs "x64" "Configure args should target x64."

$threw = $false
try {
    Select-CMakeForVisualStudio `
        -Candidates @($pathCMake) `
        -VisualStudioMajorVersions @(18) | Out-Null
} catch {
    $threw = $true
}

Assert-Equal $threw $true "Selection should fail clearly when no CMake supports the installed Visual Studio generator."

Write-Host "build_tools tests passed"
