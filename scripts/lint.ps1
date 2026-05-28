#   PS> .\scripts\lint.ps1                # format + check (write changes)
#   PS> .\scripts\lint.ps1 -CheckOnly     # no changes, exit non-zero on issues
#
# Runs:
#   * clang-format on src/**.cpp + include/**.hpp
#   * clang-tidy on src/**.cpp
#   * prettier on ui/dist/**.{js,css,html}
#   * eslint on ui/dist/**.js
#   * stylelint on ui/dist/**.css
#   * node --test for tools/roon-bridge when sidecar files are selected

param(
    [switch]$CheckOnly,
    [switch]$SkipCpp,
    [switch]$SkipWeb,
    [switch]$SkipTidy,
    [switch]$ChangedOnly,
    [string]$ChangedBase = "HEAD",
    [int]$TidyJobs = 0
)

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
. (Join-Path $PSScriptRoot "lint-tools.ps1")

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
$changedPaths = @()
if ($ChangedOnly) {
    $changedPaths = @(Get-GitChangedPaths -Root $root -BaseRef $ChangedBase)
    Write-Host "-> changed-only lint targets from git diff $ChangedBase" -ForegroundColor Cyan
}

$cppFormatFiles = @()
$cppFormatFiles += Get-ChildItem -Path (Join-Path $root "src")     -Recurse -Include *.cpp, *.hpp -File
$cppFormatFiles += Get-ChildItem -Path (Join-Path $root "include") -Recurse -Include *.hpp        -File
$cppTidyFiles = Get-ChildItem -Path (Join-Path $root "src") -Recurse -Include *.cpp -File
$webFiles = Get-ChildItem -Path (Join-Path $root "ui\dist") -Recurse -Include *.js, *.css, *.html -File
$sidecarFiles = Get-ChildItem -Path (Join-Path $root "tools\roon-bridge") `
    -Recurse -Include *.mjs, package.json, package-lock.json -File |
    Where-Object { $_.FullName -notmatch "\\node_modules\\" }

$targets = Select-LintTargets `
    -Root $root `
    -ChangedOnly:$ChangedOnly `
    -ChangedPaths $changedPaths `
    -CppFiles $cppTidyFiles `
    -HeaderFiles ($cppFormatFiles | Where-Object { $_.Extension -ieq ".hpp" }) `
    -WebFiles $webFiles `
    -SidecarFiles $sidecarFiles

if (-not $SkipCpp) {
    $clangFormat = Find-LlvmTool "clang-format"
    if (-not $clangFormat) {
        Write-Host "clang-format not found (install VS C++ workload or LLVM); skipping C++ format." -ForegroundColor Yellow
    } else {
        Write-Host "-> clang-format ($([IO.Path]::GetFileName($clangFormat)))" -ForegroundColor Cyan
        $cppPaths = @($targets.CppFormatPaths)
        if ($cppPaths.Count -eq 0) {
            Write-Host "  no C++ files selected" -ForegroundColor DarkGray
        } else {
            if ($CheckOnly) {
                & $clangFormat --dry-run --Werror @cppPaths
                if ($LASTEXITCODE -ne 0) { $fails += "clang-format" }
            } else {
                & $clangFormat -i @cppPaths
                if ($LASTEXITCODE -ne 0) { $fails += "clang-format" }
            }
        }
    }

    if (-not $SkipTidy) {
        $clangTidy = Find-LlvmTool "clang-tidy"
        if (-not $clangTidy) {
            Write-Host "clang-tidy not found; skipping." -ForegroundColor Yellow
        } else {
            $tidyPaths = @($targets.CppTidyPaths)
            $tidyJobCount = Resolve-TidyJobCount -RequestedJobs $TidyJobs -FileCount $tidyPaths.Count
            Write-Host "-> clang-tidy ($tidyJobCount job(s), $($tidyPaths.Count) file(s))" -ForegroundColor Cyan
            # Inline include flags -- mirrors target_include_directories() in
            # CMakeLists.txt. Saves having to feed clang-tidy compile_commands.json
            # (which MSVC's CMake generator doesn't produce anyway).
            $cxxArgs = @(
                "-std=c++20", "-x", "c++",
                "-D_WIN32_WINNT=0x0A00", "-DWIN32_LEAN_AND_MEAN", "-DNOMINMAX",
                "-D_CRT_SECURE_NO_WARNINGS", "-DUNICODE", "-D_UNICODE",
                "-I$(Join-Path $root 'include')",
                "-I$(Join-Path $root 'third_party\cpp-httplib')",
                "-I$(Join-Path $root 'third_party\nlohmann')",
                "-I$(Join-Path $root 'third_party\toml11')",
                "-I$(Join-Path $root 'third_party\miniaudio')"
            )
            if ($tidyPaths.Count -eq 0) {
                Write-Host "  no C++ source files selected" -ForegroundColor DarkGray
            } else {
                # PS 5.1 turns clang-tidy's stderr into error records under
                # $ErrorActionPreference="Stop", so the worker relaxes it.
                $worker = {
                    param($Tool, $File, [string[]]$ClangArgs)
                    $ErrorActionPreference = "Continue"
                    $out = & $Tool --quiet $File -- @ClangArgs 2>&1 | Out-String
                    [pscustomobject]@{
                        File     = $File
                        Output   = $out
                        ExitCode = $LASTEXITCODE
                    }
                }

                $results = @()
                if ($tidyJobCount -le 1) {
                    foreach ($path in $tidyPaths) {
                        $results += & $worker $clangTidy $path $cxxArgs
                    }
                } else {
                    $jobs = @()
                    foreach ($path in $tidyPaths) {
                        while ($jobs.Count -ge $tidyJobCount) {
                            $done = Wait-Job -Job $jobs -Any
                            $results += Receive-Job -Job $done
                            Remove-Job -Job $done
                            $jobs = @($jobs | Where-Object { $_.Id -ne $done.Id })
                        }
                        $jobs += Start-Job -ScriptBlock $worker -ArgumentList $clangTidy, $path, $cxxArgs
                    }
                    while ($jobs.Count -gt 0) {
                        $done = Wait-Job -Job $jobs -Any
                        $results += Receive-Job -Job $done
                        Remove-Job -Job $done
                        $jobs = @($jobs | Where-Object { $_.Id -ne $done.Id })
                    }
                }

                foreach ($result in ($results | Sort-Object File)) {
                    $diag = ($result.Output -split "`n") |
                        Where-Object { $_ -match ":\d+:\d+: (warning|error):" }
                    if ($diag -or $result.ExitCode -ne 0) {
                        Write-Host "  $([IO.Path]::GetFileName($result.File)): $($diag.Count) diagnostic(s)" -ForegroundColor Yellow
                        $diag | ForEach-Object { Write-Host "    $_" }
                        if (-not $diag -and $result.Output) { Write-Host $result.Output }
                        $fails += "clang-tidy ($([IO.Path]::GetFileName($result.File)))"
                    }
                }
            }
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
            $webPaths = @($targets.WebPaths)
            if ($ChangedOnly) {
                if ($webPaths.Count -eq 0) {
                    Write-Host "-> web check skipped (no dashboard files selected)" -ForegroundColor DarkGray
                } else {
                    $prettier = Join-Path $root "node_modules\.bin\prettier.cmd"
                    $eslint = Join-Path $root "node_modules\.bin\eslint.cmd"
                    $stylelint = Join-Path $root "node_modules\.bin\stylelint.cmd"
                    $jsPaths = @($webPaths | Where-Object { [IO.Path]::GetExtension($_) -ieq ".js" })
                    $cssPaths = @($webPaths | Where-Object { [IO.Path]::GetExtension($_) -ieq ".css" })

                    if ($CheckOnly) {
                        Write-Host "-> prettier changed dashboard files" -ForegroundColor Cyan
                        & $prettier --check @webPaths
                        if ($LASTEXITCODE -ne 0) { $fails += "prettier" }
                        if ($jsPaths.Count -gt 0) {
                            Write-Host "-> eslint changed JS files" -ForegroundColor Cyan
                            & $eslint @jsPaths
                            if ($LASTEXITCODE -ne 0) { $fails += "eslint" }
                        }
                        if ($cssPaths.Count -gt 0) {
                            Write-Host "-> stylelint changed CSS files" -ForegroundColor Cyan
                            & $stylelint @cssPaths
                            if ($LASTEXITCODE -ne 0) { $fails += "stylelint" }
                        }
                    } else {
                        Write-Host "-> prettier changed dashboard files" -ForegroundColor Cyan
                        & $prettier --write @webPaths
                        if ($LASTEXITCODE -ne 0) { $fails += "prettier" }
                        if ($jsPaths.Count -gt 0) {
                            Write-Host "-> eslint --fix changed JS files" -ForegroundColor Cyan
                            & $eslint --fix @jsPaths
                            if ($LASTEXITCODE -ne 0) { $fails += "eslint" }
                        }
                        if ($cssPaths.Count -gt 0) {
                            Write-Host "-> stylelint --fix changed CSS files" -ForegroundColor Cyan
                            & $stylelint --fix @cssPaths
                            if ($LASTEXITCODE -ne 0) { $fails += "stylelint" }
                        }
                    }
                }
            } elseif ($CheckOnly) {
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

$sidecarPaths = @($targets.SidecarPaths)
if (-not $SkipWeb -and $sidecarPaths.Count -gt 0) {
    $npm = Get-Command npm.cmd -ErrorAction SilentlyContinue
    if (-not $npm) { $npm = Get-Command npm -ErrorAction SilentlyContinue }
    if (-not $npm) {
        Write-Host "npm not found; skipping Roon sidecar check." -ForegroundColor Yellow
    } else {
        Write-Host "-> npm run check (Roon sidecar)" -ForegroundColor Cyan
        Push-Location (Join-Path $root "tools\roon-bridge")
        try {
            & $npm.Source run check
            if ($LASTEXITCODE -ne 0) { $fails += "roon sidecar check" }
        } finally { Pop-Location }
    }
}

if ($fails.Count -gt 0) {
    Write-Host ""
    Write-Host ("Failed: " + ($fails -join ", ")) -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "OK." -ForegroundColor Green
