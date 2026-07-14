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
DefaultDirName={localappdata}\Programs\MacType Control Center
DefaultGroupName=MacType Control Center
PrivilegesRequired=lowest
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

[Files]
Source: "{#AppExe}"; DestDir: "{app}"; DestName: "{#ControlCenterExeName}"; Flags: ignoreversion
Source: "{#PreviewExe}"; DestDir: "{app}"; DestName: "mactype-preview32.exe"; Flags: ignoreversion
Source: "{#CoreRoot}\MacType.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CoreRoot}\MacType64.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CoreRoot}\MacType.Core.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CoreRoot}\MacType64.Core.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CoreRoot}\MacLoader.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#CoreRoot}\MacLoader64.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\distribution\MacType.ini"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\distribution\ini\*.ini"; DestDir: "{app}\ini"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceRoot}\distribution\languages\*.json"; DestDir: "{app}\languages"; Flags: ignoreversion
Source: "{#SourceRoot}\distribution\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceRoot}\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\MacType Control Center"; Filename: "{app}\{#ControlCenterExeName}"; WorkingDir: "{app}"

[Run]
Filename: "{app}\{#ControlCenterExeName}"; Description: "MacType Control Center 실행"; Flags: nowait postinstall skipifsilent

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "MacTypeControlCenter"; Flags: uninsdeletevalue dontcreatekey

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\MacType\ControlCenter\cache"
