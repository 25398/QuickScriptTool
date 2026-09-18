; QuickScriptTool 安装包脚本（Inno Setup 6）
;
; 发版流程：
;   1. 确认 tools\product_version.txt 与下方 MyAppVersion 一致（当前 1.3.3）
;   2. powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
;      （会把 MSVC CRT / WebView2Fixed / ui / extension / 驱动安装脚本等打进 dist\QuickScriptTool；内核 .sys 不随默认包）
;   3. 编译本脚本（需 Inno Setup 6；简体中文语言文件在本目录 ChineseSimplified.isl）：
;      & "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\QuickScriptTool.iss
;   4. powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -SkipBuild
;      （把 Setup exe 同步到 website\downloads）
;
; 已安装检测：同一 AppId 的登记目录视为「官方安装」，可原地更新。
; 同时扫描 HKCU LastRunDir / 正在运行的进程，预填「正在使用的副本」（含开发目录）。
; 主程序 asInvoker：默认装到 {localappdata}（可写）。仍可浏览改到其它目录。
; 若装到 Program Files，设置/脚本可能无法写入，需右键管理员运行或改回可写目录。
; WebView2 用户数据在 Program Files 下会漫游到 %LOCALAPPDATA%\QuickScriptTool。
; WebView2 Fixed 单独压缩块：运行时版本未变则跳过解压（升级不必再写一遍 ~200MB 运行时）。
; 卸载：开始菜单 / 安装目录 Uninstall.exe / 系统「应用和功能」；会清安装目录残留与本软件注册表。

#define MyAppName "键鼠工坊"
#define MyAppExeName "QuickScriptTool.exe"
#define MyAppVersion "1.3.3"
#define MyAppPublisher "QuickScriptTool"
#define MyAppURL "https://www.quickscripttool.cloud/"
#define SourceDir "..\\dist\\QuickScriptTool"
#define UninstallRegKey "Software\Microsoft\Windows\CurrentVersion\Uninstall\{A8F3C2E1-9B4D-4A7E-8C1F-QuickScriptTool01}_is1"
#define ProductRegKey "Software\QuickScriptTool"
#define WebView2RuntimeVersion "133.0.3065.92"

[Setup]
AppId={{A8F3C2E1-9B4D-4A7E-8C1F-QuickScriptTool01}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
AppContact=24353623@qq.com
AppCopyright=Copyright (C) {#MyAppPublisher}
VersionInfoVersion={#MyAppVersion}.0
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName} {#MyAppVersion}
VersionInfoCopyright=Copyright (C) {#MyAppPublisher}
; 主程序 asInvoker，安装目录需用户可写（设置/脚本写在 exe 旁）。
DefaultDirName={localappdata}\QuickScriptTool
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
DisableDirPage=no
UsePreviousAppDir=yes
UsePreviousGroup=yes
UsePreviousTasks=yes
DirExistsWarning=no
MinVersion=10.0
OutputDir=..\dist
; 不要用带 "Setup" 的文件名：Win11 DetectorsAppHealth 兼容垫片会让 Inno 误报
; “The setup files are corrupted”。
OutputBaseFilename=QuickScriptTool-{#MyAppVersion}
SetupIconFile=..\resources\app_icon.ico
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallFilesDir={app}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UsedUserAreasWarning=no
ChangesAssociations=no
CloseApplications=force
CloseApplicationsFilter=QuickScriptTool.exe,QstWebViewShell.exe
RestartApplications=no
AppMutex=KeyMouse_SingleInstance_Setup
SetupLogging=yes
ShowLanguageDialog=no

[Languages]
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"

[CustomMessages]
UpgradeReady=将更新本机已安装的键鼠工坊到 {#MyAppVersion}。%n%n安装位置：%1
FreshReady=将安装键鼠工坊 {#MyAppVersion} 到：%n%n%1
DirPageHint=请选择安装位置。可点击「浏览」改到其他磁盘或文件夹。
DirPageDetected=检测到本机正在使用：%1%n可保持该目录覆盖安装，或浏览选择新位置。
DevDirConfirm=所选目录看起来是编译输出（build\Release 等）。安装会覆盖其中的文件，可能影响本地构建。确定安装到这里吗？

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加选项:"; Flags: unchecked

[Files]
; 程序文件（不含 WebView2 Fixed）：升级时始终覆盖
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "WebView2Fixed\*"; Flags: ignoreversion recursesubdirs createallsubdirs
; WebView2 单独压缩块：已安装且版本相同则跳过解压
Source: "{#SourceDir}\WebView2Fixed\*"; DestDir: "{app}\WebView2Fixed"; Flags: ignoreversion recursesubdirs createallsubdirs solidbreak; Check: ShouldInstallWebView2

[InstallDelete]
; 旧发版曾旁路一份同内容的 QstWebViewShell.exe，升级后只保留 QuickScriptTool.exe
Type: files; Name: "{app}\QstWebViewShell.exe"
Type: files; Name: "{app}\QstWebViewShell.pdb"

[Dirs]
Name: "{app}\scripts"
Name: "{app}\recordings"
Name: "{localappdata}\QuickScriptTool"
Name: "{localappdata}\QuickScriptTool\WebView2UserData"
Name: "{localappdata}\QuickScriptTool\WebView2FetchData"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Comment: "启动 {#MyAppName}"
Name: "{group}\卸载 {#MyAppName}"; Filename: "{app}\Uninstall.exe"; Comment: "卸载 {#MyAppName} 并清理安装目录"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Registry]
; 产品安装信息（「应用和功能」由 Inno 按 AppId 自动写入 Uninstall 键）
Root: HKLM; Subkey: "{#ProductRegKey}"; ValueType: string; ValueName: "InstallPath"; ValueData: "{app}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "{#ProductRegKey}"; ValueType: string; ValueName: "Version"; ValueData: "{#MyAppVersion}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "{#ProductRegKey}"; ValueType: string; ValueName: "DisplayName"; ValueData: "{#MyAppName}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "{#ProductRegKey}"; ValueType: string; ValueName: "Publisher"; ValueData: "{#MyAppPublisher}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "{#ProductRegKey}"; ValueType: string; ValueName: "URL"; ValueData: "{#MyAppURL}"; Flags: uninsdeletekey
; Win+R / 系统「打开文件位置」可解析到主程序
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\{#MyAppExeName}"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Microsoft\Windows\CurrentVersion\App Paths\{#MyAppExeName}"; ValueType: string; ValueName: "Path"; ValueData: "{app}"
; 开机启动项由软件内设置写入；卸载时删掉残留（安装时不创建）
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "键鼠工坊"; Flags: uninsdeletevalue dontcreatekey

[Run]
; postinstall 默认 runasoriginaluser，会走 SpawnServer 降权启动。
; Win11 + 已提权安装时 SpawnServer 会报 CallSpawnServer: Unexpected response: $0。
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent runascurrentuser

[UninstallDelete]
Type: filesandordirs; Name: "{app}\scripts"
Type: filesandordirs; Name: "{app}\recordings"
Type: filesandordirs; Name: "{app}\WebView2UserData"
Type: filesandordirs; Name: "{app}\WebView2FetchData"
Type: filesandordirs; Name: "{localappdata}\QuickScriptTool\WebView2UserData"
Type: filesandordirs; Name: "{localappdata}\QuickScriptTool\WebView2FetchData"
Type: files; Name: "{localappdata}\QuickScriptTool\webview_boot.log"
Type: files; Name: "{localappdata}\QuickScriptTool\shell_startup.log"
Type: dirifempty; Name: "{localappdata}\QuickScriptTool"
Type: filesandordirs; Name: "{app}\library"
Type: filesandordirs; Name: "{app}\agent_changes"
Type: filesandordirs; Name: "{app}\agent_conversations"
Type: filesandordirs; Name: "{app}\images"
Type: files; Name: "{app}\app_settings.json"
Type: files; Name: "{app}\home_state.txt"
Type: files; Name: "{app}\scheduled_tasks.json"
Type: files; Name: "{app}\*.log"
Type: files; Name: "{app}\*.json"
Type: dirifempty; Name: "{app}"

[Code]
var
  CachedOfficialAppDir: String;
  HasCachedOfficialAppDir: Boolean;
  CachedDetectedCopyDir: String;
  HasCachedDetectedCopyDir: Boolean;
  DidPrefillDir: Boolean;

function UninstallKey: String;
begin
  Result := '{#UninstallRegKey}';
end;

function ProductKey: String;
begin
  Result := '{#ProductRegKey}';
end;

function AppPathsKey: String;
begin
  Result := 'Software\Microsoft\Windows\CurrentVersion\App Paths\{#MyAppExeName}';
end;

function NormalizeDir(const Path: String): String;
begin
  Result := Trim(Path);
  if (Length(Result) >= 2) and (Result[1] = '"') then
  begin
    Delete(Result, 1, 1);
    if (Length(Result) > 0) and (Result[Length(Result)] = '"') then
      Delete(Result, Length(Result), 1);
  end;
  Result := RemoveBackslashUnlessRoot(Trim(Result));
end;

function DirHasProduct(const Dir: String): Boolean;
var
  Root: String;
begin
  Root := NormalizeDir(Dir);
  Result := (Root <> '') and DirExists(Root) and (
    FileExists(AddBackslash(Root) + '{#MyAppExeName}') or
    FileExists(AddBackslash(Root) + 'QstWebViewShell.exe'));
end;

function LooksLikeDevBuildDir(const Dir: String): Boolean;
var
  Root, Lower: String;
begin
  Root := NormalizeDir(Dir);
  Lower := LowerCase(Root);
  Result :=
    (Pos('\build\release', Lower) > 0) or
    (Pos('\build\debug', Lower) > 0) or
    FileExists(AddBackslash(Root) + 'QstWebViewShell.pdb') or
    FileExists(AddBackslash(Root) + 'WindowModeSelfTest.exe') or
    FileExists(AddBackslash(Root) + 'ScheduledTaskSelfTest.exe');
end;

function DirFromExePath(const ExePath: String): String;
var
  Path: String;
begin
  Result := '';
  Path := NormalizeDir(ExePath);
  if Path = '' then Exit;
  if CompareText(ExtractFileExt(Path), '.exe') = 0 then
    Path := ExtractFileDir(Path);
  if DirHasProduct(Path) then
    Result := NormalizeDir(Path);
end;

function QueryDirFromUninstallRoot(const RootKey: Integer): String;
var
  Path: String;
begin
  Result := '';
  if RegQueryStringValue(RootKey, UninstallKey, 'Inno Setup: App Path', Path) then
  begin
    Result := DirFromExePath(Path);
    if Result <> '' then Exit;
  end;
  if RegQueryStringValue(RootKey, UninstallKey, 'InstallLocation', Path) then
  begin
    Result := DirFromExePath(Path);
    if Result <> '' then Exit;
  end;
end;

function QueryDirFromProductRoot(const RootKey: Integer): String;
var
  Path: String;
begin
  Result := '';
  if RegQueryStringValue(RootKey, ProductKey, 'InstallPath', Path) then
    Result := DirFromExePath(Path);
end;

function QueryDirFromAppPaths(const RootKey: Integer): String;
var
  Path: String;
begin
  Result := '';
  if RegQueryStringValue(RootKey, AppPathsKey, '', Path) then
    Result := DirFromExePath(Path);
end;

function QueryLastRunDir: String;
var
  Path: String;
begin
  Result := '';
  if RegQueryStringValue(HKCU, ProductKey, 'LastRunDir', Path) then
    Result := DirFromExePath(Path);
  if (Result = '') and RegQueryStringValue(HKLM, ProductKey, 'LastRunDir', Path) then
    Result := DirFromExePath(Path);
end;

function FirstDirFromLines(const FileName: String): String;
var
  Lines: TArrayOfString;
  I, P: Integer;
  Line: String;
begin
  Result := '';
  if not FileExists(FileName) then Exit;
  if not LoadStringsFromFile(FileName, Lines) then Exit;
  for I := 0 to GetArrayLength(Lines) - 1 do
  begin
    Line := Trim(Lines[I]);
    if Line = '' then Continue;
    P := Pos('=', Line);
    if P > 0 then
      Line := Trim(Copy(Line, P + 1, MaxInt));
    Result := DirFromExePath(Line);
    if Result <> '' then Exit;
  end;
end;

function QueryWmicProcessDir(const ProcessName: String): String;
var
  Tmp: String;
  ResultCode: Integer;
begin
  Result := '';
  Tmp := ExpandConstant('{tmp}\qst_wmic_' + ChangeFileExt(ProcessName, '') + '.txt');
  if not Exec(ExpandConstant('{cmd}'),
    '/C wmic process where "name=''' + ProcessName + '''" get ExecutablePath /VALUE > "' + Tmp + '" 2>nul',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Exit;
  Result := FirstDirFromLines(Tmp);
end;

function QueryCimProcessDir: String;
var
  TmpPs, TmpOut: String;
  ResultCode: Integer;
begin
  Result := '';
  TmpPs := ExpandConstant('{tmp}\qst_find_proc.ps1');
  TmpOut := ExpandConstant('{tmp}\qst_find_proc.txt');
  SaveStringToFile(TmpPs,
    '$out = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) ''qst_find_proc.txt''' + #13#10 +
    'Get-CimInstance Win32_Process | Where-Object { $_.Name -match ''^(QstWebViewShell|QuickScriptTool)\.exe$'' } | ForEach-Object { $_.ExecutablePath } | Out-File -FilePath $out -Encoding ASCII',
    False);
  if not Exec(ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe'),
    '-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "' + TmpPs + '"',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
    Exit;
  Result := FirstDirFromLines(TmpOut);
end;

function GetOfficialAppDir: String;
begin
  if HasCachedOfficialAppDir then
  begin
    Result := CachedOfficialAppDir;
    Exit;
  end;
  Result := QueryDirFromUninstallRoot(HKLM);
  if Result = '' then
    Result := QueryDirFromUninstallRoot(HKCU);
  if Result = '' then
    Result := QueryDirFromProductRoot(HKLM);
  if Result = '' then
    Result := QueryDirFromProductRoot(HKCU);
  if Result = '' then
    Result := QueryDirFromAppPaths(HKLM);
  if Result = '' then
    Result := QueryDirFromAppPaths(HKCU);
  if LooksLikeDevBuildDir(Result) then
    Result := '';
  CachedOfficialAppDir := Result;
  HasCachedOfficialAppDir := True;
end;

function GetDetectedCopyDir: String;
begin
  if HasCachedDetectedCopyDir then
  begin
    Result := CachedDetectedCopyDir;
    Exit;
  end;
  Result := QueryLastRunDir;
  if Result = '' then
    Result := QueryCimProcessDir;
  if Result = '' then
    Result := QueryWmicProcessDir('QstWebViewShell.exe');
  if Result = '' then
    Result := QueryWmicProcessDir('QuickScriptTool.exe');
  CachedDetectedCopyDir := Result;
  HasCachedDetectedCopyDir := True;
end;

function IsOfficialUpgrade(): Boolean;
begin
  Result := GetOfficialAppDir <> '';
end;

function InstallingOverOfficial(): Boolean;
begin
  Result := (GetOfficialAppDir <> '') and SameText(NormalizeDir(WizardDirValue), GetOfficialAppDir);
end;

procedure ApplyPreferredAppDir;
var
  Detected, Official: String;
begin
  if DidPrefillDir then Exit;
  if ExpandConstant('{param:DIR|}') <> '' then
  begin
    DidPrefillDir := True;
    Exit;
  end;
  Detected := GetDetectedCopyDir;
  Official := GetOfficialAppDir;
  if Detected <> '' then
    WizardForm.DirEdit.Text := Detected
  else if Official <> '' then
    WizardForm.DirEdit.Text := Official;
  DidPrefillDir := True;
end;

procedure UpdateDirPageHint;
var
  Detected: String;
begin
  Detected := GetDetectedCopyDir;
  if Detected <> '' then
    WizardForm.SelectDirLabel.Caption := FmtMessage(CustomMessage('DirPageDetected'), [Detected])
  else
    WizardForm.SelectDirLabel.Caption := CustomMessage('DirPageHint');
end;

function ShouldInstallWebView2(): Boolean;
var
  Marker, Current, Exe: String;
  Raw: AnsiString;
  Size: Integer;
begin
  Exe := ExpandConstant('{app}\WebView2Fixed\msedgewebview2.exe');
  if not FileExists(Exe) then
  begin
    Result := True;
    Exit;
  end;
  Size := 0;
  if not FileSize(Exe, Size) or (Size < 400000) then
  begin
    Result := True;
    Exit;
  end;
  Marker := ExpandConstant('{app}\WebView2Fixed\qst-runtime-version.txt');
  if not FileExists(Marker) then
  begin
    Result := True;
    Exit;
  end;
  if not LoadStringFromFile(Marker, Raw) then
  begin
    Result := True;
    Exit;
  end;
  Current := Trim(String(Raw));
  StringChangeEx(Current, #13, '', True);
  StringChangeEx(Current, #10, '', True);
  Result := not SameText(Current, '{#WebView2RuntimeVersion}');
end;

procedure CloseAppProcesses;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM QuickScriptTool.exe /T', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM QstWebViewShell.exe /T', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function InitializeSetup(): Boolean;
begin
  Result := True;
end;

procedure InitializeWizard();
begin
  ApplyPreferredAppDir;
  if IsOfficialUpgrade then
    WizardForm.Caption := '更新 - {#MyAppName} {#MyAppVersion}'
  else
    WizardForm.Caption := '安装 - {#MyAppName} {#MyAppVersion}';
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpSelectDir then
  begin
    UpdateDirPageHint;
    WizardForm.NextButton.Caption := SetupMessage(msgButtonNext);
  end;
  if CurPageID = wpReady then
  begin
    if InstallingOverOfficial then
    begin
      WizardForm.ReadyLabel.Caption := FmtMessage(CustomMessage('UpgradeReady'), [WizardDirValue]);
      WizardForm.NextButton.Caption := '更新(&U)';
    end
    else
    begin
      WizardForm.ReadyLabel.Caption := FmtMessage(CustomMessage('FreshReady'), [WizardDirValue]);
      WizardForm.NextButton.Caption := SetupMessage(msgButtonInstall);
    end;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then
  begin
    if Trim(WizardDirValue) = '' then
    begin
      MsgBox('请选择安装位置。', mbError, MB_OK);
      Result := False;
      Exit;
    end;
    if LooksLikeDevBuildDir(WizardDirValue) then
      Result := MsgBox(CustomMessage('DevDirConfirm'), mbConfirmation, MB_YESNO) = IDYES;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  NeedsRestart := False;
  CloseAppProcesses;
  Sleep(400);
  Result := '';
end;

function InitializeUninstall(): Boolean;
begin
  CloseAppProcesses;
  Sleep(400);
  Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RegDeleteKeyIncludingSubkeys(HKLM, '{#ProductRegKey}');
    RegDeleteKeyIncludingSubkeys(HKCU, '{#ProductRegKey}');
    RegDeleteKeyIncludingSubkeys(HKLM, 'Software\Microsoft\Windows\CurrentVersion\App Paths\{#MyAppExeName}');
    if RegValueExists(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', '键鼠工坊') then
      RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', '键鼠工坊');
    if not LooksLikeDevBuildDir(ExpandConstant('{app}')) then
      DelTree(ExpandConstant('{app}'), True, True, True);
  end;
end;
