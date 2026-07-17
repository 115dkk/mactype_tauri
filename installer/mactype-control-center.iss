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

[UninstallDelete]
Type: dirifempty; Name: "{app}"

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
  SetupBrokerBackupRelativePath = 'service-runtime.setup-backup';
  MaximumBrokerDiagnosticCharacters = 4096;

var
  BrokerApplied: Boolean;
  BrokerAllowedBlocked: Boolean;
  BrokerOutputError: Boolean;
  BrokerDiagnostic: String;

procedure CaptureBrokerOutput(const S: String; const Error, FirstLine: Boolean);
var
  Remaining: Integer;
begin
  Log(S);
  if Error then
    BrokerOutputError := True;
  Remaining := MaximumBrokerDiagnosticCharacters - Length(BrokerDiagnostic);
  if Remaining > 0 then
  begin
    if BrokerDiagnostic <> '' then
    begin
      if Remaining >= 2 then
        BrokerDiagnostic := BrokerDiagnostic + #13#10
      else
        Remaining := 0;
    end;
    Remaining := MaximumBrokerDiagnosticCharacters - Length(BrokerDiagnostic);
    if Remaining > 0 then
      BrokerDiagnostic := BrokerDiagnostic + Copy(S, 1, Remaining);
  end;
  if (Pos('"ok":true', S) > 0) and (Pos('"outcome":"applied"', S) > 0) then
    BrokerApplied := True;
  if (Pos('"ok":true', S) > 0) and (Pos('"outcome":"skipped-blocked"', S) > 0) and
     ((Pos('"reason":"legacy-service"', S) > 0) or
      (Pos('"reason":"appinit"', S) > 0) or
      (Pos('"reason":"foreign-open-service"', S) > 0)) then
    BrokerAllowedBlocked := True;
end;

function BrokerFailure(const MessageText: String): String;
begin
  Result := MessageText;
  if BrokerDiagnostic <> '' then
    Result := Result + #13#10#13#10 + BrokerDiagnostic;
end;

procedure ResetBrokerCapture;
begin
  BrokerApplied := False;
  BrokerAllowedBlocked := False;
  BrokerOutputError := False;
  BrokerDiagnostic := '';
end;

procedure RunFixedBrokerOrFail(const Verb: String; const Operation: String);
var
  ResultCode: Integer;
  Broker: String;
begin
  ResetBrokerCapture;
  ResultCode := -1;
  Broker := AddBackslash(ExpandConstant('{app}')) + SetupBrokerRelativePath;
  if not ExecAndLogOutput(
    Broker,
    Verb,
    ExtractFileDir(Broker),
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode,
    @CaptureBrokerOutput
  ) then
    RaiseException(BrokerFailure(Operation + ' could not start the protected setup broker.'));
  if ResultCode <> 0 then
    RaiseException(BrokerFailure(
      Operation + ' failed with setup broker exit code ' + IntToStr(ResultCode) + '.'
    ));
end;

function RunStagedBootstrap(const Broker: String): String;
var
  ResultCode: Integer;
begin
  ResetBrokerCapture;
  ResultCode := -1;
  if not ExecAndLogOutput(
    Broker,
    'bootstrap-install',
    ExtractFileDir(Broker),
    SW_HIDE,
    ewWaitUntilTerminated,
    ResultCode,
    @CaptureBrokerOutput
  ) then
  begin
    Result := BrokerFailure('Machine service bootstrap could not start the protected setup broker.');
    Exit;
  end;
  if ResultCode <> 0 then
  begin
    Result := BrokerFailure(
      'Machine service bootstrap failed with setup broker exit code ' + IntToStr(ResultCode) + '.'
    );
    Exit;
  end;
  if BrokerOutputError then
  begin
    Result := BrokerFailure('Machine service bootstrap diagnostics could not be read safely.');
    Exit;
  end;
  if BrokerApplied then
  begin
    Result := '';
    Exit;
  end;
  if BrokerAllowedBlocked then
  begin
    Log('Machine service bootstrap was safely skipped because an explicit legacy integration conflict is present.');
    Result := '';
    Exit;
  end;
  Result := BrokerFailure('Machine service bootstrap returned no accepted terminal outcome.');
end;

function ExtractedBrokerFile(const RelativePath: String): String;
begin
  Result := AddBackslash(ExpandConstant('{tmp}')) +
    '{app}\service-runtime\' + RelativePath;
end;

procedure ExtractBrokerPayload;
var
  ExtractedCount: Integer;
begin
  ExtractedCount := ExtractTemporaryFiles('{app}\service-runtime\*');
  if ExtractedCount <> 7 then
    RaiseException(
      'expected 7 fixed broker payload files, extracted ' + IntToStr(ExtractedCount)
    );
end;

function CopyBrokerFile(const RuntimeRoot, RelativePath: String): Boolean;
begin
  Result := FileCopy(
    ExtractedBrokerFile(RelativePath),
    AddBackslash(RuntimeRoot) + RelativePath,
    False
  );
end;

function PopulateStagedBroker(const RuntimeRoot: String): String;
begin
  Result := '';
  try
    ExtractBrokerPayload;
    if not ForceDirectories(AddBackslash(RuntimeRoot) + 'payload\files') then
      RaiseException('could not create the staged payload directory');
    if not CopyBrokerFile(RuntimeRoot, 'mactype-service-setup.exe') then
      RaiseException('could not stage the setup broker');
    if not CopyBrokerFile(RuntimeRoot, 'payload\manifest.json') then
      RaiseException('could not stage the runtime manifest');
    if not CopyBrokerFile(RuntimeRoot, 'payload\files\mactype-service.exe') then
      RaiseException('could not stage the service host');
    if not CopyBrokerFile(RuntimeRoot, 'payload\files\mactype-injector32.exe') then
      RaiseException('could not stage the x86 injector');
    if not CopyBrokerFile(RuntimeRoot, 'payload\files\mactype-injector64.exe') then
      RaiseException('could not stage the x64 injector');
    if not CopyBrokerFile(RuntimeRoot, 'payload\files\MacType.dll') then
      RaiseException('could not stage the x86 MacType core');
    if not CopyBrokerFile(RuntimeRoot, 'payload\files\MacType64.dll') then
      RaiseException('could not stage the x64 MacType core');
  except
    Result := 'Machine service bootstrap staging failed: ' + GetExceptionMessage;
  end;
end;

function RestoreApplicationBroker(
  const ApplicationRoot, RuntimeRoot, BackupRoot: String;
  const ApplicationRootExisted, HadRuntime: Boolean
): String;
begin
  Result := '';
  if DirExists(RuntimeRoot) and not DelTree(RuntimeRoot, True, True, True) then
  begin
    Result := 'could not remove the temporary app-side service broker';
    Exit;
  end;
  if HadRuntime then
  begin
    if not RenameFile(BackupRoot, RuntimeRoot) then
      Result := 'could not restore the previous app-side service broker';
  end
  else if DirExists(BackupRoot) then
    Result := 'an unexpected app-side service broker backup remains';
  if (Result = '') and (not ApplicationRootExisted) then
    RemoveDir(ApplicationRoot);
end;

function BootstrapBeforeFileInstall: String;
var
  ApplicationRoot: String;
  RuntimeRoot: String;
  BackupRoot: String;
  Broker: String;
  ApplicationRootExisted: Boolean;
  HadRuntime: Boolean;
  OperationError: String;
  RestoreError: String;
begin
  ApplicationRoot := ExpandConstant('{app}');
  ApplicationRootExisted := DirExists(ApplicationRoot);
  RuntimeRoot := AddBackslash(ApplicationRoot) + 'service-runtime';
  BackupRoot := AddBackslash(ApplicationRoot) + SetupBrokerBackupRelativePath;
  if FileExists(RuntimeRoot) or FileExists(BackupRoot) then
  begin
    Result := 'Machine service bootstrap found a non-directory app-side broker path.';
    Exit;
  end;
  if DirExists(BackupRoot) then
  begin
    if DirExists(RuntimeRoot) then
    begin
      Result := 'Machine service bootstrap found an unresolved app-side broker backup collision.';
      Exit;
    end;
    if not RenameFile(BackupRoot, RuntimeRoot) then
    begin
      Result := 'Machine service bootstrap could not recover the previous app-side broker backup.';
      Exit;
    end;
  end;

  HadRuntime := DirExists(RuntimeRoot);
  if HadRuntime and not RenameFile(RuntimeRoot, BackupRoot) then
  begin
    Result := 'Machine service bootstrap could not preserve the previous app-side broker.';
    Exit;
  end;

  OperationError := '';
  try
    OperationError := PopulateStagedBroker(RuntimeRoot);
    if OperationError = '' then
    begin
      Broker := AddBackslash(RuntimeRoot) + 'mactype-service-setup.exe';
      OperationError := RunStagedBootstrap(Broker);
    end;
  except
    OperationError := 'Machine service bootstrap failed unexpectedly: ' + GetExceptionMessage;
  finally
    RestoreError := RestoreApplicationBroker(
      ApplicationRoot,
      RuntimeRoot,
      BackupRoot,
      ApplicationRootExisted,
      HadRuntime
    );
  end;

  if RestoreError <> '' then
    Result := OperationError + #13#10 +
      'App-side broker restoration failed: ' + RestoreError
  else
    Result := OperationError;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  { Required bootstrap must finish here using temporary app-side staging and restoration:
    AfterInstall exceptions may report success, and cancellation cannot restore overwritten upgrade files. }
  if CompareText(ExpandConstant('{app}'), ExpandConstant(FixedApplicationDirectory)) <> 0 then
  begin
    Result := 'MacType Control Center must be installed in its protected Program Files directory.';
    Exit;
  end;
  Result := BootstrapBeforeFileInstall;
  if Result <> '' then
    Log('Fatal machine service bootstrap failure: ' + Result);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    RunFixedBrokerOrFail('uninstall-owned', 'Owned machine service removal');
end;
