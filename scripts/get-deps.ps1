# Downloads the three single-header dependencies into third_party/.
# Run this once after cloning the project, before configuring CMake.
#
#   PS> .\scripts\get-deps.ps1
#
# Re-runs are safe -- existing files are overwritten.

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tp   = Join-Path $root "third_party"

$deps = @(
    # original headers
    @{ Url = "https://raw.githubusercontent.com/nlohmann/json/develop/single_include/nlohmann/json.hpp"; Out = "nlohmann\nlohmann\json.hpp"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/ToruNiina/toml11/main/single_include/toml.hpp"; Out = "toml11\toml.hpp"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h"; Out = "miniaudio\miniaudio.h"; Base = $tp },
    
    # STB & Kiero
    @{ Url = "https://raw.githubusercontent.com/nothings/stb/master/stb_image.h"; Out = "stb\stb_image.h"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h"; Out = "stb\stb_image_write.h"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/nothings/stb/master/stb_image_resize2.h"; Out = "stb\stb_image_resize2.h"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/Rebzzel/kiero/master/kiero.h"; Out = "kiero\kiero.h"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/Rebzzel/kiero/master/kiero.cpp"; Out = "kiero\kiero.cpp"; Base = $tp },
    
    # MinHook
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/include/MinHook.h"; Out = "minhook\include\MinHook.h"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/buffer.c"; Out = "minhook\src\buffer.c"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/buffer.h"; Out = "minhook\src\buffer.h"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hook.c"; Out = "minhook\src\hook.c"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/trampoline.c"; Out = "minhook\src\trampoline.c"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/trampoline.h"; Out = "minhook\src\trampoline.h"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/hde32.c"; Out = "minhook\src\hde\hde32.c"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/hde32.h"; Out = "minhook\src\hde\hde32.h"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/hde64.c"; Out = "minhook\src\hde\hde64.c"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/hde64.h"; Out = "minhook\src\hde\hde64.h"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/pstdint.h"; Out = "minhook\src\hde\pstdint.h"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/table32.h"; Out = "minhook\src\hde\table32.h"; Base = $tp },
    @{ Url = "https://raw.githubusercontent.com/TsudaKageyu/minhook/master/src/hde/table64.h"; Out = "minhook\src\hde\table64.h"; Base = $tp }
)

foreach ($d in $deps) {
    $target = Join-Path $d.Base $d.Out
    $dir    = Split-Path -Parent $target
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    Write-Host "-> $($d.Out)" -ForegroundColor Cyan
    Invoke-WebRequest -UseBasicParsing -Uri $d.Url -OutFile $target
}

Write-Host "Applying required patches to Kiero..." -ForegroundColor Yellow

# patch kiero.h to enable D3D12 and MinHook
$kieroH = Join-Path $tp "kiero\kiero.h"
$kieroHContent = Get-Content $kieroH -Raw
$beforeH = $kieroHContent
$kieroHContent = $kieroHContent -replace 'KIERO_INCLUDE_D3D12\s+0', 'KIERO_INCLUDE_D3D12  1'
$kieroHContent = $kieroHContent -replace 'KIERO_USE_MINHOOK\s+0', 'KIERO_USE_MINHOOK    1'
if ($kieroHContent -eq $beforeH -or $kieroHContent -notmatch 'KIERO_INCLUDE_D3D12\s+1' -or $kieroHContent -notmatch 'KIERO_USE_MINHOOK\s+1') {
   throw "Kiero header patch failed; upstream file layout changed."
}
Set-Content -Path $kieroH -Value $kieroHContent -NoNewline

$kieroCpp = Join-Path $tp "kiero\kiero.cpp"
$kieroCppContent = Get-Content $kieroCpp -Raw
$beforeCpp = $kieroCppContent
$kieroCppContent = $kieroCppContent -replace '# include "minhook/include/MinHook.h"', '# include "MinHook.h"'
$kieroCppContent = $kieroCppContent -replace '::GetProcAddress', '(void*)::GetProcAddress'
$kieroCppContent = "#include <stdlib.h>`r`n" + $kieroCppContent

if ($kieroCppContent -eq $beforeCpp -or 
    $kieroCppContent -notmatch '# include "MinHook.h"' -or 
    $kieroCppContent -notmatch '\(void\*\)::GetProcAddress') {
   throw "Kiero cpp patch failed; upstream file layout changed."
}
Set-Content -Path $kieroCpp -Value $kieroCppContent -NoNewline

Write-Host "`nAll dependencies fetched into $tp." -ForegroundColor Green