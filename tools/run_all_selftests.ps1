<#
.SYNOPSIS
  一键构建并运行 QuickScriptTool 的全部模块自检。

.DESCRIPTION
  21 个 SelfTest 之前全靠手工逐个跑（架构评估 P1-7：投入已花、收益未取）。
  本脚本是 CI（.github/workflows/build.yml）与本地共用的唯一入口。

  约定（见 .cursor/skills/module-selftest/SKILL.md）：
    - 每个 SelfTest 支持 --json，stdout 每行 {"name","ok","detail"}，
      末行 {"passed","failed","ok"}；
    - 进程 exit code = 失败用例数（0 = 全过）。
  本脚本按 exit code 判定，失败时回显该 suite 的原始 stdout。

.PARAMETER Tier
  logic  = 纯逻辑 suite（无 GUI / 无驱动 / 无需管理员），可在 CI 跑。
  full   = logic + 交互类（窗口模式 / 注入 / 虚拟 HID），需桌面会话与已装驱动。

.PARAMETER SkipBuild
  跳过构建，直接跑 build\<Configuration> 下已有 exe。

.PARAMETER LogPath
  除控制台外，把全部输出同时写入该文件（CI 归档失败日志用）。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1
  powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1 -Tier full
  powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1 -SkipBuild -LogPath build\selftest.log
#>
[CmdletBinding()]
param(
    [ValidateSet('logic', 'full')]
    [string]$Tier = 'logic',

    [string]$Configuration = 'Release',

    [string]$BuildDir = '',

    [string]$LogPath = '',

    [switch]$SkipBuild,

    [switch]$ListOnly
)

$ErrorActionPreference = 'Stop'

# 未捕获异常必须留下痕迹。宿主（AI 终端 / CI runner）经常只回一个 exit code ——
# 既没有 stdout、也没有日志文件，只能靠反复重跑「猜」是不是抖动（实测踩过：
# 同一个命令 7 次里失败 2 次，失败那两次磁盘上一个字节都没留下）。
# 这个 trap 保证任何异常都以可读形式进 stdout + -LogPath 文件。
trap {
    $detail = "脚本异常终止：$($_.Exception.GetType().Name) :: $($_.Exception.Message)"
    $pos = $_.InvocationInfo.PositionMessage
    if ($pos) { $detail += "`n$pos" }
    Write-Host $detail -ForegroundColor Red
    if ($LogPath) {
        try { Add-Content -Path $LogPath -Value $detail -Encoding UTF8 } catch { }
    }
    exit 3
}

# $PSScriptRoot 在部分宿主里为空 —— 典型是把命令拼成字符串再执行的包装器（AI 终端、
# `powershell -Command "& .\tools\x.ps1"` 的某些实现），此时脚本内 $PSScriptRoot 拿不到值。
# 直接 Split-Path -Parent $null 会抛「无法将参数绑定到参数"Path"，因为该参数是空值」，
# 报错位置还指向调用方行号，极难定位。这里做三级兜底。
$scriptDir = $PSScriptRoot
if (-not $scriptDir) {
    $selfPath = $MyInvocation.MyCommand.Path
    if (-not $selfPath) { $selfPath = $MyInvocation.MyCommand.Definition }
    if ($selfPath -and (Test-Path -LiteralPath $selfPath)) {
        $scriptDir = Split-Path -Parent $selfPath
    }
}
$repoRoot = if ($scriptDir) { Split-Path -Parent $scriptDir } else { '' }
if (-not $repoRoot -or -not (Test-Path (Join-Path $repoRoot 'CMakeLists.txt'))) {
    # 兜底：当前目录就是仓库根（脚本里所有相对路径都相对仓库根）
    $cwd = (Get-Location).Path
    if (Test-Path (Join-Path $cwd 'CMakeLists.txt')) { $repoRoot = $cwd }
}
if (-not $repoRoot -or -not (Test-Path (Join-Path $repoRoot 'CMakeLists.txt'))) {
    throw "无法定位仓库根目录（找不到 CMakeLists.txt）。请在仓库根目录下运行，或用 -BuildDir 显式指定。"
}

if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'build' }
$outDir = Join-Path $BuildDir $Configuration

$script:LogFile = $null
if ($LogPath) {
    $script:LogFile = $LogPath
    $logDir = Split-Path -Parent $LogPath
    if ($logDir -and -not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir -Force | Out-Null }
    if (Test-Path $script:LogFile) { Remove-Item $script:LogFile -Force }
}

# 同时写控制台与（可选）日志文件。CI 里控制台与文件都要，便于归档失败详情。
function Write-Log {
    param([string]$Text = '', [string]$Color = 'Gray')
    Write-Host $Text -ForegroundColor $Color
    if ($script:LogFile) { Add-Content -Path $script:LogFile -Value $Text -Encoding UTF8 }
}

# 在 GitHub Actions 里额外写「作业摘要」：失败时**不用下载 artifact、不用仓库写权限**
# 就能在 run 页面直接看到哪个 suite、哪条用例挂了。
# 起因：本地排查 CI 失败时，job log 与 artifact 下载都需要仓库写权限（403/401），
# 只能靠猜 —— 摘要是公开可读的，这条能把「猜」变成「看」。
function Write-GitHubSummary {
    param([object[]]$Results, [string]$Tier)
    if (-not $env:GITHUB_STEP_SUMMARY) { return }
    $bad = @($Results | Where-Object { -not $_.Ok })
    $md = New-Object System.Collections.Generic.List[string]
    $md.Add("## 自检结果（Tier=$Tier）")
    $md.Add('')
    $md.Add("**$($Results.Count) 个 suite：$($Results.Count - $bad.Count) 通过 / $($bad.Count) 失败**")
    $md.Add('')
    $md.Add('| suite | 结果 | passed | exit |')
    $md.Add('|---|---|---|---|')
    foreach ($r in $Results) {
        $mark = if ($r.Ok) { 'PASS' } else { 'FAIL' }
        $md.Add("| $($r.Suite) | $mark | $($r.Passed) | $($r.Exit) |")
    }
    if ($bad.Count -gt 0) {
        $md.Add('')
        $md.Add('### 失败用例')
        foreach ($r in $bad) {
            $md.Add('')
            $md.Add("**$($r.Suite)**（exit=$($r.Exit)）")
            $md.Add('')
            $md.Add('```')
            $fails = @($r.Raw -split "`r?`n" | Where-Object { $_ -match '"ok"\s*:\s*false' })
            if ($fails.Count -gt 0) {
                foreach ($f in $fails) { $md.Add($f.Trim()) }
            } else {
                # 没有结构化失败行（如进程崩溃/加载失败）时给原始尾部，别让摘要空着
                $tail = @($r.Raw -split "`r?`n" | Where-Object { $_.Trim() -ne '' } | Select-Object -Last 15)
                foreach ($t in $tail) { $md.Add($t.Trim()) }
            }
            $md.Add('```')
        }
    }
    Add-Content -Path $env:GITHUB_STEP_SUMMARY -Value $md -Encoding UTF8
}

# ── suite 清单 ────────────────────────────────────────────────────
# logic：纯逻辑，无 GUI / 无驱动 / 无需管理员。
$LogicSuites = @(
    'ScriptActionBuilderSelfTest',
    'ScriptIoSelfTest',
    'ScriptPackageSelfTest',
    'CoordSpaceSelfTest',
    'MacroVariablesSelfTest',
    'ImageMatchSelfTest',
    'OcrSelfTest',
    'AiActionRouterSelfTest',
    'AgentAssistantSelfTest',
    'AgentDesktopTaskSelfTest',
    'AgentMcpSelfTest',
    'OoxmlSelfTest',
    'SqliteSelfTest',
    'WebAiSelfTest',
    'WindowAiSelfTest',
    'AppSettingsStoreSelfTest',
    'ThemeUiSelfTest',
    'BreakoutCooldownSelfTest',
    'HotkeyStopSelfTest',
    'FloatBallGeomSelfTest',
    'OverlayInputGuardSelfTest',
    'HotkeyScopeSelfTest',
    'ClickerTimingSelfTest',
    'ScheduledTaskSelfTest',
    'RecorderSelfTest',
    'BridgeJsonSelfTest',
    'ScriptRunnerSelfTest',
    'BridgeContractSelfTest',
    'ScriptSerializationSelfTest',
    'TimeScaleSelfTest'
)

# interactive：需要桌面会话（窗口模式）/ 已装内核驱动（虚拟 HID）/ 管理员（注入）。
$InteractiveSuites = @(
    'WindowModeSelfTest',
    'VirtualHidSelfTest',
    'InjectionSelfTest'
)

$suites = if ($Tier -eq 'full') { $LogicSuites + $InteractiveSuites } else { $LogicSuites }

# RecorderSelfTest 的输出名刻意避开 Recorder*+SendInput 启发式指纹。
function Get-SuiteExeName([string]$target) {
    if ($target -eq 'RecorderSelfTest') { return 'QstRecorderLogicTest.exe' }
    return "$target.exe"
}

if ($ListOnly) {
    Write-Log "Tier = $Tier ($($suites.Count) suites)"
    foreach ($s in $suites) { Write-Log ("  {0,-28} -> {1}" -f $s, (Get-SuiteExeName $s)) }
    exit 0
}

# ── MSBuild 环境护栏 ──────────────────────────────────────────────
# MSBuild 用大小写不敏感的字典构造子进程环境；当同时存在 https_proxy 与
# HTTPS_PROXY（Windows 常见：系统级 + 工具链各设一份）时会抛
# MSB6001 "已添加项。字典中的关键字:https_proxy"，构建直接失败且报错极难定位。
# 这里在**本进程内**去掉重复的一份（只影响脚本自身，不动系统环境）。
foreach ($name in @('HTTP_PROXY', 'HTTPS_PROXY', 'ALL_PROXY', 'NO_PROXY')) {
    $lower = $name.ToLowerInvariant()
    if ((Test-Path "Env:$name") -and (Test-Path "Env:$lower")) {
        Write-Log "警告：检测到 $lower 与 $name 同时存在（MSBuild 会因此报 MSB6001）；本进程内移除 $name。" 'Yellow'
        Remove-Item "Env:$name"
    }
}

# ── 构建 ──────────────────────────────────────────────────────────
if (-not $SkipBuild) {
    $sln = Join-Path $BuildDir 'QuickScriptTool.sln'
    if (-not (Test-Path $sln)) {
        throw "找不到 $sln。先执行：cmake -S . -B build -G ""Visual Studio 17 2022"" -A x64"
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $msbuild = $null
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
            -property installationPath 2>$null | Select-Object -First 1
        if ($vsPath) { $msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe' }
    }
    if (-not $msbuild -or -not (Test-Path $msbuild)) {
        $msbuild = (Get-Command msbuild -ErrorAction SilentlyContinue).Source
    }
    if (-not $msbuild) { throw '找不到 MSBuild.exe（需要 VS 2022 或 Build Tools）。' }

    Write-Log "==> 构建 $($suites.Count) 个 SelfTest 目标（$Configuration）" 'Cyan'
    # 注意：不要用 /t:A;B 拼多个 target（PowerShell 会把分号拆成多条命令）。
    foreach ($s in $suites) {
        Write-Log "    - $s"
        & $msbuild $sln /p:Configuration=$Configuration /t:$s /m /v:minimal /nologo
        if ($LASTEXITCODE -ne 0) { throw "构建失败：$s" }
    }
}

# ── 运行 ──────────────────────────────────────────────────────────
$results = @()
foreach ($s in $suites) {
    $exe = Join-Path $outDir (Get-SuiteExeName $s)
    if (-not (Test-Path $exe)) {
        $results += [pscustomobject]@{ Suite = $s; Ok = $false; Exit = -1; Passed = 0; Failed = -1; Raw = '' }
        Write-Log ("[SKIP] {0,-28} exe 不存在：{1}" -f $s, $exe) 'Yellow'
        continue
    }

    # 原生程序写 stderr 时，$ErrorActionPreference='Stop' 会把它升级成
    # NativeCommandError 终止脚本；跑测试期间必须放宽。
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $raw = (& $exe --json 2>&1 | Out-String)
    $exit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap

    $passed = 0; $failed = 0
    $lines = @($raw -split "`r?`n" | Where-Object { $_.Trim() -ne '' })
    $lastLine = if ($lines.Count -gt 0) { $lines[-1] } else { '' }
    if ($lastLine -match '"passed"\s*:\s*(\d+)') { $passed = [int]$Matches[1] }
    if ($lastLine -match '"failed"\s*:\s*(\d+)') { $failed = [int]$Matches[1] }

    $ok = ($exit -eq 0)
    $results += [pscustomobject]@{ Suite = $s; Ok = $ok; Exit = $exit; Passed = $passed; Failed = $failed; Raw = $raw }

    if ($ok) {
        Write-Log ("[PASS] {0,-28} passed={1}" -f $s, $passed) 'Green'
    } else {
        Write-Log ("[FAIL] {0,-28} exit={1} failed={2}" -f $s, $exit, $failed) 'Red'
    }
}

# ── 汇总 ──────────────────────────────────────────────────────────
$bad = @($results | Where-Object { -not $_.Ok })
Write-Log ''
Write-Log ("===== 汇总：{0} 个 suite，{1} 通过，{2} 失败（Tier={3}）=====" -f `
    $results.Count, ($results.Count - $bad.Count), $bad.Count, $Tier)
Write-GitHubSummary -Results $results -Tier $Tier

if ($bad.Count -gt 0) {
    Write-Log ''
    foreach ($r in $bad) {
        Write-Log ("----- {0} 原始输出 -----" -f $r.Suite) 'Red'
        if ($r.Raw) { Write-Log $r.Raw }
        $fails = @($r.Raw -split "`r?`n" | Where-Object { $_ -match '"ok"\s*:\s*false' })
        if ($fails.Count -gt 0) {
            Write-Log '  失败用例：'
            foreach ($f in $fails) { Write-Log "    $f" }
        }
    }
    exit $bad.Count
}

exit 0
