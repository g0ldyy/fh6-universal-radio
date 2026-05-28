$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$maxLines = 500
$paths = @(
    "CMakeLists.txt",
    "config.example.toml",
    "package.json"
)
$paths += git -C $root ls-files "include/*" "src/*" "scripts/*" "ui/dist/*" "tests/*"

$violations = @()
foreach ($relativePath in $paths | Sort-Object -Unique) {
    $path = Join-Path $root $relativePath
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }

    $lineCount = @(Get-Content -LiteralPath $path).Count
    if ($lineCount -gt $maxLines) {
        $violations += "{0}: {1} lines" -f $relativePath, $lineCount
    }
}

if ($violations.Count -gt 0) {
    throw "Files exceed ${maxLines}-line limit:`n$($violations -join "`n")"
}

Write-Host "file line limit tests passed"
