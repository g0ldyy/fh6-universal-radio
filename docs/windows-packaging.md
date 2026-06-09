# Windows packaging plan

This is the working plan for making Apple Music + VB-CABLE easy to install,
test, and remove.

## Goals

- Keep the FH6 DLL focused on the in-game radio path.
- Use VB-CABLE as the supported Apple Music transport:
  `Apple Music -> CABLE Input -> CABLE Output -> FH6 radio`.
- Add a small companion app for behavior the DLL cannot provide when FH6 is
  closed:
  - detect whether FH6 is running;
  - monitor `CABLE Output` to the default Windows output while FH6 is not
    running;
  - stop monitoring when FH6 is running, leaving the cable to the game;
  - optionally expose a tray icon for status and quick enable/disable.

## Installer flow

1. Detect the FH6 install directory or let the user pick it.
2. Install/update `version.dll` and `fh6-radio` files.
3. Detect VB-CABLE by looking for capture devices containing `CABLE Output`.
4. If VB-CABLE is missing and the driver zip was bundled, show an explicit
   consent task with VB-Audio attribution. Do not silently install the driver.
5. Write Apple Music defaults:
   - `capture_mode = "auto"`
   - `capture_device = "CABLE Output"`
   - `mute_external_output = false`
   - `monitor_when_radio_inactive = false`
6. Install the companion app only if the user enables the Apple Music cable
   workflow.

The current package includes a self-contained PowerShell installer:

```powershell
.\Install-FH6UniversalRadio.ps1 -GameDir "E:\SteamLibrary\steamapps\common\ForzaHorizon6"
```

If `vbcable\VBCABLE_Driver_Pack45.zip` is present in the package, the
PowerShell installer can also launch the VB-CABLE setup:

```powershell
.\Install-FH6UniversalRadio.ps1 -GameDir "E:\SteamLibrary\steamapps\common\ForzaHorizon6" -InstallVBCable
```

Re-running the installer is the update path for now. It replaces the DLL,
dashboard, and companion executable while preserving the existing game config.
The companion is installed under `%LOCALAPPDATA%\FH6 Universal Radio` and a
Startup shortcut is created unless `-NoStartupShortcut` is passed.

Each staged package includes `package-manifest.json`, copied into the companion
install directory. That gives a future updater a stable local state file to
compare against a release manifest.

## EXE installer

The repo also includes an Inno Setup definition for a normal Windows installer:

```powershell
.\scripts\build-installer.ps1
```

This first stages `package\windows`, then compiles
`package\FH6UniversalRadioSetup.exe`. The installer lets the user select the
FH6 install folder, copies `version.dll` and dashboard files into the game,
preserves an existing config, installs the companion under
`%LOCALAPPDATA%\FH6 Universal Radio`, and creates optional Startup/launch tasks.
When the VB-CABLE zip is bundled and the cable is not already installed, the
installer offers to launch VB-CABLE setup after FH6 Universal Radio finishes.
That launch requires administrator approval because VB-CABLE is an audio
driver.

Build requirement: Inno Setup 6 (`winget install JRSoftware.InnoSetup`).

To include VB-CABLE in the package, fetch the official VB-Audio zip before
building:

```powershell
.\scripts\fetch-vbcable.ps1
.\scripts\build-installer.ps1
```

## Uninstall flow

1. Remove/restore `version.dll`.
2. Optionally remove `fh6-radio` config/log/UI files.
3. Stop and remove the companion app.
4. Leave VB-CABLE installed by default. Driver removal must be an explicit
   admin action using VB-CABLE's own setup/uninstall flow.

The package uninstall script removes the DLL, companion app, and Startup
shortcut. It preserves `fh6-radio` config by default and only removes it when
called with `-RemoveConfig`.

## Companion app behavior

The companion should be a normal user-mode Windows app, not injected into FH6.

- Input: WASAPI capture from the configured cable capture endpoint.
- Output: WASAPI render to the current default Windows output.
- State:
  - FH6 not running: monitor cable to default output.
  - FH6 running: stop monitoring and let the DLL consume the cable.
- Future enhancement: consume a status heartbeat from the DLL/dashboard so it
  can monitor while FH6 is running but the custom radio is unavailable.

The current first-pass companion is `fh6-radio-companion.exe`. It accepts an
optional capture-device search string; without arguments it looks for
`CABLE Output`.

## Apple Music routing instructions

After VB-CABLE is installed, Windows app-volume routing still has to point Apple
Music at the cable:

1. Open Apple Music and start playback once so Windows lists it in the mixer.
2. Open Windows Settings > System > Sound > Volume mixer.
3. In the Apps list, set Apple Music's Output device to
   `CABLE Input (VB-Audio Virtual Cable)`.
4. Leave Apple Music's Input device as Default.
5. Start `fh6-radio-companion.exe`.
6. With FH6 closed, the companion monitors `CABLE Output` back to the default
   Windows output so Apple Music remains audible.
7. With FH6 running, the companion releases the cable so the FH6 DLL can capture
   `CABLE Output` and feed the in-game radio.

If the cable devices are missing, rerun the VB-CABLE setup as administrator and
reboot Windows.

## Driver redistribution note

VB-CABLE is third-party donationware from VB-Audio (`https://vb-audio.com/`).
Their licensing page allows bundling the standard VB-CABLE package when the
donationware model remains visible and applicable. Keep attribution and user
consent visible in the installer UI, and do not bundle the paid additional
VB-CABLE A+B or C+D packages.
