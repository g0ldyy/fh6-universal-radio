$ErrorActionPreference = "Stop"

function Get-VisualStudioGeneratorName([int]$MajorVersion) {
    switch ($MajorVersion) {
        18 { return "Visual Studio 18 2026" }
        17 { return "Visual Studio 17 2022" }
        16 { return "Visual Studio 16 2019" }
        15 { return "Visual Studio 15 2017" }
        default { return $null }
    }
}

function Select-CMakeForVisualStudio {
    param(
        [Parameter(Mandatory = $true)] [object[]] $Candidates,
        [Parameter(Mandatory = $true)] [int[]] $VisualStudioMajorVersions
    )

    foreach ($major in $VisualStudioMajorVersions) {
        $generator = Get-VisualStudioGeneratorName $major
        if (-not $generator) { continue }

        foreach ($candidate in $Candidates) {
            if ($candidate.Generators -contains $generator) {
                return [pscustomobject]@{
                    CMakePath = $candidate.Path
                    Generator = $generator
                }
            }
        }
    }

    throw "No CMake candidate supports the installed Visual Studio generator."
}

function New-CMakeConfigureArguments {
    param(
        [Parameter(Mandatory = $true)] [string] $SourceDir,
        [Parameter(Mandatory = $true)] [string] $BuildDir,
        [Parameter(Mandatory = $true)] [string] $Generator
    )

    return @("-S", $SourceDir, "-B", $BuildDir, "-G", $Generator, "-A", "x64")
}

function Get-CMakeGeneratorNames([string]$CMakePath) {
    $help = & $CMakePath --help
    return $help |
        Where-Object { $_ -match "^\s*(\*?\s*)?(Visual Studio \d+ \d{4}|NMake Makefiles|Ninja(?: Multi-Config)?)\s+=" } |
        ForEach-Object {
            ($_ -replace "^\s*\*?\s*", "" -replace "\s+=.*$", "").Trim()
        }
}

function Get-CMakeCandidates {
    param([string[]]$VisualStudioInstallPaths)

    $seen = @{}
    $paths = @()

    foreach ($vs in $VisualStudioInstallPaths) {
        $p = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
        if (Test-Path $p) { $paths += $p }
    }

    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { $paths += $cmd.Source }

    $paths += @(
        "${env:ProgramFiles}\CMake\bin\cmake.exe",
        "${env:ProgramFiles(x86)}\CMake\bin\cmake.exe"
    )

    foreach ($p in $paths) {
        if (-not $p -or -not (Test-Path $p)) { continue }
        $resolved = (Resolve-Path $p).Path
        if ($seen.ContainsKey($resolved)) { continue }
        $seen[$resolved] = $true
        [pscustomobject]@{
            Path       = $resolved
            Generators = @(Get-CMakeGeneratorNames $resolved)
        }
    }
}

function Get-VisualStudioInstallInfo {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return @() }

    $json = & $vswhere -all -products * -format json
    if (-not $json) { return @() }

    $instances = $json | ConvertFrom-Json
    foreach ($instance in $instances) {
        $major = [int]($instance.installationVersion -replace "\..*$", "")
        [pscustomobject]@{
            Path         = $instance.installationPath
            MajorVersion = $major
        }
    }
}

function Find-CMakeForBuild {
    $vsInfo = @(Get-VisualStudioInstallInfo | Sort-Object MajorVersion -Descending)
    if ($vsInfo.Count -eq 0) {
        throw "Visual Studio with the Desktop development with C++ workload was not found."
    }

    $candidates = @(Get-CMakeCandidates -VisualStudioInstallPaths @($vsInfo.Path))
    if ($candidates.Count -eq 0) {
        throw "cmake.exe not found."
    }

    return Select-CMakeForVisualStudio `
        -Candidates $candidates `
        -VisualStudioMajorVersions @($vsInfo.MajorVersion)
}
