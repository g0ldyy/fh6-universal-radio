$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root "scripts\lint-tools.ps1")

function Assert-Equal($Actual, $Expected, [string]$Message) {
    if ($Actual -ne $Expected) {
        throw "$Message Expected '$Expected', got '$Actual'."
    }
}

function Assert-SetEqual([string[]]$Actual, [string[]]$Expected, [string]$Message) {
    $a = @($Actual | Sort-Object)
    $e = @($Expected | Sort-Object)
    Assert-Equal $a.Count $e.Count "$Message Count mismatch."
    for ($i = 0; $i -lt $e.Count; $i++) {
        Assert-Equal $a[$i] $e[$i] "$Message Item $i mismatch."
    }
}

function New-FileInfoLike([string]$RelativePath) {
    [pscustomobject]@{
        FullName = Join-Path $root $RelativePath
    }
}

$cppFiles = @(
    New-FileInfoLike "src\bridge.cpp"
    New-FileInfoLike "src\sources\roon_source.cpp"
    New-FileInfoLike "src\unused.cpp"
)
$headerFiles = @(
    New-FileInfoLike "include\fh6\config.hpp"
    New-FileInfoLike "include\fh6\sources\roon_source.hpp"
)
$webFiles = @(
    New-FileInfoLike "ui\dist\app.js"
    New-FileInfoLike "ui\dist\styles.css"
    New-FileInfoLike "ui\dist\index.html"
)

$changed = @(
    "src\bridge.cpp",
    "include/fh6/config.hpp",
    "ui\dist\app.js",
    "docs\checklist.md",
    "CMakeLists.txt",
    "tests\lint_tools.tests.ps1"
)

$changedTargets = Select-LintTargets `
    -Root $root `
    -ChangedOnly `
    -ChangedPaths $changed `
    -CppFiles $cppFiles `
    -HeaderFiles $headerFiles `
    -WebFiles $webFiles

Assert-SetEqual $changedTargets.CppFormatPaths @(
    (Join-Path $root "src\bridge.cpp"),
    (Join-Path $root "include\fh6\config.hpp")
) "Changed-only C++ format selection should include changed source/header files."
Assert-SetEqual $changedTargets.CppTidyPaths @(
    (Join-Path $root "src\bridge.cpp")
) "Changed-only clang-tidy selection should include changed .cpp files only."
Assert-SetEqual $changedTargets.WebPaths @(
    (Join-Path $root "ui\dist\app.js")
) "Changed-only web selection should include changed dashboard files only."

$allTargets = Select-LintTargets `
    -Root $root `
    -ChangedPaths @() `
    -CppFiles $cppFiles `
    -HeaderFiles $headerFiles `
    -WebFiles $webFiles

Assert-Equal $allTargets.CppFormatPaths.Count 5 "Full C++ format selection should include all C++ sources and headers."
Assert-Equal $allTargets.CppTidyPaths.Count 3 "Full clang-tidy selection should include all C++ sources."
Assert-Equal $allTargets.WebPaths.Count 3 "Full web selection should include all dashboard files."

Assert-Equal (Resolve-TidyJobCount -RequestedJobs 3 -FileCount 8 -ProcessorCount 16) 3 `
    "Explicit tidy job count should be honored."
Assert-Equal (Resolve-TidyJobCount -RequestedJobs 99 -FileCount 8 -ProcessorCount 16) 8 `
    "Tidy job count should not exceed file count."
Assert-Equal (Resolve-TidyJobCount -RequestedJobs 0 -FileCount 8 -ProcessorCount 4) 4 `
    "Automatic tidy job count should use available processors."
Assert-Equal (Resolve-TidyJobCount -RequestedJobs 0 -FileCount 0 -ProcessorCount 4) 1 `
    "Tidy job count should stay valid when there are no files."

Write-Host "lint_tools tests passed"
