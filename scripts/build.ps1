# One-shot build script. Configures CMake (Release, x64), compiles, then
# stages everything that needs to ship in dist\.
#
#   PS> .\scripts\build.ps1
#
# Output:
#   dist\version.dll            the proxy DLL (drops next to forzahorizon6.exe)
#   dist\fh6-radio\ui\          dashboard (mounted at http://localhost:<port>)
#   dist\fh6-radio\tools\       optional source sidecars
#   dist\fh6-radio\config.toml  seeded from config.example.toml

$ErrorActionPreference = "Stop"
$root  = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$dist  = Join-Path $root "dist"

. (Join-Path $PSScriptRoot "build-tools.ps1")
. (Join-Path $PSScriptRoot "package-tools.ps1")

$cmakeInfo = Find-CMakeForBuild
$cmake = $cmakeInfo.CMakePath
Write-Host "Using cmake: $cmake" -ForegroundColor DarkGray
Write-Host "Using generator: $($cmakeInfo.Generator)" -ForegroundColor DarkGray

if (-not (Test-Path (Join-Path $root "third_party\cpp-httplib\httplib.h"))) {
    Write-Host "third_party/ is empty -- running get-deps.ps1 first." -ForegroundColor Yellow
    & (Join-Path $PSScriptRoot "get-deps.ps1")
}

Write-Host "-> cmake configure" -ForegroundColor Cyan
$configureArgs = New-CMakeConfigureArguments `
    -SourceDir $root `
    -BuildDir $build `
    -Generator $cmakeInfo.Generator
& $cmake @configureArgs | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Host "-> cmake build (Release)" -ForegroundColor Cyan
& $cmake --build $build --config Release | Out-Host
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force -Path $dist | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $dist "fh6-radio") | Out-Null

Copy-Item (Join-Path $build "Release\version.dll") $dist
Copy-Item -Recurse (Join-Path $root "ui\dist") (Join-Path $dist "fh6-radio\ui")
Copy-Item (Join-Path $root "config.example.toml") (Join-Path $dist "fh6-radio\config.toml")

$roonBridgeSrc = Join-Path $root "tools\roon-bridge"
$roonBridgeDst = Join-Path $dist "fh6-radio\tools\roon-bridge"
if (Test-Path $roonBridgeSrc) {
    Write-Host "-> stage Roon sidecar" -ForegroundColor Cyan
    Copy-RoonBridgePackage -SourceDir $roonBridgeSrc -DestinationDir $roonBridgeDst
    Install-RoonBridgeRuntimeDependencies -BridgeDir $roonBridgeDst
}

$readme = @'
FH6 Universal Radio
-------------------

Thanks for grabbing this. It's a free, open-source mod that drops a
brand new station into Forza Horizon 6's radio dial. You feed it audio
either from a folder of music files on your PC, or from any YouTube /
YouTube Music link, and the game treats the result like every other
station -- it ducks for menus, follows your in-game volume slider, and
fades on the loading screen.


Getting it running
~~~~~~~~~~~~~~~~~~

Make sure FH6 isn't open first. Then drop the contents of this archive
straight into the folder that contains forzahorizon6.exe. Depending on
where you installed the game, that'll look like one of:

    Steam      ->  ...\steamapps\common\ForzaHorizon6
    Xbox app   ->  ...\XboxGames\Forza Horizon 6\Content

Let Windows overwrite when it asks. Heads-up: some antivirus tools dunk
on the bundled version.dll because of how the mod hooks into the game.
If yours yeets the file, add the FH6 folder to its exclusions
list and re-extract.

Once the files are in place, launch the game and head into
Settings -> Audio. Two switches matter:

    Streamer Mode  ->  ON     (the new station only shows up with
                                this enabled)
    Radio DJ       ->  OFF    (otherwise the in-game DJ chimes in
                                over your tracks)

Now cycle the radio stations in-game until you land on the new one.
The mod's audio only goes out while that station is the active one --
flip to another station and it stops broadcasting.


Configuring it from your browser
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Everything else is driven from a small web dashboard the mod hosts
locally. With the game running, open this URL in any browser on the
same machine or LAN:

    http://localhost:8420

From there:

  * Local files -- give it a folder full of MP3, FLAC, WAV, OGG, M4A
    or OPUS tracks. Subfolders are scanned too. Shuffle, skip, and
    volume controls are all in the UI.

  * YouTube Music -- paste a video URL, a playlist, or a YT Music
    link. This path needs three tools reachable on disk:

      - yt-dlp and ffmpeg: either on PATH, or pointed at explicitly
        from Settings > YouTube Music in the dashboard.
      - Deno: a JavaScript runtime that yt-dlp now leans on to solve
        YouTube's player-side signature challenges. Install it from
        https://deno.com/ (or `winget install DenoLand.Deno`).

    For age-gated or private content, export your browser's cookies
    as a Netscape cookies.txt and load that from the same panel.

Edits save the moment you change them -- no need to bounce the game.


Pulling it back out
~~~~~~~~~~~~~~~~~~~

Two things to remove from the FH6 install folder: version.dll, and the
fh6-radio/ folder sitting next to it. After that, hit "Verify integrity
of game files" (Steam) or "Repair" (Xbox app / MS Store) and the patched
game assets get pulled back to vanilla.


About the project
~~~~~~~~~~~~~~~~~

This mod is a hobby project released under GPLv3. The source lives at
github.com/g0ldyy/fh6-universal-radio -- bug reports, feature ideas,
and PRs are all welcome over there. If you want to chip in financially,
the README on the repo has GitHub Sponsors and Ko-fi links.

Unofficial fan project. Nothing here is affiliated with, endorsed by, or
connected to Turn 10 Studios, Playground Games, Xbox Game Studios,
Microsoft, Google, or YouTube. Forza Horizon, Forza Motorsport, and all
other names dropped above belong to their respective owners. Provided
as-is, no warranty, use at your own risk.
'@
Set-Content -Path (Join-Path $dist "README.txt") -Value $readme -Encoding utf8

Write-Host "`nBuilt + staged in $dist" -ForegroundColor Green
$stagedFiles = @(Get-ChildItem -Recurse -File $dist)
$runtimeFiles = @($stagedFiles | Where-Object { $_.FullName -match "\\node_modules\\" })
$stagedFiles |
    Where-Object { $_.FullName -notmatch "\\node_modules\\" } |
    ForEach-Object { "  $($_.FullName.Substring($dist.Length + 1))" }
if ($runtimeFiles.Count -gt 0) {
    "  fh6-radio\tools\roon-bridge\node_modules\  ($($runtimeFiles.Count) runtime dependency files)"
}
