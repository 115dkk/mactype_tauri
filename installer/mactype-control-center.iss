#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef AppExe
  #error AppExe must point to MacType Control Center.exe
#endif
#ifndef PreviewExe
  #error PreviewExe must point to mactype-preview32.exe
#endif
#ifndef CoreRoot
  #error CoreRoot must point to the source-built core artifact directory
#endif
#ifndef ServiceRuntimeRoot
  #error ServiceRuntimeRoot must point to the fixed open-service runtime artifact directory
#endif
#ifndef SourceRoot
  #define SourceRoot ".."
#endif
#ifndef OutputRoot
  #define OutputRoot "..\artifacts\installer"
#endif
#define ControlCenterExeName "MacType Control Center.exe"

[Setup]
AppId={{AF6B9697-3DF2-46C4-B203-79194967AE7A}
AppName=MacType Control Center
AppVersion={#AppVersion}
AppPublisher=MacType contributors
AppPublisherURL=https://github.com/snowie2000/mactype
DefaultDirName={autopf}\MacType Control Center
DefaultGroupName=MacType Control Center
PrivilegesRequired=admin
UsePreviousAppDir=no
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputRoot}
OutputBaseFilename=MacType Control Center
SetupIconFile={#SourceRoot}\assets\mactype.ico
LicenseFile={#SourceRoot}\LICENSE
UninstallDisplayIcon={app}\{#ControlCenterExeName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
CloseApplications=yes
RestartApplications=no
ChangesAssociations=no
VersionInfoDescription=Open MacType Control Center and source-built core

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: checkedonce

[InstallDelete]
Type: filesandordirs; Name: "{app}\service-runtime"

[Files]
Source: "{#AppExe}"; DestDir: "{app}"; DestName: "{#ControlCenterExeName}"; Flags: ignoreversion
Source: "{#PreviewExe}"; DestDir: "{app}"; DestName: "mactype-preview32.exe"; Flags: ignoreversion
Source: "{#CoreRoot}\MacType.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CoreRoot}\MacType64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CoreRoot}\MacType.Core.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CoreRoot}\MacType64.Core.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CoreRoot}\MacLoader.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CoreRoot}\MacLoader64.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#ServiceRuntimeRoot}\mactype-service-setup.exe"; DestDir: "{app}\service-runtime"; Flags: ignoreversion
Source: "{#ServiceRuntimeRoot}\payload\manifest.json"; DestDir: "{app}\service-runtime\payload"; Flags: ignoreversion
Source: "{#ServiceRuntimeRoot}\payload\files\mactype-service.exe"; DestDir: "{app}\service-runtime\payload\files"; Flags: ignoreversion
Source: "{#ServiceRuntimeRoot}\payload\files\mactype-injector32.exe"; DestDir: "{app}\service-runtime\payload\files"; Flags: ignoreversion
Source: "{#ServiceRuntimeRoot}\payload\files\mactype-injector64.exe"; DestDir: "{app}\service-runtime\payload\files"; Flags: ignoreversion
Source: "{#ServiceRuntimeRoot}\payload\files\MacType.dll"; DestDir: "{app}\service-runtime\payload\files"; Flags: ignoreversion
Source: "{#ServiceRuntimeRoot}\payload\files\MacType64.dll"; DestDir: "{app}\service-runtime\payload\files"; Flags: ignoreversion
Source: "{#SourceRoot}\distribution\MacType.ini"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\distribution\ini\*.ini"; DestDir: "{app}\ini"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceRoot}\distribution\languages\*.json"; DestDir: "{app}\languages"; Flags: ignoreversion
Source: "{#SourceRoot}\distribution\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\MacType Control Center"; Filename: "{app}\{#ControlCenterExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\MacType Control Center"; Filename: "{app}\{#ControlCenterExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#ControlCenterExeName}"; Description: "{cm:LaunchProgram,MacType Control Center}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
const
  FixedApplicationDirectory = '{autopf}\MacType Control Center';
  SetupBrokerRelativePath = 'service-runtime\mactype-service-setup.exe';

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  if CompareText(ExpandConstant('{app}'), ExpandConstant(FixedApplicationDirectory)) <> 0 then
    Result := 'MacType Control Center must be installed in its protected Program Files directory.'
  else
    Result := '';
end;

procedure RunFixedBrokerOrFail(const Verb: String; const Operation: String);
var
  ResultCode: Integer;
  Broker: String;
begin
  Broker := AddBackslash(ExpandConstant('{app}')) + SetupBrokerRelativePath;
  if not Exec(Broker, Verb, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    RaiseException(Operation + ' could not start the protected setup broker.');
  if ResultCode <> 0 then
    RaiseException(Operation + ' failed with setup broker exit code ' + IntToStr(ResultCode) + '.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    RunFixedBrokerOrFail('bootstrap-install', 'Machine service bootstrap');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RunFixedBrokerOrFail('uninstall-owned', 'Owned machine service removal');
end;
