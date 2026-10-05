# rollback_audit.ps1 -- audit the "engine must not decide" rollback.
#
# Principle being enforced (docs section 45):
#   the engine only SENSES + ACTS + REPORTS HONESTLY. It never decides for the
#   model, and it never keeps cross-frame world state. Skills only suggest.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\verify\rollback_audit.ps1
#   ... -Strict        # exit 1 if ANY family still has hits (use when all batches done)
#   ... -SkipSuites    # symbol scan only
#
# ASCII only on purpose: Chinese text in a .ps1 without a BOM gets parsed as GBK
# and can eat quotes/braces (see AGENTS.md "core conventions").

param(
    [switch]$Strict,
    [switch]$SkipSuites
)

$ErrorActionPreference = 'Stop'

# Resolve the repo root by WALKING UP until src\engine exists -- never by counting
# directory levels. The first version did Split-Path twice, so running a copy from
# anywhere else made every scan dir invisible; Scan-Family then found zero hits,
# called every family GONE, and the script printed a false "all clear".
$root = $PSScriptRoot
while ($root -and -not (Test-Path (Join-Path $root 'src\engine'))) {
    $parent = Split-Path -Parent $root
    if ([string]::IsNullOrEmpty($parent) -or $parent -eq $root) { break }
    $root = $parent
}
if (-not $root -or -not (Test-Path (Join-Path $root 'src\engine'))) {
    Write-Host "FATAL: repo root not found from $PSScriptRoot -- refusing to report a result" -ForegroundColor Red
    exit 2
}
Set-Location $root
# A scan that reads zero files must never look like a clean result either.
$scanFileCount = @(Get-ChildItem -Path 'src' -Recurse -File -ErrorAction SilentlyContinue).Count
if ($scanFileCount -eq 0) {
    Write-Host 'FATAL: src/ has no files -- refusing to report a result' -ForegroundColor Red
    exit 2
}

# family -> symbols that must be gone, grouped by the batch that removes them
$batchA = @(
    'AiSkipDeadClick', 'AiSkipRepeatSuccess', 'AiDecideNearDupClick',
    'FormatNearDupClickRefusal', 'AiNearDupVerdict', 'AiDecideRepeatRefusal',
    'AiRepeatRefusalVerdict', 'NormalizeLocateTargetKey', 'LocateTargetsEquivalent',
    'AiDecideSelfCorrectClick', 'AiSelfCorrectVerdict', 'AiDecideLocateRetry',
    'AiLocateRetryVerdict', 'aiLocateMemos', 'tryMissSelfCorrect',
    'BuildMissSelfCorrectPrompt', 'AiLocateFailKeyBlock', 'AiNoteLocateFailed',
    'AiLocateTargetScope', 'aiLocateTargetThisBatch'
)
$batchB = @(
    'AiUiLayout', 'AiLocateCache', 'AiTrack', 'AiUiDetectGridPeriod',
    'onLocateGrid', 'PlanFindImageFastPath', 'AcceptFindImageFastPathHit',
    'findImageFastPath', 'ResetFindImageFastPath'
)
$batchC = @(
    'AiInferGridFromSpans', 'BindOcrSpansToGrid', 'AiGridLabelLookup',
    'AiIndexGridSpec', 'kAiGameTrimmedTools', 'AiGameTrimmedToolsMentioned',
    'FormatGameTrimmedToolsNote', 'trimGameIrrelevantTools'
)
# batch D: engine stopping its own decision-making (site heuristics, engine-initiated
# navigation, decision-flavoured refusals, and the three "engine joins the thinking" areas).
$batchD = @(
    'LooksLikeSiteSearchResultsUrl', 'LooksLikeUserSpaceSiteUrl',
    'CheckAiActionPlanGate', 'IsPlanGatedToolName', 'AiActionPlanGateIsOpen',
    'LooksLikeLocateOnlyOutsourcePrompt',
    'kAiLookaheadMaxStartsPerAction',
    'AiVisionLocateHintFor'
)
# batch E: the last same-shaped instance of the mechanism batch C removed -- the engine
# trimming the tool table because the PROMPT TEXT contains a phrase (`只填表`). Same shape
# as the game-foreground trim, and it contradicts "the tool table is always complete now".
$batchE = @(
    'fillTableOnly'
)

# The other half of the audit: things the rollback MUST NOT take with it.
# A deletion batch that over-reaches (removes perception or the honest receipt)
# would otherwise only be caught by the suites -- and engine-side wiring has no
# suite coverage at all. So pin the survivors by name.
$keepPerception = @(
    'FormatOcrTextIndex', 'CollectOcrIndexRows', 'BuildAiElementIndex',
    'AiElementIndexResolve', 'AiOcrLabelMatchTier', 'AiElementIndexPartialMatch',
    'kOcrIndexMaxNumericSpans',            # numeric two-pass collection
    'IsPointInWindowNonClientStrip', 'ProbeWindowNonClientAtPoint'
)
$keepActAndReceipt = @(
    'BuildAiActionExecuteTools', 'AiBatchOutcome', 'AiExecResultLooksUncertain',
    'AiJudgeUiReaction', 'AiFrameClickMark', 'MarkLastAiClickScreenPoint',
    'FormatLastRequestBreakdown'
)
$keepProtocolBrakes = @(
    'AiDecideStreamBrake', 'AiDecideNoToolCallAnswer', 'PlanSpendBudget'
)
# Guards that protect the user's EXISTING state (a document, a window, game progress).
# The user ruled they are deferred: do not delete them together with the decision gates,
# and do not delete them by accident either -- removing one is irreversible in the field.
$keepDeferredGuards = @(
    'blockVisionLanding', 'GuardEscapeInSaveDialog', 'bareTabStreak',
    'saveAsScrollStreak', 'lastHideWindowsSeq', 'pendingSavePath'
)

# code only: docs / AGENTS / .cursor keep the historical narrative on purpose.
# `skills` IS scanned: those files ship to users as the assistant's capability guide, so a
# skill that still teaches a deleted capability is the same defect as stale code (found by
# hand in batch C: skills\agent\game.md still taught `locateAndClick(grid=...)`).
$scanDirs = @('src', 'tools', 'ui', 'cmake', 'tests', 'skills')
$codeExt = '*.cpp', '*.h', '*.hpp', '*.cc', '*.ps1', '*.py', '*.txt', '*.cmake'
# this script carries the symbol list itself -- never count it as a hit
$selfPath = $MyInvocation.MyCommand.Path

function Scan-Family {
    param([string]$name, [string[]]$symbols, [bool]$required)
    $hits = @()
    foreach ($sym in $symbols) {
        foreach ($dir in $scanDirs) {
            if (-not (Test-Path $dir)) { continue }
            $found = Get-ChildItem -Path $dir -Recurse -File -Include $codeExt -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -ne $selfPath } |
                Select-String -SimpleMatch -Pattern $sym -ErrorAction SilentlyContinue
            foreach ($f in $found) {
                $hits += [pscustomobject]@{
                    Symbol = $sym
                    File   = $f.Path.Replace("$root\", '')
                    Line   = $f.LineNumber
                }
            }
        }
    }
    $state = if ($hits.Count -eq 0) { 'GONE' } elseif ($required) { 'LEFT' } else { 'pending' }
    [pscustomobject]@{ Family = $name; State = $state; Hits = $hits.Count; Detail = $hits }
}

function Scan-Keep {
    param([string]$name, [string[]]$symbols)
    $missing = @()
    foreach ($sym in $symbols) {
        $hit = $false
        foreach ($dir in $scanDirs) {
            if (-not (Test-Path $dir)) { continue }
            $found = Get-ChildItem -Path $dir -Recurse -File -Include $codeExt -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -ne $selfPath } |
                Select-String -SimpleMatch -Pattern $sym -ErrorAction SilentlyContinue |
                Select-Object -First 1
            if ($found) { $hit = $true; break }
        }
        if (-not $hit) { $missing += $sym }
    }
    [pscustomobject]@{
        Family = $name
        State  = if ($missing.Count -eq 0) { 'INTACT' } else { 'LOST' }
        Hits   = $missing.Count
        Detail = $missing
    }
}

Write-Host '=== rollback audit: engine must not decide ===' -ForegroundColor Cyan
$results = @(
    Scan-Family 'batch A (veto gates)'   $batchA $true
    Scan-Family 'batch B (state tables)' $batchB $true
    Scan-Family 'batch C (grid/trim)' $batchC $true
    Scan-Family 'batch D (engine decides)' $batchD $true
    Scan-Family 'batch E (prompt-trim)' $batchE $true
)

$keeps = @(
    Scan-Keep 'keep: perception'  $keepPerception
    Scan-Keep 'keep: act+receipt' $keepActAndReceipt
    Scan-Keep 'keep: protocol brakes' $keepProtocolBrakes
    Scan-Keep 'keep: deferred guards' $keepDeferredGuards
)

$results | Select-Object Family, State, Hits | Format-Table -AutoSize
$keeps   | Select-Object Family, State, Hits | Format-Table -AutoSize

# @() everywhere: in PS 5.1 a Where-Object result that matches exactly ONE item
# collapses to a scalar, and `[pscustomobject].Count` is EMPTY -- so
# `if ($lost.Count -gt 0)` was silently always-false for the one-group case and
# the FAIL branch below never ran. Measured, not guessed.
$lost     = @($keeps   | Where-Object { $_.State -eq 'LOST' })
foreach ($k in $lost) {
    Write-Host "--- $($k.Family): LOST $($k.Hits) symbol(s) that must survive:" -ForegroundColor Red
    $k.Detail | ForEach-Object { Write-Host "    $_" }
}

$leftover = @($results | Where-Object { $_.State -eq 'LEFT' })
foreach ($r in $leftover) {
    Write-Host "--- $($r.Family): $($r.Hits) hit(s)" -ForegroundColor Red
    $r.Detail | Select-Object -First 40 | ForEach-Object {
        Write-Host ("    {0}  {1}:{2}" -f $_.Symbol, $_.File, $_.Line)
    }
}
$pending = @($results | Where-Object { $_.State -eq 'pending' })
foreach ($r in $pending) {
    Write-Host "--- $($r.Family): still present ($($r.Hits) hit(s)) -- expected until that batch lands" -ForegroundColor DarkYellow
}

if (-not $SkipSuites) {
    Write-Host ''
    Write-Host '=== all logic self-tests ===' -ForegroundColor Cyan
    Remove-Item Env:HTTPS_PROXY -ErrorAction SilentlyContinue
    Remove-Item Env:https_proxy -ErrorAction SilentlyContinue
    $log = 'build\selftest_audit.log'
    & powershell -ExecutionPolicy Bypass -File tools\run_all_selftests.ps1 -LogPath $log | Select-Object -Last 4
    $suiteExit = $LASTEXITCODE
    Write-Host "run_all_selftests exit=$suiteExit (log: $log)"
}

Write-Host ''
Write-Host '=== player template parity ===' -ForegroundColor Cyan
$a = 'build\Release\QstPlayer.exe'
$b = 'build\Release\tools\player\QstPlayer.exe'
if ((Test-Path $a) -and (Test-Path $b)) {
    $sa = (Get-Item $a).Length; $sb = (Get-Item $b).Length
    if ($sa -eq $sb) { Write-Host "OK  both $sa bytes" }
    else { Write-Host "MISMATCH  $sa vs $sb" -ForegroundColor Red }
} else {
    Write-Host 'SKIP  player not built' -ForegroundColor DarkYellow
}

Write-Host ''
# The summary line must not be able to lie: it is derived from the same $leftover
# the table above was printed from. (First version printed "no required family
# left" unconditionally -- i.e. it always claimed success when run without -Strict.)
if ($lost.Count -gt 0) {
    Write-Host "RESULT: FAIL -- the rollback took $($lost.Count) kept group(s) with it" -ForegroundColor Red
    exit 1
}
if ($leftover.Count -gt 0) {
    if ($Strict) {
        Write-Host "RESULT: FAIL ($($leftover.Count) required family/families still present)" -ForegroundColor Red
        exit 1
    }
    Write-Host "RESULT: INCOMPLETE ($($leftover.Count) required family/families still present; -Strict would fail)" -ForegroundColor Yellow
    exit 0
}
Write-Host 'RESULT: all required families gone' -ForegroundColor Green
exit 0
