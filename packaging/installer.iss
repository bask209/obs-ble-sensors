; Inno Setup script: builds obs-ble-sensors-<version>-windows-x64-setup.exe from the
; staged install tree (scripts\package.ps1 creates it with `cmake --install`).
;   ISCC.exe /DAppVersion=1.0.0 /DStageDir=..\release\Release packaging\installer.iss

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef StageDir
  #define StageDir "..\release\Release"
#endif

[Setup]
AppId={{7C1E2B9A-4F3D-4E8B-9A61-0B5E2D7C3A11}
AppName=OBS BLE Sensors
AppVersion={#AppVersion}
AppVerName=OBS BLE Sensors {#AppVersion}
AppPublisher=bask209
AppPublisherURL=https://github.com/bask209/obs-ble-sensors
AppSupportURL=https://github.com/bask209/obs-ble-sensors/issues
; Standard OBS plugin location (loaded by OBS 28+ for all users).
DefaultDirName={commonappdata}\obs-studio\plugins\obs-ble-sensors
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=admin
; /CURRENTUSER together with /DIR= is only meant for automated tests.
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.18362
OutputDir=..\release
OutputBaseFilename=obs-ble-sensors-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=OBS BLE Sensors
; Updating while OBS has the DLL loaded: offer to close OBS via Restart Manager.
CloseApplications=yes
RestartApplications=no

[Files]
Source: "{#StageDir}\obs-ble-sensors\bin\64bit\obs-ble-sensors.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion
Source: "{#StageDir}\obs-ble-sensors\bin\64bit\obs-ble-sensors.pdb"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StageDir}\obs-ble-sensors\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\NOTICE.md"; DestDir: "{app}"; Flags: ignoreversion

[Messages]
FinishedLabel=OBS BLE Sensors is installed.%n%nStart (or restart) OBS Studio, then add a source: Sources > + > BLE Heart Rate, and click "Scan / Manage Sensors...".

[Code]
// Lock the plugin folder down: only Administrators and SYSTEM may change the DLL that OBS
// loads; everyone else can only read and execute it. (By default ProgramData lets any
// user create files in subfolders.)
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if (CurStep = ssPostInstall) and IsAdminInstallMode() then
    Exec(ExpandConstant('{sys}\icacls.exe'),
      '"' + ExpandConstant('{app}') + '" /inheritance:r /grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F *S-1-5-32-545:(OI)(CI)RX',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function ObsInstalled(): Boolean;
begin
  Result := RegKeyExists(HKLM, 'SOFTWARE\OBS Studio') or RegKeyExists(HKLM64, 'SOFTWARE\OBS Studio')
    or FileExists(ExpandConstant('{commonpf64}\obs-studio\bin\64bit\obs64.exe'));
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
  if not ObsInstalled() and not WizardSilent() then
    Result := MsgBox('OBS Studio was not found on this PC. OBS BLE Sensors is a plugin for OBS Studio 31 or newer.'
      + #13#10#13#10 + 'Install the plugin anyway?', mbConfirmation, MB_YESNO) = IDYES;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ConfigDir: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    ConfigDir := ExpandConstant('{userappdata}\obs-studio\plugin_config\obs-ble-sensors');
    if DirExists(ConfigDir) and not UninstallSilent() then
      if MsgBox('Also delete your saved sensor list?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        DelTree(ConfigDir, True, True, True);
  end;
end;
