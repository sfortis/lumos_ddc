; Inno Setup script for the Lumos installer (lumos-setup.exe).
;
; Lumos is installed for the current user only, in %LOCALAPPDATA%\Programs\Lumos,
; so Setup needs no administrator rights. Settings stay in %APPDATA%\Lumos and
; are kept on uninstall.
;
; CI builds it with:
;   ISCC.exe /DAppVersion=1.4.1 /DBuildDir=<folder with lumos.exe and lumosctl.exe> installer\lumos.iss
; and the setup is written to BuildDir.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\build"
#endif

[Setup]
; The AppId identifies the installation in Apps & Features. Never change it,
; or an upgrade installs a second copy instead of replacing the first.
AppId={{331F27C6-1946-4552-A66E-2F7085A8CC36}
AppName=Lumos
AppVersion={#AppVersion}
AppVerName=Lumos {#AppVersion}
AppPublisher=sfortis
AppPublisherURL=https://github.com/sfortis/lumos_ddc
AppSupportURL=https://github.com/sfortis/lumos_ddc/issues
AppUpdatesURL=https://github.com/sfortis/lumos_ddc/releases
PrivilegesRequired=lowest
DefaultDirName={autopf}\Lumos
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
SetupIconFile=..\res\lumos.ico
UninstallDisplayIcon={app}\lumos.exe
UninstallDisplayName=Lumos
OutputDir={#BuildDir}
OutputBaseFilename=lumos-setup
Compression=lzma2
SolidCompression=yes
; Lumos is closed by the code below, through its own takeover message.
CloseApplications=no
; Setup and the uninstaller broadcast WM_SETTINGCHANGE after a PATH change.
ChangesEnvironment=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "autostart"; Description: "Start Lumos when I sign in to Windows"
Name: "addtopath"; Description: "Add lumosctl to the PATH (command line control)"

[Files]
Source: "{#BuildDir}\lumos.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\lumosctl.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Lumos"; Filename: "{app}\lumos.exe"

[Registry]
; The same value that "Start with Windows" in the Lumos settings writes.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Lumos"; ValueData: """{app}\lumos.exe"""; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\lumos.exe"; Description: "Start Lumos"; Flags: nowait postinstall skipifsilent

[Code]
const
  LumosMutex = 'Lumos_SingleInstance';
  EnvKey = 'Environment';

var
  WasRunning: Boolean;

function RegisterWindowMessage(lpString: String): Integer;
  external 'RegisterWindowMessageW@user32.dll stdcall';

{ Asks a running Lumos to quit with the message a newly started Lumos sends
  to the old one, then waits up to 5 seconds for its mutex to disappear.
  Returns False when Lumos is still running. }
function CloseLumos(): Boolean;
var
  I: Integer;
begin
  Result := True;
  if not CheckForMutexes(LumosMutex) then
    Exit;
  PostBroadcastMessage(RegisterWindowMessage('Lumos_TakeoverQuit'), 0, 0);
  for I := 1 to 50 do begin
    Sleep(100);
    if not CheckForMutexes(LumosMutex) then
      Exit;
  end;
  Result := False;
end;

{ Returns the position of Dir in the user PATH, framed by semicolons, or 0. }
function PathPos(Path, Dir: String): Integer;
begin
  Result := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Path) + ';');
end;

procedure AddToPath(Dir: String);
var
  Path: String;
begin
  if not RegQueryStringValue(HKCU, EnvKey, 'Path', Path) then
    Path := '';
  if PathPos(Path, Dir) > 0 then
    Exit;
  if (Path <> '') and (Path[Length(Path)] <> ';') then
    Path := Path + ';';
  RegWriteExpandStringValue(HKCU, EnvKey, 'Path', Path + Dir);
end;

procedure RemoveFromPath(Dir: String);
var
  Path, Framed: String;
  P: Integer;
begin
  if not RegQueryStringValue(HKCU, EnvKey, 'Path', Path) then
    Exit;
  P := PathPos(Path, Dir);
  if P = 0 then
    Exit;
  { Remove ";Dir" from ";Path;", then drop the two framing semicolons. }
  Framed := ';' + Path + ';';
  Delete(Framed, P, Length(Dir) + 1);
  RegWriteExpandStringValue(HKCU, EnvKey, 'Path', Copy(Framed, 2, Length(Framed) - 2));
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  WasRunning := CheckForMutexes(LumosMutex);
  if not CloseLumos() then
    Result := 'Lumos is still running. Exit it from its tray menu and run Setup again.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep <> ssPostInstall then
    Exit;
  if WizardIsTaskSelected('addtopath') then
    AddToPath(ExpandConstant('{app}'));
  { An interactive install offers "Start Lumos" on the last page. A silent
    install (winget upgrade) starts Lumos again only if it was running. }
  if WizardSilent() and WasRunning then
    ExecAsOriginalUser(ExpandConstant('{app}\lumos.exe'), '', '', SW_SHOWNORMAL, ewNoWait, ResultCode);
end;

function InitializeUninstall(): Boolean;
begin
  Result := CloseLumos();
  if not Result then
    SuppressibleMsgBox('Lumos is still running. Exit it from its tray menu and run the uninstaller again.',
      mbError, MB_OK, IDOK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromPath(ExpandConstant('{app}'));
end;
