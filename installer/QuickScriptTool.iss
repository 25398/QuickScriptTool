; QuickScriptTool 安装包脚本（Inno Setup 6）
;
; 发版流程：
;   1. 确认 tools\product_version.txt 与下方 MyAppVersion 一致（当前 1.1.10）
;   2. powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
;      （会把 MSVC CRT / WebView2Fixed / ui / extension / 驱动安装链等打进 dist\QuickScriptTool）
;   3. 编译本脚本（需 Inno Setup 6；简体中文语言文件在本目录 ChineseSimplified.isl）：
;      & "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe" installer\QuickScriptTool.iss
;   4. powershell -ExecutionPolicy Bypass -File tools\package_release.ps1 -SkipBuild
;      （把 Setup exe 同步到 website\downloads）
;
; 依赖说明：
;   - 运行库（vcruntime140 / msvcp140 等）已由 package_release 旁路拷进 {app}，
;     安装包递归安装整个 dist，用户无需再装 vc_redist。
;   - WebView2 Fixed Runtime 随包（WebView2Fixed\），无需系统 Evergreen。
;   - Interception / 虚拟 HID / OCR(Python) 仍走软件内安装按钮（安装器只负责把安装链放进 {app}）。
;   - 扩展 / UI / Fixed / VHID：整包从 dist\QuickScriptTool 递归安装（勿加 skipifsourcedoesntexist）

#define MyAppName "键鼠工坊"
#define MyAppExeName "QuickScriptTool.exe"
#define MyAppVersion "1.1.10"
#define SourceDir "..\\dist\\QuickScriptTool"

[Setup]
AppId={{A8F3C2E1-9B4D-4A7E-8C1F-QuickScriptTool01}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=QuickScriptTool
DefaultDirName={autopf}\QuickScriptTool
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
; 不要用带 "Setup" 的文件名：Win11 DetectorsAppHealth 兼容垫片会让 Inno 误报
; “The setup files are corrupted”。
OutputBaseFilename=QuickScriptTool-{#MyAppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin

[Languages]
Name: "chinesesimplified"; MessagesFile: "ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加选项:"; Flags: unchecked

[Files]
; 整包：exe / MSVC CRT / ui / WebView2Fixed / dll / extension / driver / skills / tools …
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Dirs]
Name: "{app}\scripts"
Name: "{app}\recordings"
Name: "{app}\WebView2UserData"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; postinstall 默认 runasoriginaluser，会走 SpawnServer 降权启动。
; Win11 + 已提权安装时 SpawnServer 会报 CallSpawnServer: Unexpected response: $0。
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent runascurrentuser
