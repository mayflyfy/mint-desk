; Build-only dependency: Inno Setup 7.1+. The installed app has no new runtime.
#ifndef AppVersion
  #error AppVersion is required
#endif
#ifndef AppExecutable
  #error AppExecutable is required
#endif
#ifndef ReleaseDirectory
  #define ReleaseDirectory "..\release"
#endif
#ifdef TestMode
  #define ProductId "MintDesk.InstallerTests"
  #define Preferences "Software\MCC\MintDeskInstallerTests"
  #define StartupKey "Software\MCC\MintDeskInstallerTests\Run"
  #define MenuKey "Software\MCC\MintDeskInstallerTests\Shell\MintDesk"
  #define ProductName "MintDesk Installer Test"
#else
  #define ProductId "MintDesk.1088B995-38B7-460A-A684-790E3F20A5EE"
  #define Preferences "Software\MCC\MintDesk"
  #define StartupKey "Software\Microsoft\Windows\CurrentVersion\Run"
  #define MenuKey "Software\Classes\DesktopBackground\Shell\MintDesk"
  #define ProductName "薄荷桌面助手 / MintDesk"
#endif

[Setup]
AppId={#ProductId}
AppName={#ProductName}
AppVersion={#AppVersion}
AppPublisher=MCC
AppPublisherURL=https://github.com/mayflyfy/mint-desk
AppSupportURL=https://github.com/mayflyfy/mint-desk/issues
AppUpdatesURL=https://github.com/mayflyfy/mint-desk/releases
DefaultDirName={localappdata}\Programs\MintDesk
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
DisableDirPage=no
DisableProgramGroupPage=yes
DisableWelcomePage=yes
UsePreviousAppDir=yes
UsePreviousTasks=no
WizardStyle=modern
SetupIconFile=..\assets\mintdesk.ico
UninstallDisplayIcon={app}\MintDesk.exe
UninstallFilesDir={app}\Uninstall
UninstallDisplayName={#ProductName}
OutputDir={#ReleaseDirectory}
OutputBaseFilename=MintDesk-v{#AppVersion}-setup-x64
Compression=lzma2
SolidCompression=yes
CloseApplications=no
RestartApplications=no
AllowCancelDuringInstall=no
#ifdef TestMode
SetupMutex=Local\MintDesk.InstallerTests.Setup.1
#else
SetupMutex=Local\MintDesk.Setup.1
#endif
VersionInfoCompany=MCC
VersionInfoCopyright=Copyright (c) 2026 MCC

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
chinesesimplified.DesktopIcon=创建桌面快捷方式
chinesesimplified.Autostart=登录 Windows 时自动启动
chinesesimplified.AutoUpdate=自动检查更新
chinesesimplified.TasksHint=请选择需要的选项。
chinesesimplified.LaunchApp=运行薄荷桌面助手
chinesesimplified.CloseFailed=桌面项目尚未完成恢复，或旧版程序仍在使用中。请正常退出旧版，等待恢复完成后重试。安装程序不会强制结束进程。
chinesesimplified.ConfigFailed=程序文件已安装，但未能完整保存集成选项。请检查当前用户目录权限后重新运行安装程序。
chinesesimplified.UninstallFailed=尚未完成桌面恢复或入口清理，已停止卸载。请正常退出程序并完成恢复后重试。
chinesesimplified.SameDirectory=检测到已安装版本。请在原目录升级；如需更换目录，请先卸载（保留布局和文件记录），然后重新安装。
chinesesimplified.KeepData=卸载会保留卡片布局和恢复记录，不删除桌面文件。
english.DesktopIcon=Create a desktop shortcut
english.Autostart=Start automatically when signing in to Windows
english.AutoUpdate=Automatically check for updates
english.TasksHint=Choose your options.
english.LaunchApp=Run MintDesk
english.CloseFailed=Desktop restoration has not finished, or the old version is still in use. Exit it normally, wait for restoration, and retry. Setup never force-terminates it.
english.ConfigFailed=Program files were installed, but integration options could not be fully saved. Check your user-directory permissions and run setup again.
english.UninstallFailed=Desktop restoration or integration cleanup has not finished. Uninstall was stopped. Exit normally, complete restoration, and retry.
english.SameDirectory=An installed version was found. Upgrade in its existing directory. To change directories, uninstall first (your layouts and file records are retained), then install again.
english.KeepData=Uninstall keeps card layouts and recovery records, and never deletes desktop files.

[Tasks]
Name: "desktopicon"; Description: "{cm:DesktopIcon}"
Name: "autostart"; Description: "{cm:Autostart}"
Name: "autoupdate"; Description: "{cm:AutoUpdate}"; Flags: unchecked

[Files]
Source: "{#AppExecutable}"; DestDir: "{app}"; DestName: "MintDesk.exe"; Flags: ignoreversion
Source: "{#AppExecutable}"; DestName: "maintenance.exe"; Flags: dontcopy
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Registry]
Root: HKCU; Subkey: "{#Preferences}"; ValueType: string; ValueName: "InstallDirectory"; ValueData: "{app}"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\MintDesk.exe"; Description: "{cm:LaunchApp}"; Flags: nowait postinstall skipifsilent runasoriginaluser; Check: IntegrationsReady

[Code]
var
  InstallLock: THandle;
  Configured: Boolean;
  MenuEnabled: Boolean;
  PreviousDirectory: String;
  DefaultTasks: String;
  DefaultsApplied: Boolean;

function CreateMutexW(Attributes: NativeInt; InitialOwner: Boolean; Name: String): THandle;
  external 'CreateMutexW@kernel32.dll stdcall';
function CloseHandle(Handle: THandle): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';

procedure ReleaseInstallLock;
begin
  if InstallLock <> 0 then begin
    CloseHandle(InstallLock);
    InstallLock := 0;
  end;
end;

function AcquireInstallLock: Boolean;
var
  Existing: Boolean;
begin
  if InstallLock <> 0 then begin Result := True; exit; end;
#ifdef TestMode
  InstallLock := CreateMutexW(0, False, 'Local\MintDesk.InstallerTests.Instance.1');
#else
  InstallLock := CreateMutexW(0, False, 'Local\DeskCards.Instance.1');
#endif
  Existing := DLLGetLastError = 183;
  Result := (InstallLock <> 0) and not Existing;
  if not Result then ReleaseInstallLock;
end;

function RunMaintenance(Executable: String; Arguments: String): Boolean;
var
  Code: Integer;
begin
  Result := Exec(Executable, Arguments, ExtractFileDir(Executable), SW_HIDE,
    ewWaitUntilTerminated, Code) and (Code = 0);
end;

function SameDirectory: Boolean;
begin
  Result := (PreviousDirectory = '') or
    (CompareText(RemoveBackslashUnlessRoot(ExpandFileName(PreviousDirectory)),
      RemoveBackslashUnlessRoot(ExpandFileName(ExpandConstant('{app}')))) = 0);
end;

function InitializeSetup: Boolean;
var
  Completed: Cardinal;
begin
  RegQueryStringValue(HKCU, '{#Preferences}', 'InstallDirectory', PreviousDirectory);
  MenuEnabled := not RegQueryDWordValue(HKCU, '{#Preferences}', 'InitialIntegrationV1', Completed)
    or RegKeyExists(HKCU, '{#MenuKey}');
  Result := True;
end;

procedure InitializeWizard;
var
  Choices: String;
  Completed, Value: Cardinal;
  I: Integer;
  ExplicitTasks: Boolean;
  DesktopDirectory: String;
begin
  WizardForm.SelectTasksLabel.Caption := CustomMessage('TasksHint');
  ExplicitTasks := False;
  for I := 1 to ParamCount do
    if (Pos('/TASKS=', Uppercase(ParamStr(I))) = 1) or
       (Pos('/MERGETASKS=', Uppercase(ParamStr(I))) = 1) then ExplicitTasks := True;
  if ExplicitTasks then exit;
#ifdef TestMode
  RegQueryStringValue(HKCU, '{#Preferences}', 'FixtureDesktop', DesktopDirectory);
#else
  DesktopDirectory := ExpandConstant('{userdesktop}');
#endif
  Choices := 'desktopicon,autostart,!autoupdate';
  if RegQueryDWordValue(HKCU, '{#Preferences}', 'InitialIntegrationV1', Completed) then begin
    Choices := '!desktopicon,!autostart,!autoupdate';
    if RegQueryDWordValue(HKCU, '{#Preferences}', 'DesktopShortcutEnabled', Value) then begin
      if Value = 1 then Choices := Choices + ',desktopicon';
    end else if FileExists(AddBackslash(DesktopDirectory) + '薄荷桌面助手.lnk') or
      FileExists(AddBackslash(DesktopDirectory) + 'MintDesk.lnk') then Choices := Choices + ',desktopicon';
    if RegValueExists(HKCU, '{#StartupKey}', 'MintDesk') then Choices := Choices + ',autostart';
  end;
  if RegQueryDWordValue(HKCU, '{#Preferences}', 'AutoCheckUpdates', Value) and (Value = 1) then
    Choices := Choices + ',autoupdate';
  DefaultTasks := Choices;
end;

procedure ApplyTaskDefaults;
begin
  if DefaultsApplied then exit;
  DefaultsApplied := True;
  if DefaultTasks <> '' then WizardSelectTasks(DefaultTasks);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpSelectTasks then ApplyTaskDefaults;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not SameDirectory then begin
    if not WizardSilent then MsgBox(CustomMessage('SameDirectory'), mbError, MB_OK);
    Result := False;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  ApplyTaskDefaults;
  if not SameDirectory then begin Result := CustomMessage('SameDirectory'); exit; end;
  if InstallLock <> 0 then exit;
  ExtractTemporaryFile('maintenance.exe');
  if not RunMaintenance(ExpandConstant('{tmp}\maintenance.exe'), '/prepare-install') or
    not AcquireInstallLock then Result := CustomMessage('CloseFailed');
end;

function Bit(Value: Boolean): String;
begin
  if Value then Result := '1' else Result := '0';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Arguments: String;
begin
  if CurStep = ssPostInstall then begin
    Arguments := '/configure-install ' + Bit(WizardIsTaskSelected('desktopicon')) + ' ' +
      Bit(WizardIsTaskSelected('autostart')) + ' ' + Bit(WizardIsTaskSelected('autoupdate')) + ' ' + Bit(MenuEnabled);
    Configured := RunMaintenance(ExpandConstant('{app}\MintDesk.exe'), Arguments);
    ReleaseInstallLock;
    if not Configured then RaiseException(CustomMessage('ConfigFailed'));
  end;
end;

function IntegrationsReady: Boolean;
begin
  Result := Configured;
end;

procedure DeinitializeSetup;
begin
  ReleaseInstallLock;
end;

function InitializeUninstall: Boolean;
var
  RegisteredDirectory: String;
begin
  Result := False;
  if RegQueryStringValue(HKCU, '{#Preferences}', 'InstallDirectory', RegisteredDirectory) and
    (CompareText(RemoveBackslashUnlessRoot(RegisteredDirectory),
      RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) <> 0) then begin
    SuppressibleMsgBox(CustomMessage('SameDirectory'), mbError, MB_OK, IDOK);
    exit;
  end;
  if RunMaintenance(ExpandConstant('{app}\MintDesk.exe'), '/prepare-install') and AcquireInstallLock then
    Result := RunMaintenance(ExpandConstant('{app}\MintDesk.exe'), '/uninstall-integrations');
  if not Result then begin
    ReleaseInstallLock;
    SuppressibleMsgBox(CustomMessage('UninstallFailed'), mbError, MB_OK, IDOK);
  end;
end;

procedure InitializeUninstallProgressForm;
begin
  UninstallProgressForm.StatusLabel.Caption := CustomMessage('KeepData');
end;

procedure DeinitializeUninstall;
begin
  ReleaseInstallLock;
end;
