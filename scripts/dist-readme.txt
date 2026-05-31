FH6 Universal Radio
===================

Thanks for grabbing this. It's a free, open-source mod that drops a
brand new station into Forza Horizon 6's radio dial. You feed it audio
from a folder of music files on your PC, from any YouTube / YouTube
Music link, from a Jellyfin server, or from any other Windows app
(Spotify, Deezer, a browser tab), and the game treats the result
like every other station: it ducks for menus, follows your in-game
volume slider, and fades on the loading screen.


Getting it running
~~~~~~~~~~~~~~~~~~

Make sure FH6 isn't open first. Then drop the contents of this archive
straight into the folder that contains forzahorizon6.exe. Depending on
where you installed the game, that'll look like one of:

    Steam      ->  ...\steamapps\common\ForzaHorizon6
    Xbox app   ->  ...\XboxGames\Forza Horizon 6\Content

Let Windows overwrite when it asks. Heads-up: some antivirus tools dunk
on the bundled version.dll because of how the mod hooks into the game.
If yours yeets the file, add the FH6 folder to its exclusions list
and re-extract.

Once the files are in place, launch the game and head into
Settings > Audio. Two switches matter:

    Streamer Mode  ->  ON     (the new station only shows up with
                                this enabled)
    Radio DJ       ->  OFF    (otherwise the in-game DJ chimes in
                                over your tracks)

Now cycle the radio stations in-game until you land on the new one.
The mod's audio only goes out while that station is the active one.
Flip to another station and it stops broadcasting.


Configuring it from your browser
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Everything else is driven from a small web dashboard the mod hosts
locally. With the game running, open this URL in any browser on the
same machine:

    http://localhost:8420

From another device on the same network, use your PC's local IP
instead (e.g. http://192.168.1.42:8420). Run `ipconfig` in a Command
Prompt to find it.

From there:

  * Local files: give it a folder full of MP3, FLAC, WAV, OGG, M4A,
    AAC, OPUS or WMA tracks. Subfolders are scanned too. Shuffle,
    skip, and volume controls are all in the UI. MP3 / FLAC / WAV /
    OGG play out of the box; the other formats route through ffmpeg
    (same binary as YouTube Music below).

  * YouTube Music: paste a video URL, a playlist, or a YT Music
    link. This path needs three external tools. Open a Command
    Prompt and run:

        winget install yt-dlp.yt-dlp
        winget install Gyan.FFmpeg
        winget install DenoLand.Deno

    Then restart the game. yt-dlp and ffmpeg can also be pointed
    at explicitly from Settings > YouTube Music (yt-dlp)
    and Settings > General > ffmpeg path in the dashboard
    if you prefer a manual install.

    For age-gated or private content, export your browser's cookies
    as a Netscape cookies.txt (use an extension like "Get cookies.txt
    LOCALLY") and load that from the same panel.

  * Jellyfin: stream playlists from your own Jellyfin server.
    Configure the server URL, API key, user ID, and playlist ID
    under Settings > Jellyfin. Jellyfin transcodes to PCM via
    ffmpeg, so the configured ffmpeg path must be valid.

  * External audio: capture any Windows playback device and pipe a
    live app (Spotify, Deezer, a browser tab) into the radio. The capture
    is a loopback of whatever the device plays, so route the app to a
    virtual audio cable (e.g. VB-Audio Virtual Cable, set to 48000 Hz)
    and pick that cable as the capture device, otherwise you hear the
    app directly instead of through the radio. Track info and
    next/previous come from the selected Windows media session, and the
    app pauses and resumes together with the game radio.

A handful of in-game extras live under Settings in the dashboard:

  * Race start action: on race begin, advance to next track, restart
    the current one, or leave it alone.
  * Quick station skip: tune the radio knob away and back within 1s
    to skip the current track.
  * Loudness normalization for consistent volume across tracks.
  * 5-band equalizer (60 Hz / 250 Hz / 1 kHz / 4 kHz / 12 kHz peaking
    biquads, +/-6 dB per band, applied producer-side at 48 kHz before
    audio hits the game).

Edits save the moment you change them, no need to bounce the game.


Pulling it back out
~~~~~~~~~~~~~~~~~~~

Two things to remove from the FH6 install folder: version.dll, and the
fh6-radio/ folder sitting next to it. After that, hit "Verify integrity
of game files" (Steam) or "Repair" (Xbox app / MS Store) and the
patched game assets get pulled back to vanilla.


About the project
~~~~~~~~~~~~~~~~~

This mod is a hobby project released under GPLv3. The source lives at
github.com/g0ldyy/fh6-universal-radio. Bug reports, feature ideas, and
PRs are all welcome over there. If you want to chip in financially,
the README on the repo has GitHub Sponsors and Ko-fi links.

Unofficial fan project. Nothing here is affiliated with, endorsed by,
or connected to Turn 10 Studios, Playground Games, Xbox Game Studios,
Microsoft, Google, YouTube, or Jellyfin (Jellyfin LLC). Forza Horizon,
Forza Motorsport, and all other names dropped above belong to their
respective owners. Provided as-is, no warranty, use at your own risk.
