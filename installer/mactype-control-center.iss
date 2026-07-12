#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef AppExe
  #error AppExe must point to mactype-control-center.exe
#endif
#ifndef PreviewExe
  #error PreviewExe must point to mactype-preview32.exe
#endif
#ifndef SourceRoot
  #define SourceRoot ".."
#endif
#ifndef OutputRoot
  #define OutputRoot "..\artifacts\installer"
#endif

[Setup]
AppId={{AF6B9697-3DF2-46C4-B203-79194967AE7A}
AppName=MacType Control Center
AppVersion={#AppVersion}
AppPublisher=MacType contributors
DefaultDirName={localappdata}\Programs\MacType Control Center
DefaultGroupName=MacType Control Center
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputRoot}
OutputBaseFilename=MacType-Control-Center-{#AppVersion}-setup
SetupIconFile={#SourceRoot}\assets\mactype.ico
UninstallDisplayIcon={app}\mactype-control-center.exe
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
CloseApplications=yes
RestartApplications=no
ChangesAssociations=no

[Files]
Source: "{#AppExe}"; DestDir: "{app}"; DestName: "mactype-control-center.exe"; Flags: ignoreversion
Source: "{#PreviewExe}"; DestDir: "{app}"; DestName: "mactype-preview32.exe"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\MacType Control Center"; Filename: "{app}\mactype-control-center.exe"; WorkingDir: "{app}"

[Run]
Filename: "{app}\mactype-control-center.exe"; Description: "MacType Control Center 실행"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\MacType\ControlCenter\cache"
