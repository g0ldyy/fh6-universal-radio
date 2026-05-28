$ErrorActionPreference = "Stop"

function ConvertTo-RepoRelativePath([string]$Root, [string]$Path) {
    $rootFull = [IO.Path]::GetFullPath($Root).TrimEnd('\', '/')
    $pathFull = if ([IO.Path]::IsPathRooted($Path)) {
        [IO.Path]::GetFullPath($Path)
    } else {
        [IO.Path]::GetFullPath((Join-Path $rootFull $Path))
    }

    if ($pathFull.StartsWith($rootFull, [StringComparison]::OrdinalIgnoreCase)) {
        $pathFull = $pathFull.Substring($rootFull.Length).TrimStart('\', '/')
    }

    return $pathFull.Replace('/', '\')
}

function Get-GitChangedPaths([string]$Root, [string]$BaseRef = "HEAD") {
    $tracked = @(& git -C $Root diff --name-only --diff-filter=ACMRTUXB $BaseRef --)
    if ($LASTEXITCODE -ne 0) { throw "git diff failed while resolving changed lint targets." }

    $untracked = @(& git -C $Root ls-files --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw "git ls-files failed while resolving changed lint targets." }

    return @($tracked + $untracked) |
        Where-Object { $_ } |
        Sort-Object -Unique
}

function Select-LintTargets {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [switch]$ChangedOnly,
        [string[]]$ChangedPaths = @(),
        [object[]]$CppFiles = @(),
        [object[]]$HeaderFiles = @(),
        [object[]]$WebFiles = @(),
        [object[]]$SidecarFiles = @()
    )

    $changed = @{}
    if ($ChangedOnly) {
        foreach ($path in $ChangedPaths) {
            $key = (ConvertTo-RepoRelativePath $Root $path).ToLowerInvariant()
            $changed[$key] = $true
        }
    }

    function Select-Paths([object[]]$Files) {
        $paths = @()
        foreach ($file in $Files) {
            $full = [string]$file.FullName
            if (-not $ChangedOnly) {
                $paths += $full
                continue
            }

            $rel = (ConvertTo-RepoRelativePath $Root $full).ToLowerInvariant()
            if ($changed.ContainsKey($rel)) { $paths += $full }
        }
        return @($paths)
    }

    $cppFormat = @(Select-Paths @($CppFiles + $HeaderFiles))
    $cppTidy = @(Select-Paths $CppFiles)
    $web = @(Select-Paths $WebFiles)
    $sidecar = @(Select-Paths $SidecarFiles)

    return [pscustomobject]@{
        CppFormatPaths = $cppFormat
        CppTidyPaths   = $cppTidy
        WebPaths       = $web
        SidecarPaths   = $sidecar
    }
}

function Resolve-TidyJobCount {
    param(
        [int]$RequestedJobs,
        [int]$FileCount,
        [int]$ProcessorCount = [Environment]::ProcessorCount
    )

    if ($FileCount -le 0) { return 1 }
    $jobs = if ($RequestedJobs -gt 0) { $RequestedJobs } else { [Math]::Max(1, $ProcessorCount) }
    return [Math]::Max(1, [Math]::Min($jobs, $FileCount))
}
