; QuickScriptTool 安装包脚本（Inno Setup 6）
; 1. 先运行: powershell -ExecutionPolicy Bypass -File tools\package_release.ps1
; 2. （可选）下载 vc_redist.x64.exe 放到 installer\redist\ 目录
;    https://aka.ms/vs/17/release/vc_redist.x64.exe
; 3. 用 Inno Setup 编译本文件，或在命令行执行:
;    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" installer\QuickScriptTool.iss

#define MyAppName "鼠大侠脚本工具"
#define MyAppExeName "QuickScriptTool.exe"
#define MyAppVersion "1.0.0"
#define SourceDir "..\\dist\\QuickScriptTool"
#define RedistDir "..\\installer\\redist"

[Setup]
AppId={{A8F3C2E1-9B4D-4A7E-8C1F-QuickScriptTool01}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=QuickScriptTool
DefaultDirName={autopf}\QuickScriptTool
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=QuickScriptTool-Setup-{#MyAppVersion}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin

[Languages]
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加选项:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\QuickScriptTool.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\opencv_world*.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\tools\*"; DestDir: "{app}\tools"; Flags: ignoreversion recursesubdirs
Source: "{#RedistDir}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: RedistBundled

[Dirs]
Name: "{app}\scripts"
Name: "{app}\recordings"

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "正在安装 Visual C++ 运行库..."; Check: RedistBundled; Flags: waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "启动 {#MyAppName}"; Flags: nowait postinstall skipifsilent

[Code]
function RedistBundled: Boolean;
begin
  Result := FileExists(ExpandConstant('{#RedistDir}\vc_redist.x64.exe'));
end;
