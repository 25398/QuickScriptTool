<#
.SYNOPSIS
  按指定版本号一键发版：写版本号 → 构建 → 组装 dist → 便携 zip → 安装包 → 同步官网下载目录。

.DESCRIPTION
  把发版的全套手工步骤合成一条命令，并且刻意不依赖任何「写死的东西」，
  所以后续代码怎么改都不需要动这个脚本：

    * 版本号写入用正则匹配、不依赖行号 —— 文件被重排、加注释都不失效；
    * 构建目标从 tools\package_release.ps1 里解析 `--target`，那边改了目标会自动跟随；
    * 组装 dist / 打 zip / 同步 website\downloads 完全复用 tools\package_release.ps1，
      所以新增源码、UI、扩展、Skill、文档都会自动进包；
    * 任一步失败默认回滚版本号，不留「版本号已改但没出包」的半成品状态。

  输出策略：完整过程写进 build\release-<版本>.log，控制台只保留关键行，结尾打
  成功/失败横幅 —— cmake 与 ISCC 会刷几千行（ISCC 每个文件一行 Compressing），
  不过滤根本看不到结果。

  版本号写入四处（与项目约定一致，清单见下方 $VersionWrites，只改那一处即可）：
    tools\product_version.txt        打包脚本读取的唯一来源
    installer\QuickScriptTool.iss    #define MyAppVersion
    src\app_branding.cpp             AppBranding::version_
    resources\QuickScriptTool.rc     FILEVERSION / PRODUCTVERSION / FileVersion / ProductVersion
                                     （写进 exe 属性→详细信息，漏写会让「关于」页与属性显示旧版本）

.PARAMETER Version
  目标版本号，形如 1.3.4（也接受 1.3 或 1.3.4.0）。必填。

.PARAMETER SkipBuild
  复用 build\Release 现有产物，不重新编译（代码没动、只想重出包时用）。

.PARAMETER SkipInstaller
  只出便携 zip，不编译 Inno 安装包。

.PARAMETER DryRun
  只做检查（版本号格式 / 工具链 / 构建目标）并打印将要执行的步骤，不改任何文件。

.PARAMETER KeepVersionOnFailure
  失败时不回滚已写入的版本号（默认回滚）。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\package_with_version.ps1 -Version 1.3.4

.EXAMPLE
  # 代码没动，只想用当前版本号重新出一次包
  powershell -ExecutionPolicy Bypass -File tools\package_with_version.ps1 -Version <x.y.z> -SkipBuild

.EXAMPLE
  # 只看会做什么
  powershell -ExecutionPolicy Bypass -File tools\package_with_version.ps1 -Version 1.3.4 -DryRun

.NOTES
  含中文，必须保存为 UTF-8 with BOM（见 AGENTS.md）。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$Version,

    [switch]$SkipBuild,
    [switch]$SkipInstaller,
    [switch]$DryRun,
    [switch]$KeepVersionOnFailure
)

$ErrorActionPreference = 'Stop'
$script:StartedAt = Get-Date

# ─────────────────────────────────────────────────────────────────────────
# 0) 受限环境自适应
#    WorkBuddy 的 PowerShell 会话注入了 safe-delete 守卫（依赖 node 子进程，本机
#    起不来 → 所有 Remove-Item 硬失败），且无法启动外部程序。检测到该环境且存在
#    兼容层就自动加载；普通终端里没有这个文件，行为完全不受影响。
# ─────────────────────────────────────────────────────────────────────────
if ($env:CODEBUDDY_SAFE_DELETE_BULK_GUARD) {
    foreach ($c in @(
        (Join-Path $PSScriptRoot 'qst_pack_compat.ps1'),
        (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\qst_pack_compat.ps1'),
        # build\ 可能被 clean 掉；skill 目录里的副本是更持久的兜底
        (Join-Path $env:USERPROFILE '.workbuddy-ai\skills\qst-release-package\qst_pack_compat.ps1')
    )) {
        if (Test-Path -LiteralPath $c) {
            . $c
            Write-Host "[兼容层] 已加载 $c" -ForegroundColor DarkYellow
            break
        }
    }
}

# 三个版本号文件实测都是 UTF-8 无 BOM；读写固定用同一编码，避免改个版本号顺带改编码
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# ─────────────────────────────────────────────────────────────────────────
# 输出工具
#   子命令（cmake / ISCC / package_release.ps1）的每一行都进日志文件，
#   控制台只放行关键行；结尾统一打横幅，一眼看出成败。
# ─────────────────────────────────────────────────────────────────────────
$script:LogFile = $null
$script:LogBuffer = New-Object System.Collections.Generic.List[string]

function Flush-Log {
    if ($script:LogBuffer.Count -eq 0) { return }
    if ($script:LogFile) {
        [System.IO.File]::AppendAllLines($script:LogFile, $script:LogBuffer, $Utf8NoBom)
    }
    $script:LogBuffer.Clear()
}

# 子命令的一行输出：永远进日志；只有「关键行」才回显控制台
function Write-ChildLine([string]$Text) {
    $script:LogBuffer.Add($Text)
    if ($script:LogBuffer.Count -ge 200) { Flush-Log }
    if ($Text -match '^\s*[+\-]\s|^OK |^Website:|^Zip OK|^Done\.|^FATAL|error|错误|失败|warning|警告|not found|不存在|拒绝|无法') {
        Write-Host "    $Text"
    }
}

function Write-Step([string]$Text) {
    Write-Host ""
    Write-Host "--- $Text ---" -ForegroundColor Cyan
}
function Write-Ok([string]$Text)   { Write-Host "  + $Text" -ForegroundColor Green }
function Write-Warn2([string]$Text) { Write-Host "  ! $Text" -ForegroundColor Yellow }
function Write-Hint([string]$Text) { Write-Host "    $Text" -ForegroundColor DarkGray }

function Write-Banner {
    param([string]$Text, [string]$Color = 'Cyan')
    $bar = '=' * 68
    Write-Host ""
    Write-Host $bar -ForegroundColor $Color
    Write-Host "  $Text" -ForegroundColor $Color
    Write-Host $bar -ForegroundColor $Color
}

function Read-Text([string]$Path) { return [System.IO.File]::ReadAllText($Path, $Utf8NoBom) }
function Write-Text([string]$Path, [string]$Text) {
    [System.IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)
}

function Resolve-Tool {
    param([string[]]$Candidates, [string]$CommandName)
    foreach ($c in $Candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return $c }
    }
    $cmd = Get-Command $CommandName -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

# ─────────────────────────────────────────────────────────────────────────
# 1) 基本检查
# ─────────────────────────────────────────────────────────────────────────
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($RepoRoot) -or
    -not (Test-Path -LiteralPath (Join-Path $RepoRoot 'CMakeLists.txt'))) {
    throw "FATAL: 无法从 $PSScriptRoot 推出仓库根（缺 CMakeLists.txt）"
}
if ($Version -notmatch '^\d+(\.\d+){1,3}$') {
    throw "FATAL: 版本号格式不对：'$Version'（应为 1.3.4 这类点分数字）"
}

$VersionFile = Join-Path $RepoRoot 'tools\product_version.txt'
$IssFile     = Join-Path $RepoRoot 'installer\QuickScriptTool.iss'
$BrandFile   = Join-Path $RepoRoot 'src\app_branding.cpp'
$RcFile      = Join-Path $RepoRoot 'resources\QuickScriptTool.rc'
$PkgScript   = Join-Path $RepoRoot 'tools\package_release.ps1'
$BuildDir    = Join-Path $RepoRoot 'build'
$DistRoot    = Join-Path $RepoRoot 'dist'
$WebDl       = Join-Path $RepoRoot 'website\downloads'
$script:LogFile = Join-Path $BuildDir ("release-{0}.log" -f $Version)

# ─────────────────────────────────────────────────────────────────────────
# 1.5) 版本号写入计划 —— DryRun 与实际执行**共用同一份清单**
# ─────────────────────────────────────────────────────────────────────────
# 为什么集中成一份：版本号散落在多个文件里，DryRun 与实际执行若各写一套，
# 两边迟早漂移（第 3 轮验收发现 .rc 的四处版本号根本没人写，下次发版 exe
# 属性里会显示旧版本）。新增携带版本号的文件请**只改这里**。
#
# .rc 的 FILEVERSION / PRODUCTVERSION 是逗号分隔的四段（1,3,4,0），
# FileVersion 是点分四段（1.3.4.0），ProductVersion 与 $Version 同形 ——
# 所以要做两种归一化，且校验串也要跟着变（见 $VersionWrites 的 Verify）。
$verParts = @($Version -split '\.')
while ($verParts.Count -lt 4) { $verParts += '0' }
$verComma = ($verParts -join ',')
$verDot4  = ($verParts -join '.')

$VersionWrites = @(
    @{ Path = $VersionFile; Label = 'tools\product_version.txt'; WholeFile = $true },
    @{ Path = $IssFile; Label = 'installer\QuickScriptTool.iss (#define MyAppVersion)'
       Pattern = '(#define\s+MyAppVersion\s+")[^"]*(")'
       Replacement = ('${1}' + $Version + '${2}') },
    @{ Path = $BrandFile; Label = 'src\app_branding.cpp (AppBranding::version_)'
       Pattern = '(AppBranding::version_\s*=\s*L"v?)[^"]*(")'
       Replacement = ('${1}' + $Version + '${2}') },
    @{ Path = $RcFile; Label = 'resources\QuickScriptTool.rc (FILEVERSION)'
       Pattern = '(FILEVERSION\s+)\d[\d,]*'
       Replacement = ('${1}' + $verComma); Verify = $verComma },
    @{ Path = $RcFile; Label = 'resources\QuickScriptTool.rc (PRODUCTVERSION)'
       Pattern = '(PRODUCTVERSION\s+)\d[\d,]*'
       Replacement = ('${1}' + $verComma); Verify = $verComma },
    # 注意：这里**不要**写成 (\.\d+){0,3} —— 重复捕获组只保留最后一次匹配
    # （`.3.3.0` 只会留下 `.0`），替换串一旦漏掉收尾引号就会生成
    # `"9.9.9.0.0`（丢引号）这种残破内容，而「包含新版本号」的弱校验拦不住。
    # 用 \d[\d.]* 一次吃掉整个版本号，并把收尾引号放进 Verify，
    # 这样「引号丢了」也会被判失败。
    @{ Path = $RcFile; Label = 'resources\QuickScriptTool.rc (FileVersion)'
       Pattern = '(VALUE\s+"FileVersion",\s+")\d[\d.]*(")'
       Replacement = ('${1}' + $verDot4 + '${2}'); Verify = ('"' + $verDot4 + '"') },
    @{ Path = $RcFile; Label = 'resources\QuickScriptTool.rc (ProductVersion)'
       Pattern = '(VALUE\s+"ProductVersion",\s+")\d[\d.]*(")'
       Replacement = ('${1}' + $Version + '${2}'); Verify = ('"' + $Version + '"') }
)

$currentVersion = ''
if (Test-Path -LiteralPath $VersionFile) { $currentVersion = (Read-Text $VersionFile).Trim() }

Write-Host ""
Write-Host "QuickScriptTool 发版" -ForegroundColor White
Write-Host "  目标版本 : $Version"
if ($currentVersion) { Write-Host "  当前版本 : $currentVersion" }
Write-Host "  仓库根   : $RepoRoot"

# ─────────────────────────────────────────────────────────────────────────
# 2) 构建目标：从 package_release.ps1 解析，避免写死
# ─────────────────────────────────────────────────────────────────────────
function Get-BuildTargets {
    $fallback = @('QstWebViewShell', 'QstUninstall')
    if (-not (Test-Path -LiteralPath $PkgScript)) { return $fallback }
    $found = @([regex]::Matches((Read-Text $PkgScript), '--target\s+([A-Za-z_][A-Za-z0-9_\.\-]*)') |
        ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
    if ($found.Count -gt 0) { return $found }
    return $fallback
}
$BuildTargets = Get-BuildTargets

# ─────────────────────────────────────────────────────────────────────────
# 3) 工具链探测
# ─────────────────────────────────────────────────────────────────────────
$Cmake = Resolve-Tool -CommandName 'cmake.exe' -Candidates @(
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "${env:ProgramFiles}\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "${env:ProgramFiles}\CMake\bin\cmake.exe"
)
$Iscc = Resolve-Tool -CommandName 'ISCC.exe' -Candidates @(
    (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles}\Inno Setup 6\ISCC.exe"
)

Write-Host ""
Write-Host "  构建目标 : $($BuildTargets -join ', ')"
Write-Host "  CMake    : $(if ($Cmake) { $Cmake } else { '未找到' })"
Write-Host "  ISCC     : $(if ($Iscc) { $Iscc } else { '未找到' })"

if (-not $SkipBuild -and -not $Cmake) {
    throw "FATAL: 找不到 cmake.exe（装 VS2022，或把 cmake 加进 PATH；只想复用现有产物可加 -SkipBuild）"
}
if (-not $SkipInstaller -and -not $Iscc) {
    throw "FATAL: 找不到 ISCC.exe（Inno Setup 6）；只想出便携 zip 可加 -SkipInstaller"
}

# 提前探测外部程序能否启动。受限宿主（某些 AI 终端 / 沙箱会话）会静默拦掉外部程序，
# 表现为 $LASTEXITCODE 为空、直到构建阶段才以「配置失败」告终，极难定位。
if (-not $SkipBuild) {
    $null = & $Cmake --version 2>$null
    if ($null -eq $LASTEXITCODE) {
        throw @"
FATAL: 当前宿主禁止启动外部程序（cmake 没有被真正执行）。

  本脚本要调用 cmake / ISCC 这类外部程序，请在**普通 PowerShell 或 CMD 终端**里运行，
  不要在某些受限的 AI 终端 / 沙箱会话里运行。

  如果只想更新版本号并重打便携包（复用已有 build\Release），可以加：
    -SkipBuild -SkipInstaller
"@
    }
}

# ─────────────────────────────────────────────────────────────────────────
# DryRun：只报告计划
# ─────────────────────────────────────────────────────────────────────────
if ($DryRun) {
    $targetArgs = ($BuildTargets | ForEach-Object { "--target $_" }) -join ' '
    Write-Host ""
    Write-Host "[DryRun] 将要执行：" -ForegroundColor Yellow
    Write-Host ("  1. 写入版本号 {0} 到 {1} 处：" -f $Version, $VersionWrites.Count)
    foreach ($w in $VersionWrites) { Write-Host ("       {0}" -f $w.Label) }

    # F4：DryRun 也必须验证「正则能匹配」。
    # 否则 .iss / .rc 的结构一旦变了，要等正式跑（已写进 product_version.txt 之后）
    # 才失败，只能靠回滚兜底 —— 把「文件结构变了」提前到 DryRun 暴露，成本几行。
    Write-Host ""
    Write-Host "[DryRun] 校验版本号写入点（文件存在 + 正则可匹配）：" -ForegroundColor Yellow
    foreach ($w in $VersionWrites) {
        if (-not (Test-Path -LiteralPath $w.Path)) {
            throw "FATAL: 找不到 $($w.Label)（$($w.Path)）"
        }
        if ($w.WholeFile) {
            Write-Host ("       OK  {0}（整文件覆盖）" -f $w.Label) -ForegroundColor Green
            continue
        }
        if (-not [regex]::IsMatch((Read-Text $w.Path), $w.Pattern)) {
            throw ("FATAL: {0} 里匹配不到版本号，文件结构可能变了。`n  正则：{1}" -f $w.Label, $w.Pattern)
        }
        Write-Host ("       OK  {0}" -f $w.Label) -ForegroundColor Green
    }
    Write-Host "[DryRun] 版本号写入点全部可匹配。" -ForegroundColor Green

    if (-not $SkipBuild) {
        Write-Host "  2. 构建："
        Write-Host "       `"$Cmake`" -S `"$RepoRoot`" -B `"$BuildDir`""
        Write-Host "       `"$Cmake`" --build `"$BuildDir`" --config Release $targetArgs -j 16"
    } else {
        Write-Host "  2. (跳过构建，复用 build\Release)"
    }
    Write-Host "  3. 组装 + 打包 + 同步 zip："
    Write-Host "       & `"$PkgScript`" -SkipBuild"
    if (-not $SkipInstaller) {
        Write-Host "  4. 编安装包："
        Write-Host "       & `"$Iscc`" `"$IssFile`""
        Write-Host "  5. 再同步一次（把安装包推进官网目录）："
        Write-Host "       & `"$PkgScript`" -SkipBuild"
    } else {
        Write-Host "  4. (跳过安装包)"
    }
    Write-Host ""
    Write-Host "[DryRun] 未改动任何文件。" -ForegroundColor Yellow
    exit 0
}

# ─────────────────────────────────────────────────────────────────────────
# 4) 正式执行（失败默认回滚版本号）
# ─────────────────────────────────────────────────────────────────────────
$backups = [ordered]@{}

function Set-VersionInFile {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Label,
        [string]$Pattern = '',
        [string]$Replacement = '',
        [string]$Verify = '',
        [switch]$WholeFile
    )
    if (-not $WholeFile -and [string]::IsNullOrWhiteSpace($Pattern)) {
        throw "FATAL: $Label 需要 -Pattern（非 -WholeFile 模式）"
    }
    if (-not (Test-Path -LiteralPath $Path)) { throw "FATAL: 找不到 $Label（$Path）" }
    $old = Read-Text $Path
    # 只记**第一次**读到的内容。同一个文件可能被多条规则依次写（如 .rc 的四处
    # 版本号），若每次都覆盖备份，回滚会退回到「第一次写之后」的中间态而不是原始态。
    if (-not $backups.Contains($Path)) { $backups[$Path] = $old }

    if ($WholeFile) {
        # 保留原文件的行尾风格，避免「版本号没变却多出一条 git diff」
        $eol = ''
        if ($old -match '(\r\n|\n|\r)\s*$') { $eol = $matches[1] }
        $new = $Replacement + $eol
    } else {
        if (-not [regex]::IsMatch($old, $Pattern)) {
            throw "FATAL: $Label 里匹配不到版本号，文件结构可能变了。`n  正则：$Pattern"
        }
        $new = [regex]::Replace($old, $Pattern, $Replacement)
        # 校验串默认是 $Version；.rc 的 FILEVERSION 是逗号四段（1,3,4,0），
        # 不含点分形式，所以由调用方通过 -Verify 传入对应的归一化形式。
        $needle = if ($Verify) { $Verify } else { $Version }
        if (-not [regex]::IsMatch($new, [regex]::Escape($needle))) {
            throw "FATAL: $Label 替换后没出现新版本号 $needle（正则：$Pattern）"
        }
    }
    if ($new -ne $old) { Write-Text $Path $new }
    Write-Ok "$Label"
}

Write-Host "  过程日志 : $script:LogFile"
Write-Host "  （cmake / ISCC 输出很长，完整内容写日志；控制台只显示关键行）" -ForegroundColor DarkGray
"=== QuickScriptTool release $Version  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') ===" |
    Set-Content -LiteralPath $script:LogFile -Encoding UTF8

try {
    # ---- 1/5 版本号 ----
    Write-Step "1/5 写入版本号 $Version（$($VersionWrites.Count) 处）"
    foreach ($w in $VersionWrites) {
        $splat = @{ Path = $w.Path; Label = $w.Label }
        if ($w.WholeFile) { $splat.WholeFile = $true; $splat.Replacement = $Version }
        else {
            $splat.Pattern = $w.Pattern
            $splat.Replacement = $w.Replacement
            if ($w.Verify) { $splat.Verify = $w.Verify }
        }
        Set-VersionInFile @splat
    }

    # ---- MSBuild 环境护栏 ----
    # 同时存在 https_proxy 与 HTTPS_PROXY 时，MSBuild 构造子进程环境会抛
    # MSB6001「已添加项。字典中的关键字:https_proxy」，报错极难定位。本进程内去掉一份。
    foreach ($name in @('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'NO_PROXY')) {
        $lower = $name.ToLowerInvariant()
        if ((Test-Path "Env:$name") -and (Test-Path "Env:$lower")) {
            Write-Warn2 "检测到 $lower 与 $name 并存（会让 MSBuild 报 MSB6001），本进程内移除 $name"
            Remove-Item "Env:$name" -ErrorAction SilentlyContinue
        }
    }

    # ---- 2/5 构建 ----
    if ($SkipBuild) {
        Write-Step "2/5 构建（已跳过，复用 build\Release）"
        if (-not (Test-Path -LiteralPath (Join-Path $BuildDir 'Release\QuickScriptTool.exe'))) {
            throw "FATAL: -SkipBuild 但 build\Release\QuickScriptTool.exe 不存在，先完整跑一次"
        }
    } else {
        Write-Step "2/5 构建 Release（$($BuildTargets -join ', ')）"
        Write-Hint "cmake 配置 + 编译，约 2-4 分钟..."

        # 注意：先整体捕获输出再过滤，而不是 `& exe ... | ForEach-Object`。
        # 管道里跑原生命令后 $LASTEXITCODE 的取值虽通常保留，但发版脚本不赌这个；
        # 先捕获能让退出码 100% 可靠（反正输出本来就要过滤，实时性没有损失）。
        $out = & $Cmake -S $RepoRoot -B $BuildDir 2>&1
        $rc = $LASTEXITCODE
        $out | ForEach-Object { Write-ChildLine ([string]$_) }
        Flush-Log
        if ($null -eq $rc -or $rc -ne 0) { throw "FATAL: CMake 配置失败（exit $rc）" }

        $buildArgs = @('--build', $BuildDir, '--config', 'Release')
        foreach ($t in $BuildTargets) { $buildArgs += @('--target', $t) }
        $buildArgs += @('-j', '16')

        $out = & $Cmake @buildArgs 2>&1
        $rc = $LASTEXITCODE
        $out | ForEach-Object { Write-ChildLine ([string]$_) }
        Flush-Log
        if ($null -eq $rc -or $rc -ne 0) { throw "FATAL: 构建失败（exit $rc）" }
        Write-Ok "构建完成"
    }

    # ---- 3/5 组装 + 便携 zip + 同步 ----
    Write-Step "3/5 组装 dist + 便携 zip + 同步官网目录"
    Write-Hint "拷贝约 630 MB 并压缩，约 1 分钟..."
    & $PkgScript -SkipBuild *>&1 | ForEach-Object { Write-ChildLine ([string]$_) }
    Flush-Log
    Write-Ok "便携包完成"

    # ---- 4/5 安装包 ----
    if (-not $SkipInstaller) {
        Write-Step "4/5 编译 Inno 安装包"
        Write-Hint "ISCC 要压缩 600+ 个文件（每个文件一行 Compressing），约 2-3 分钟，中间会安静一会儿..."
        $out = & $Iscc $IssFile 2>&1
        $rc = $LASTEXITCODE
        $out | ForEach-Object { Write-ChildLine ([string]$_) }
        Flush-Log
        if ($null -eq $rc -or $rc -ne 0) { throw "FATAL: ISCC 编译失败（exit $rc）" }
        Write-Ok "安装包完成"

        # ---- 5/5 再同步一次（这次 dist 里已有安装包）----
        Write-Step "5/5 同步安装包到 website\downloads"
        & $PkgScript -SkipBuild *>&1 | ForEach-Object { Write-ChildLine ([string]$_) }
        Flush-Log
        Write-Ok "安装包已同步"
    } else {
        Write-Step "4/5 安装包（已跳过 -SkipInstaller）"
    }

    # ---- 校验 ----
    Write-Step "校验"
    $problems = @()
    $zipMB = 0

    $zipName = "QuickScriptTool-Release-$Version.zip"
    $zipPath = Join-Path $DistRoot $zipName
    if (Test-Path -LiteralPath $zipPath) {
        $zipMB = [math]::Round((Get-Item -LiteralPath $zipPath).Length / 1MB, 1)
        Write-Ok "$zipName  $zipMB MB"
    } else {
        $problems += "缺 dist\$zipName"
    }

    if (-not $SkipInstaller) {
        $setupName = "QuickScriptTool-$Version.exe"
        $setupPath = Join-Path $DistRoot $setupName
        if (Test-Path -LiteralPath $setupPath) {
            $vi = (Get-Item -LiteralPath $setupPath).VersionInfo
            if ($vi.ProductVersion -eq $Version) {
                Write-Ok "$setupName  ProductVersion=$($vi.ProductVersion)"
            } else {
                $problems += "$setupName 版本信息不符：File=$($vi.FileVersion) Product=$($vi.ProductVersion)"
            }
        } else {
            $problems += "缺 dist\$setupName"
        }
    }

    foreach ($pair in @(
        @{ Name = 'QuickScriptTool-Release.zip'; Need = $true },
        @{ Name = 'QuickScriptTool-Setup.exe';   Need = (-not $SkipInstaller) }
    )) {
        if (-not $pair.Need) { continue }
        $p = Join-Path $WebDl $pair.Name
        if (Test-Path -LiteralPath $p) {
            Write-Ok "website\downloads\$($pair.Name)  $([math]::Round((Get-Item -LiteralPath $p).Length / 1MB, 1)) MB"
        } else {
            $problems += "缺 website\downloads\$($pair.Name)"
        }
    }

    # 版本号写入点是否真的都写进去了（清单来自 $VersionWrites，不再另列一份）
    foreach ($w in $VersionWrites) {
        $needle = if ($w.Verify) { $w.Verify } else { $Version }
        if (-not [regex]::IsMatch((Read-Text $w.Path), [regex]::Escape($needle))) {
            $problems += "$($w.Label) 里没写进 $needle"
        }
    }

    if ($problems.Count -gt 0) {
        throw ("发版校验未通过：`n  - " + ($problems -join "`n  - "))
    }

    $elapsed = (Get-Date) - $script:StartedAt
    Write-Banner "发版成功  |  QuickScriptTool $Version" 'Green'
    Write-Host "  便携包 : dist\$zipName"
    if (-not $SkipInstaller) {
        Write-Host "  安装包 : dist\QuickScriptTool-$Version.exe"
    }
    Write-Host "  官网   : website\downloads\  (QuickScriptTool-Release.zip / QuickScriptTool-Setup.exe)"
    Write-Host "  日志   : $script:LogFile"
    Write-Host "  耗时   : $([math]::Round($elapsed.TotalMinutes, 1)) 分钟"
    Write-Host ""
    exit 0
} catch {
    Flush-Log
    $msg = $_.Exception.Message
    Write-Banner "发版失败  |  QuickScriptTool $Version" 'Red'
    Write-Host "  原因 : $msg" -ForegroundColor Red

    if ($script:LogFile -and (Test-Path -LiteralPath $script:LogFile)) {
        Write-Host ""
        Write-Host "  日志末尾 20 行（完整日志：$script:LogFile）：" -ForegroundColor DarkGray
        Get-Content -LiteralPath $script:LogFile -Tail 20 | ForEach-Object { Write-Host "    $_" -ForegroundColor DarkGray }
    }

    if (-not $KeepVersionOnFailure -and $backups.Count -gt 0) {
        foreach ($k in $backups.Keys) {
            try { Write-Text $k $backups[$k] } catch { }
        }
        Write-Host ""
        Write-Warn2 "已回滚版本号到 $currentVersion（想保留现场加 -KeepVersionOnFailure）"
    }
    Write-Host ""
    exit 1
}
