#   PS> .\scripts\lint.ps1                # format + check (write changes)
#   PS> .\scripts\lint.ps1 -CheckOnly     # no changes, exit non-zero on issues
#
# Runs:
#   * clang-format on src/**.cpp + include/**.hpp
#   * clang-tidy on src/**.cpp
#   * prettier on ui/dist/**.{js,css,html}
#   * eslint on ui/dist/**.js
#   * stylelint on ui/dist/**.css

param(
    [switch]$CheckOnly,
    [switch]$SkipCpp,
    [switch]$SkipWeb,
    [switch]$SkipTidy
)

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"

function Find-LlvmTool([string]$exe) {
    $cmd = Get-Command $exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -property installationPath
        foreach ($arch in @("x64", "ARM64", "")) {
            $p = Join-Path $vs ("VC\Tools\Llvm\{0}\bin\{1}.exe" -f $arch, $exe)
            if (Test-Path $p) { return $p }
        }
    }
    return $null
}

$fails = @()

if (-not $SkipCpp) {
    $clangFormat = Find-LlvmTool "clang-format"
    if (-not $clangFormat) {
        Write-Host "clang-format not found (install VS C++ workload or LLVM); skipping C++ format." -ForegroundColor Yellow
    } else {
        Write-Host "-> clang-format ($([IO.Path]::GetFileName($clangFormat)))" -ForegroundColor Cyan
        $cppFiles = @()
        $cppFiles += Get-ChildItem -Path (Join-Path $root "src")     -Recurse -Include *.cpp, *.hpp -File
        $cppFiles += Get-ChildItem -Path (Join-Path $root "include") -Recurse -Include *.hpp        -File
        $cppPaths = $cppFiles | ForEach-Object { $_.FullName }
        if ($CheckOnly) {
            & $clangFormat --dry-run --Werror @cppPaths
            if ($LASTEXITCODE -ne 0) { $fails += "clang-format" }
        } else {
            & $clangFormat -i @cppPaths
            if ($LASTEXITCODE -ne 0) { $fails += "clang-format" }
        }
    }

    if (-not $SkipTidy) {
        $clangTidy = Find-LlvmTool "clang-tidy"
        if (-not $clangTidy) {
            Write-Host "clang-tidy not found; skipping." -ForegroundColor Yellow
        } else {
            Write-Host "-> clang-tidy" -ForegroundColor Cyan
            # Inline include flags -- mirrors target_include_directories() in
            # CMakeLists.txt. Saves having to feed clang-tidy compile_commands.json
            # (which MSVC's CMake generator doesn't produce anyway).
            $cxxArgs = @(
                "-std=c++20", "-x", "c++",
                "-D_WIN32_WINNT=0x0A00", "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX",
                "-D_CRT_SECURE_NO_WARNINGS", "-DUNICODE", "-D_UNICODE",
                "-I$(Join-Path $root 'include')",
                "-I$(Join-Path $root 'third_party\nlohmann')",
                "-I$(Join-Path $root 'third_party\toml11')",
                "-I$(Join-Path $root 'third_party\miniaudio')"
            )
            $tidyFiles = Get-ChildItem -Path (Join-Path $root "src") -Recurse -Include *.cpp -File
            # PS 5.1 turns clang-tidy's "N warnings generated." stderr line
            # into a terminating error under $ErrorActionPreference="Stop"
            # even with 2>&1 capture, so relax for this block only.
            $prevEAP = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            foreach ($f in $tidyFiles) {
                $out = & $clangTidy --quiet $f.FullName -- @cxxArgs 2>&1 | Out-String
                $diag = ($out -split "`n") | Where-Object { $_ -match ":\d+:\d+: (warning|error):" }
                if ($diag) {
                    Write-Host "  $($f.Name): $($diag.Count) diagnostic(s)" -ForegroundColor Yellow
                    $diag | ForEach-Object { Write-Host "    $_" }
                    $fails += "clang-tidy ($($f.Name))"
                }
            }
            $ErrorActionPreference = $prevEAP
        }
    }
}

if (-not $SkipWeb) {
    $npm = Get-Command npm -ErrorAction SilentlyContinue
    if (-not $npm) {
        Write-Host "npm not found; skipping web formatting/linting." -ForegroundColor Yellow
    } else {
        if (-not (Test-Path (Join-Path $root "node_modules"))) {
            Write-Host "-> npm install (first run)" -ForegroundColor Cyan
            Push-Location $root
            try   { & npm.cmd install --no-audit --no-fund | Out-Host }
            finally { Pop-Location }
            if ($LASTEXITCODE -ne 0) { $fails += "npm install"; }
        }

        Push-Location $root
        try {
            if ($CheckOnly) {
                Write-Host "-> npm run check" -ForegroundColor Cyan
                & npm.cmd run check
                if ($LASTEXITCODE -ne 0) { $fails += "web check" }
            } else {
                Write-Host "-> npm run fix" -ForegroundColor Cyan
                & npm.cmd run fix
                if ($LASTEXITCODE -ne 0) { $fails += "web fix" }
            }
        }
        finally { Pop-Location }
    }
}

if ($fails.Count -gt 0) {
    Write-Host ""
    Write-Host ("Failed: " + ($fails -join ", ")) -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "OK." -ForegroundColor Green
