; Compile through build-installer.ps1 so the payload and dependencies are verified.
#ifndef BuildConfigFile
  #error BuildConfigFile is required
#endif
#include BuildConfigFile

[Setup]
#ifdef FixtureBuild
AppId={#FixtureId}
AppName=Contour Installer Test
AppVerName=Contour Installer Test {#AppVersion}
UninstallDisplayName=Contour Installer Test
DefaultDirName={#FixtureRoot}\app
PrivilegesRequired=lowest
OutputBaseFilename=Contour-Installer-Test-{#AppVersion}
#else
AppId={{A93D1404-762A-449C-9B60-69D42621BBD5}
AppName=Contour
AppVerName=Contour {#AppVersion}
UninstallDisplayName=Contour
DefaultDirName={commonpf}\Contour
PrivilegesRequired=admin
OutputBaseFilename=Contour-{#AppVersion}-Setup-x64
#endif
AppVersion={#AppVersion}
VersionInfoVersion={#AppVersion}
SetupArchitecture=x64
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
UsePreviousAppDir=no
DisableDirPage=yes
DisableProgramGroupPage=yes
UsePreviousTasks=yes
WizardStyle=modern dynamic
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\contour.exe
CloseApplications=no
RestartApplications=no
SetupLogging=yes
UninstallLogging=yes
OutputDir={#OutputDirectory}
Compression=lzma2
SolidCompression=yes
AllowNoIcons=yes
DisableWelcomePage=yes
ChangesAssociations=no
ChangesEnvironment=no

[Tasks]
Name: startmenu; Description: "Add a Start menu shortcut"; GroupDescription: "Shortcuts:";

[Files]
#include FilesInclude
#ifndef FixtureBuild
Source: "{#PrerequisiteDirectory}\vc_redist.x64.exe"; Flags: dontcopy
Source: "{#PrerequisiteDirectory}\MicrosoftEdgeWebview2Setup.exe"; Flags: dontcopy
#endif

[Icons]
#ifdef FixtureBuild
Name: "{#FixtureRoot}\shortcuts\Contour"; Filename: "{app}\contour.exe"; Tasks: startmenu
#else
Name: "{commonprograms}\Contour"; Filename: "{app}\contour.exe"; Tasks: startmenu
#endif

#ifndef FixtureBuild
[Run]
Filename: "{app}\contour.exe"; Description: "Open Contour"; Flags: nowait postinstall skipifsilent runasoriginaluser
#endif

[Messages]
WizardSelectTasks=Shortcuts
SelectTasksDesc=Add Contour to the Start menu.
SelectTasksLabel2=Choose whether to add a Start menu shortcut, then click Next.
WelcomeLabel2=Install Contour, then choose the devices you want to use in Settings.
FinishedLabel=Contour is installed.%n%nOpen Contour and choose your audio devices in Settings.
ConfirmUninstall=Remove Contour and its audio processing from all installed devices?%n%nAudio may briefly stop. Your profiles and preferences will be kept.

[Code]
const
#ifdef FixtureBuild
  InstallRoot = HKCU64;
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{#FixtureId}_is1';
#else
  InstallRoot = HKLM64;
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{A93D1404-762A-449C-9B60-69D42621BBD5}_is1';
#endif
  RuntimeKey = 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  WebViewKey = 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}';

function GetFileAttributes(Path: String): LongWord;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function ExpectedDirectory: String;
begin
#ifdef FixtureBuild
  Result := '{#FixtureRoot}\app';
#else
  Result := ExpandConstant('{commonpf}\Contour');
#endif
end;

function ShortcutPath: String;
begin
#ifdef FixtureBuild
  Result := '{#FixtureRoot}\shortcuts\Contour.lnk';
#else
  Result := ExpandConstant('{commonprograms}\Contour.lnk');
#endif
end;

function AtLeastVersion(Value, Required: String): Boolean;
var A, B: Int64;
begin
  if Copy(Value, 1, 1) = 'v' then Delete(Value, 1, 1);
  Result := StrToVersion(Value, A) and StrToVersion(Required, B);
  if Result then Result := ComparePackedVersion(A, B) >= 0;
end;

procedure InitializeWizard;
begin
  WizardForm.ReadyMemo.WordWrap := True;
  WizardForm.ReadyMemo.ScrollBars := ssVertical;
end;

function RuntimePresent: Boolean;
var Installed: Cardinal; Value: String;
begin
#ifdef FixtureBuild
  Result := FileExists('{#FixtureRoot}\control\vc-ready');
#else
  Result := RegQueryDWordValue(HKLM32, RuntimeKey, 'Installed', Installed) and
    (Installed = 1) and RegQueryStringValue(HKLM32, RuntimeKey, 'Version', Value);
  if not Result then
    Result := RegQueryDWordValue(HKLM64, RuntimeKey, 'Installed', Installed) and
      (Installed = 1) and RegQueryStringValue(HKLM64, RuntimeKey, 'Version', Value);
  if Result then Result := AtLeastVersion(Value, '{#RuntimeVersion}');
#endif
end;

function WebViewPresent: Boolean;
var Value: String;
begin
#ifdef FixtureBuild
  Result := FileExists('{#FixtureRoot}\control\webview-ready');
#else
  { This is a per-machine install. A different administrator's per-user runtime
    does not establish availability for the person who will run Contour. }
  Result := RegQueryStringValue(HKLM32, WebViewKey, 'pv', Value);
  if Result then Result := AtLeastVersion(Value, '1.0.0.0');
#endif
end;

function CheckEditorClosed: String;
var Locator, Services, Processes: Variant;
begin
  Result := '';
#ifdef FixtureBuild
  if FileExists('{#FixtureRoot}\control\busy') then
    Result := 'Quit Contour from its tray menu, then try again.';
#else
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Services := Locator.ConnectServer('', 'root\CIMV2');
    Processes := Services.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''contour.exe''');
    if Processes.Count > 0 then
      Result := 'Save your changes and quit Contour from its tray menu, then try again.';
  except
    Log(GetExceptionMessage);
    Result := 'Windows could not check whether Contour is open. Close Contour and try again.';
  end;
#endif
end;

function SafeDirectoryTree(Path: String): Boolean;
var FindRec: TFindRec;
begin
  Result := True;
  if DirExists(Path) and FindFirst(AddBackslash(Path) + '*', FindRec) then begin
    try
      repeat
        if (FindRec.Name <> '.') and (FindRec.Name <> '..') then begin
          if (FindRec.Attributes and $400) <> 0 then Result := False
          else if (FindRec.Attributes and FILE_ATTRIBUTE_DIRECTORY) <> 0 then
            Result := SafeDirectoryTree(AddBackslash(Path) + FindRec.Name);
          if not Result then Break;
        end;
      until not FindNext(FindRec);
    finally
      FindClose(FindRec);
    end;
  end;
end;

function CheckInstallDirectory: String;
var Path: String; Attributes: LongWord;
begin
  Result := '';
  Path := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  if CompareText(Path, ExpectedDirectory) <> 0 then begin
    Result := 'Contour must be installed in ' + ExpectedDirectory + '.';
    Exit;
  end;
  { Refuse redirected folders before writing or invoking elevated helpers. }
  repeat
    Attributes := GetFileAttributes(Path);
    if (Attributes <> $FFFFFFFF) and ((Attributes and $400) <> 0) then begin
      Result := 'The Contour installation folder is redirected. Restore its original folder and try again.';
      Exit;
    end;
    Path := ExtractFileDir(Path);
  until (Length(Path) <= 3);
  if not SafeDirectoryTree(ExpectedDirectory) then
    Result := 'The Contour installation contains a redirected file or folder. Restore it and try again.';
end;

function InstallDependency(Name, Arguments, Hash: String; var Code: Integer): Boolean;
var Path: String;
begin
#ifdef FixtureBuild
  if Name = 'vc_redist.x64.exe' then Path := 'vc' else Path := 'webview';
  SaveStringToFile('{#FixtureRoot}\control\dependency-calls', Path + #13#10, True);
  Result := True;
  if FileExists('{#FixtureRoot}\control\dependency-fail') then Code := 1603
  else if FileExists('{#FixtureRoot}\control\dependency-restart') then Code := 3010
  else begin
    Code := 0;
    SaveStringToFile('{#FixtureRoot}\control\' + Path + '-ready', 'ready', False);
  end;
#else
  ExtractTemporaryFile(Name);
  Path := ExpandConstant('{tmp}\') + Name;
  if CompareText(GetSHA256OfFile(Path), Hash) <> 0 then
    RaiseException('A required component could not be verified. Download Contour again and retry.');
  Result := Exec(Path, Arguments, '', SW_HIDE, ewWaitUntilTerminated, Code);
#endif
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var Previous: String; Code: Integer; Launched: Boolean;
begin
  Result := CheckInstallDirectory;
  if Result <> '' then Exit;
  Result := CheckEditorClosed;
  if Result <> '' then Exit;
  if RegQueryStringValue(InstallRoot, UninstallKey, 'DisplayVersion', Previous) then
    if not AtLeastVersion('{#AppVersion}', Previous) then begin
      Result := 'A newer version of Contour is already installed. Use that version or a newer installer.';
      Exit;
    end;
  if not RuntimePresent then begin
    WizardForm.StatusLabel.Caption := 'Installing Microsoft Visual C++ Runtime...';
    Launched := InstallDependency('vc_redist.x64.exe', '/install /quiet /norestart', '{#RuntimeHash}', Code);
    Log(Format('Visual C++ installer result: %d', [Code]));
    if Launched and (Code = 3010) then begin
      NeedsRestart := True;
      Result := 'Windows needs to restart to finish installing a required component. Restart, then run Contour Setup again.';
      Exit;
    end;
    if not Launched or ((Code <> 0) and (Code <> 1638)) or not RuntimePresent then begin
      Result := 'Microsoft Visual C++ Runtime could not be installed. Restart Windows and try again.';
      Exit;
    end;
  end;
  if not WebViewPresent then begin
    WizardForm.StatusLabel.Caption := 'Installing Microsoft Edge WebView2 Runtime...';
    Launched := InstallDependency('MicrosoftEdgeWebview2Setup.exe', '/silent /install', '{#WebViewHash}', Code);
    Log(Format('WebView2 installer result: %d', [Code]));
    if not Launched or (Code <> 0) or not WebViewPresent then begin
      Result := 'Microsoft Edge WebView2 Runtime could not be installed. Check your internet connection and try again.';
      Exit;
    end;
  end;
  { Dependency installation can take time: do not overwrite an editor opened
    while a prerequisite was downloading. }
  Result := CheckEditorClosed;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := 'Contour will be installed in:' + NewLine + Space + ExpectedDirectory + NewLine;
  if MemoTasksInfo <> '' then Result := Result + NewLine + MemoTasksInfo + NewLine;
  if not RuntimePresent then Result := Result + NewLine + 'Microsoft Visual C++ Runtime will also be installed.';
  if not WebViewPresent then Result := Result + NewLine + 'Microsoft Edge WebView2 Runtime will be downloaded and installed. An internet connection is required.';
  Result := Result + NewLine + NewLine + 'Your profiles and existing device installations will be kept. Choose devices in Contour after installation.';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var Shell, Shortcut: Variant;
begin
  if (CurStep = ssPostInstall) and not WizardIsTaskSelected('startmenu') and FileExists(ShortcutPath) then begin
    { A later upgrade can opt out. Remove only a shortcut targeting this copy. }
    Shell := CreateOleObject('WScript.Shell');
    Shortcut := Shell.CreateShortcut(ShortcutPath);
    if CompareText(Shortcut.TargetPath, ExpandConstant('{app}\contour.exe')) = 0 then
      if not DeleteFile(ShortcutPath) then Log('The old Start menu shortcut could not be removed.');
  end;
end;

function InitializeUninstall: Boolean;
var Error: String;
begin
  Error := CheckInstallDirectory;
  if Error = '' then Error := CheckEditorClosed;
  Result := Error = '';
  if not Result then SuppressibleMsgBox(Error, mbError, MB_OK, IDOK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var Error, Params: String; Code, I: Integer; Output: TExecOutput; Launched: Boolean;
begin
  if CurUninstallStep <> usUninstall then Exit;
  Error := CheckEditorClosed;
  if Error <> '' then RaiseException(Error);
  UninstallProgressForm.StatusLabel.Caption := 'Removing Contour audio processing...';
  Params := '-NoProfile -NonInteractive -ExecutionPolicy RemoteSigned -File "' +
    ExpandConstant('{app}\setup\contour-uninstall.ps1') + '" -Action Remove -PlainText';
  Launched := ExecAndCaptureOutput(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    Params, ExpandConstant('{app}'), SW_SHOWNORMAL, ewWaitUntilTerminated, Code, Output);
  Error := '';
  for I := 0 to GetArrayLength(Output.StdOut) - 1 do begin
    Log(Output.StdOut[I]);
    if Length(Error) < 2000 then Error := Error + Output.StdOut[I] + #13#10;
  end;
  for I := 0 to GetArrayLength(Output.StdErr) - 1 do Log(Output.StdErr[I]);
  if not Launched or (Code <> 0) or Output.Error then
    { Inno treats exceptions in usUninstall as fatal BEFORE deleting any app
      files. Keep the helper and uninstaller available for a recovery retry. }
    RaiseException('Contour could not finish removing its audio processing. The app was kept so you can retry.' + #13#10#13#10 + Error);
end;
