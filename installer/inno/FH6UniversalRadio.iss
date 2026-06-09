#define MyAppName "FH6 Universal Radio"
#define MyAppVersion "0.1.0-dev"
#define PackageDir "..\..\package\windows"
#define VBCableZip PackageDir + "\vbcable\VBCABLE_Driver_Pack45.zip"

[Setup]
AppId={{7B4D0E89-5955-49E0-841E-6E4B3F6E1001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=FH6 Universal Radio
DefaultDirName={localappdata}\Programs\FH6 Universal Radio
DisableProgramGroupPage=yes
DirExistsWarning=no
UsePreviousAppDir=no
OutputDir=..\..\package
OutputBaseFilename=FH6UniversalRadioSetup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\fh6-radio-companion.exe
AppModifyPath={app}\FH6UniversalRadioSetup.exe
SetupIconFile=..\..\assets\app.ico
SetupLogging=yes
InfoAfterFile={#PackageDir}\INSTALL.txt

[Tasks]
Name: "startupcompanion"; Description: "Start FH6 Radio Companion when Windows starts"; GroupDescription: "Companion app:"
Name: "launchcompanion"; Description: "Start FH6 Radio Companion after install"; GroupDescription: "Companion app:"
Name: "installvbcable"; Description: "Launch VB-CABLE driver installer after setup (requires administrator approval and a reboot)"; GroupDescription: "Apple Music cable driver:"; Flags: unchecked; Check: ShouldOfferVBCableInstall

[Files]
Source: "{#PackageDir}\fh6-radio-companion.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\package-manifest.json"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\INSTALL.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\Install-VBCable.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#PackageDir}\vbcable\*"; DestDir: "{app}\vbcable"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{srcexe}"; DestDir: "{app}"; DestName: "FH6UniversalRadioSetup.exe"; Flags: external ignoreversion
Source: "{#PackageDir}\version.dll"; DestDir: "{code:GetGameDir}"; Flags: ignoreversion; BeforeInstall: BackupGameVersionDll
Source: "{#PackageDir}\fh6-radio\ui\*"; DestDir: "{code:GetGameDir}\fh6-radio\ui"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PackageDir}\media\*"; DestDir: "{code:GetGameDir}\media"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "{#PackageDir}\config.example.toml"; DestDir: "{code:GetGameDir}\fh6-radio"; DestName: "config.toml"; Flags: onlyifdoesntexist uninsneveruninstall
Source: "{#PackageDir}\README.md"; DestDir: "{code:GetGameDir}\fh6-radio"; Flags: ignoreversion
Source: "{#PackageDir}\windows-packaging.md"; DestDir: "{code:GetGameDir}\fh6-radio"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\FH6 Universal Radio\FH6 Radio Companion"; Filename: "{app}\fh6-radio-companion.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\FH6 Universal Radio\Repair or Update FH6 Universal Radio"; Filename: "{app}\FH6UniversalRadioSetup.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\FH6 Universal Radio\Install VB-CABLE Driver"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-ExecutionPolicy Bypass -NoProfile -File ""{app}\Install-VBCable.ps1"""; WorkingDir: "{app}"; Check: IsVBCablePackageInstalled
Name: "{autoprograms}\FH6 Universal Radio\Uninstall FH6 Universal Radio"; Filename: "{uninstallexe}"
Name: "{autoprograms}\FH6 Universal Radio\Radio Dashboard"; Filename: "http://localhost:8420"
Name: "{userstartup}\FH6 Radio Companion"; Filename: "{app}\fh6-radio-companion.exe"; WorkingDir: "{app}"; Tasks: startupcompanion

[Run]
Filename: "{app}\fh6-radio-companion.exe"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent; Tasks: launchcompanion
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-ExecutionPolicy Bypass -NoProfile -File ""{app}\Install-VBCable.ps1"""; WorkingDir: "{app}"; Flags: postinstall skipifsilent shellexec waituntilterminated; Verb: runas; Tasks: installvbcable; Check: IsVBCablePackageInstalled

[UninstallRun]
Filename: "{cmd}"; Parameters: "/c taskkill /IM fh6-radio-companion.exe /F"; Flags: runhidden; RunOnceId: "StopCompanion"

[Registry]
Root: HKCU; Subkey: "Software\FH6 Universal Radio"; ValueType: string; ValueName: "GameDir"; ValueData: "{code:GetGameDir}"; Flags: uninsdeletekeyifempty

[InstallDelete]
Type: files; Name: "{code:GetGameDir}\fh6-radio-companion.exe"
Type: files; Name: "{code:GetGameDir}\package-manifest.json"
Type: files; Name: "{code:GetGameDir}\INSTALL.txt"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"

[Code]
var
  GameDirPage: TInputDirWizardPage;
  DetectedGameDir: String;
  ExistingInstall: Boolean;

function IsFH6Running(): Boolean;
var
  Locator: Variant;
  Wmi: Variant;
  Processes: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Wmi := Locator.ConnectServer('.', 'root\CIMV2');
    Processes := Wmi.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = "forzahorizon6.exe"');
    Result := Processes.Count > 0;
  except
    Result := False;
  end;
end;

function IsVBCablePackageBundled(): Boolean;
begin
  #if FileExists(VBCableZip)
  Result := True;
  #else
  Result := False;
  #endif
end;

function IsVBCablePackageInstalled(): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\vbcable\VBCABLE_Driver_Pack45.zip'));
end;

function IsVBCableInstalled(): Boolean;
var
  Locator: Variant;
  Wmi: Variant;
  Devices: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Wmi := Locator.ConnectServer('.', 'root\CIMV2');
    Devices := Wmi.ExecQuery(
      'SELECT Name FROM Win32_PnPEntity WHERE ' +
      'Name LIKE ''%VB-Audio Virtual Cable%'' OR ' +
      'Name LIKE ''%CABLE Input%'' OR ' +
      'Name LIKE ''%CABLE Output%'''
    );
    Result := Devices.Count > 0;
  except
    Result := False;
  end;
end;

function ShouldOfferVBCableInstall(): Boolean;
begin
  Result := IsVBCablePackageBundled() and not IsVBCableInstalled();
end;

function IsGameDir(Path: String): Boolean;
begin
  Result := FileExists(AddBackslash(Path) + 'forzahorizon6.exe');
end;

function SavedGameDir(var Path: String): Boolean;
begin
  Result :=
    RegQueryStringValue(HKCU, 'Software\FH6 Universal Radio', 'GameDir', Path) and
    IsGameDir(Path);
end;

function TryGameDir(Path: String): Boolean;
begin
  Result := False;
  if (DetectedGameDir = '') and IsGameDir(Path) then begin
    DetectedGameDir := Path;
    Result := True;
  end;
end;

function SteamRootFromRegistry(var SteamRoot: String): Boolean;
begin
  Result :=
    RegQueryStringValue(HKCU, 'Software\Valve\Steam', 'SteamPath', SteamRoot) or
    RegQueryStringValue(HKLM, 'Software\WOW6432Node\Valve\Steam', 'InstallPath', SteamRoot) or
    RegQueryStringValue(HKLM, 'Software\Valve\Steam', 'InstallPath', SteamRoot);
end;

procedure TrySteamLibrary(Root: String);
begin
  if Root = '' then Exit;
  StringChangeEx(Root, '/', '\', True);
  TryGameDir(AddBackslash(Root) + 'steamapps\common\ForzaHorizon6');
  TryGameDir(AddBackslash(Root) + 'steamapps\common\Forza Horizon 6');
end;

procedure TrySteamLibraryFolders(SteamRoot: String);
var
  Lines: TArrayOfString;
  I: Integer;
  Line: String;
  Path: String;
  FirstQuote: Integer;
  SecondQuote: Integer;
  ThirdQuote: Integer;
begin
  TrySteamLibrary(SteamRoot);
  if not LoadStringsFromFile(AddBackslash(SteamRoot) + 'steamapps\libraryfolders.vdf', Lines) then
    Exit;

  for I := 0 to GetArrayLength(Lines) - 1 do begin
    Line := Trim(Lines[I]);
    if Pos('"path"', Line) = 0 then Continue;

    FirstQuote := Pos('"path"', Line);
    Delete(Line, 1, FirstQuote + 5);
    SecondQuote := Pos('"', Line);
    if SecondQuote = 0 then Continue;
    Delete(Line, 1, SecondQuote);
    ThirdQuote := Pos('"', Line);
    if ThirdQuote = 0 then Continue;
    Path := Copy(Line, 1, ThirdQuote - 1);
    StringChangeEx(Path, '\\', '\', True);
    TrySteamLibrary(Path);
    if DetectedGameDir <> '' then Exit;
  end;
end;

function DriveRoot(Index: Integer): String;
begin
  Result := Chr(Ord('A') + Index) + ':\';
end;

function FindGameDir(): String;
var
  SteamRoot: String;
  I: Integer;
  Root: String;
  Saved: String;
begin
  DetectedGameDir := '';

  if SavedGameDir(Saved) then
    TryGameDir(Saved);

  TryGameDir('E:\SteamLibrary\steamapps\common\ForzaHorizon6');
  TryGameDir('C:\Program Files (x86)\Steam\steamapps\common\ForzaHorizon6');
  TryGameDir('C:\XboxGames\Forza Horizon 6\Content');

  if (DetectedGameDir = '') and SteamRootFromRegistry(SteamRoot) then
    TrySteamLibraryFolders(SteamRoot);

  if DetectedGameDir = '' then begin
    for I := 2 to 25 do begin
      Root := DriveRoot(I);
      TryGameDir(Root + 'SteamLibrary\steamapps\common\ForzaHorizon6');
      TryGameDir(Root + 'SteamLibrary\steamapps\common\Forza Horizon 6');
      TryGameDir(Root + 'XboxGames\Forza Horizon 6\Content');
      if DetectedGameDir <> '' then Break;
    end;
  end;

  Result := DetectedGameDir;
end;

function GetGameDir(Param: String): String;
begin
  if ExistingInstall and (DetectedGameDir <> '') then
    Result := DetectedGameDir
  else if Assigned(GameDirPage) and (GameDirPage.Values[0] <> '') then
    Result := GameDirPage.Values[0]
  else
    Result := FindGameDir();
end;

function HasInstalledFiles(GameDir: String): Boolean;
begin
  Result :=
    (GameDir <> '') and
    FileExists(AddBackslash(GameDir) + 'version.dll') and
    FileExists(AddBackslash(GameDir) + 'fh6-radio\config.toml');
end;

function DetectExistingInstall(GameDir: String): Boolean;
begin
  Result := HasInstalledFiles(GameDir);
end;

function GameVersionDllPath(): String;
begin
  Result := AddBackslash(GetGameDir('')) + 'version.dll';
end;

function GameVersionDllBackupPath(): String;
begin
  Result := GameVersionDllPath() + '.bak';
end;

procedure BackupGameVersionDll();
var
  Dll: String;
  Bak: String;
  GameDir: String;
begin
  GameDir := GetGameDir('');
  if DetectExistingInstall(GameDir) then begin
    Log('Existing FH6 Universal Radio install detected; preserving existing version.dll backup state.');
    Exit;
  end;

  Dll := GameVersionDllPath();
  Bak := GameVersionDllBackupPath();
  if FileExists(Dll) and not FileExists(Bak) then begin
    if CopyFile(Dll, Bak, False) then
      Log('Backed up existing version.dll to version.dll.bak.')
    else
      Log('Failed to back up existing version.dll before install.');
  end;
end;

procedure RestoreGameVersionDll();
var
  Dll: String;
  Bak: String;
begin
  Dll := GameVersionDllPath();
  Bak := GameVersionDllBackupPath();
  if FileExists(Bak) then begin
    DeleteFile(Dll);
    if RenameFile(Bak, Dll) then
      Log('Restored version.dll from version.dll.bak.')
    else
      Log('Failed to restore version.dll from version.dll.bak.');
  end else if FileExists(Dll) then begin
    DeleteFile(Dll);
    Log('Removed installed version.dll.');
  end;
end;

procedure InitializeWizard();
var
  AutoGameDir: String;
begin
  AutoGameDir := FindGameDir();
  ExistingInstall := DetectExistingInstall(AutoGameDir);
  GameDirPage := CreateInputDirPage(
    wpSelectDir,
    'Select Forza Horizon 6 Folder',
    'Where is Forza Horizon 6 installed?',
    'Setup will install version.dll and the fh6-radio dashboard files into the folder that contains forzahorizon6.exe.',
    False,
    ''
  );
  GameDirPage.Add('');
  GameDirPage.Values[0] := AutoGameDir;
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := False;
  if ExistingInstall and (DetectedGameDir <> '') and
     ((Assigned(GameDirPage) and (PageID = GameDirPage.ID)) or
      (PageID = wpSelectDir) or
      (PageID = wpSelectTasks) or
      (PageID = wpInfoAfter)) then
    Result := True;
end;

function InitializeSetup(): Boolean;
var
  ErrorCode: Integer;
begin
  Result := True;
  if IsFH6Running() then begin
    MsgBox('Forza Horizon 6 is running. Close the game before installing or updating FH6 Universal Radio.', mbError, MB_OK);
    Result := False;
  end;

  Exec(ExpandConstant('{cmd}'), '/c taskkill /IM fh6-radio-companion.exe /F', '', SW_HIDE, ewWaitUntilTerminated, ErrorCode);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if Assigned(GameDirPage) and (CurPageID = GameDirPage.ID) then begin
    if not IsGameDir(GameDirPage.Values[0]) then begin
      MsgBox('Please select the Forza Horizon 6 install folder that contains forzahorizon6.exe.', mbError, MB_OK);
      Result := False;
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RestoreGameVersionDll();
end;
