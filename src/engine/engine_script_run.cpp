// engine_script_run.cpp — F2 slice extracted from engine_host_window.h
#include "engine/engine_host_window.h"
#include "action_utils.h"
#include "agent_ui_notify.h"
#include "ai_action_service.h"
#include "ai_fast_paths.h"
#include "ai_locate_verify.h"
#include "ai_logic_convert.h"
#include "color_match.h"
#include "desktop_tools/desktop_tools.h"
#include "engine/hotkey_scope.h"
#include "image_var_util.h"
#include "low_power_mode.h"
#include "macro_execute_tools.h"
#include "ocr_engine.h"
// `kAgentAttachmentJpegQuality` / `kAgentAttachmentMaxLongEdge`：`zoom` 的交付图必须**一次**
// 就编成送图链路的形态（同一套尺寸规则 + 同一个 JPEG 质量）⇒ 磁盘上的字节数就是模型拿到的
// 字节数，回执不可能说谎。**一处事实一份逻辑** —— 别在这里另写 82/1280。
#include "agent_attachment.h"
#include "opencv_runtime.h"
#include "script_io.h"
#include "var_compute.h"
#include "window_mode/ui_element_probe.h"
#include "window_mode/window_list.h"
#include "window_mode/window_capture.h"
#include "window_mode/window_capture_wgc.h"
#include "page_snapshot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

static std::wstring ResolveNestedLibraryTarget(const std::wstring& targetPath,
    const std::wstring& blockName) {    std::wstring resolved;
    if (!targetPath.empty() && ResolveLibraryScriptPath(targetPath, resolved))
        return resolved;
    if (!blockName.empty() && ResolveLibraryScriptPath(blockName, resolved))
        return resolved;
    return {};
}

// ── zoom 放大图的落地目录：**唯一文件名** + 有界清理 ────────────────────────
// 两个纯工具函数都在 `macro_execute_tools.cpp`（`FormatAiZoomTempPath` /
// `PruneAiZoomTempDir`）—— 放那边是为了**能被自检直接钉住**（文件名的唯一性与
// 「按年龄清旧图」的 FILETIME 算法都属于「算错了会静默出事」的那一类）。

// ── 「文字直点」用的本地 OCR 屏幕文字索引 ────────────────────────────────
// 与观察帧同源：观察时本来就要跑一次本地 OCR（给模型看「屏幕上哪段文字在哪」），
// 这里**留一份带坐标的原始行表**，让 locateAndClick 能对「纯短文本」目标直接落点。
// 实测一次「点个按钮」= 整屏识图 + Zoom 精炼两轮 VLM ≈ 20s，而这张表早就有坐标了。
// 有效期很短（画面一变坐标就废）：过期即回落识图，绝不拿旧坐标硬点。
namespace {

constexpr long long kOcrScreenIndexFreshMs = 90000;

struct OcrScreenIndexState {
    std::vector<OcrTextLine> lines;
    long long stampMs = 0;
    int capX1 = 0, capY1 = 0, capX2 = 0, capY2 = 0;
    /// 索引对应的前台窗口：窗口换了，这些字坐标立刻作废（哪怕还在 6s 内）
    HWND hwnd = nullptr;
};

OcrScreenIndexState& OcrScreenIndex() {
    static OcrScreenIndexState s;
    return s;
}

/// ★★AI 批次里**相邻两次输入之间的最小节拍**（docs §58）。
/// 实测（真机日志）：一轮 10 次「选卡 → 落点」只成功 **9** 次；一轮 5 次只成功 **3** 次。
/// 模型自己也算出来了（`spent 1575 = 9×175，不是 10`）却无从解释 —— 因为**回执没错、坐标也没错**，
/// 错的是**节拍**：AI 批次没有绝对时间轴，循环全速跑 ⇒ 两次点击相隔 ~1ms；而游戏按**帧**读输入
/// （30~60fps = 16~33ms），落在同一帧里的两次事件只会被看见一次
/// ⇒ 「选卡」与「落点」同帧时，那一次投放**静默丢失**。
/// ⚠ `MouseInputRouter::PaceLocked()` 帮不上忙：它的设计目标是**贴回放时间轴**
/// （`catchUpGapUs_==0` 时「永不垫间隔，迟到就连发追赶」），对没有时间轴的 AI 批次等于不设防。
/// ⚠ 这是**输入保真度**，不是策略：它不决定做什么，只保证每个动作真的被目标看见。
/// 50ms ≥ 20fps 的一帧；20 步的批次只多花 ~1s，换来的是「点了几次就真的落地几次」。
constexpr int kAiBatchStepMinGapMs = 50;

void StoreOcrScreenIndex(const OcrEngineOutput& ocr, int cx1, int cy1, int cx2, int cy2) {
    auto& s = OcrScreenIndex();
    if (!ocr.success || ocr.lines.empty()) {
        // 这次没识别出东西 → 旧表立刻作废（宁可回落识图，也不用过期坐标）
        s.lines.clear();
        s.stampMs = 0;
        return;
    }
    s.lines = ocr.lines;
    s.stampMs = static_cast<long long>(GetTickCount64());
    s.capX1 = cx1;
    s.capY1 = cy1;
    s.capX2 = cx2;
    s.capY2 = cy2;
    s.hwnd = GetForegroundWindow();
}

void ResetOcrScreenIndex() {
    auto& s = OcrScreenIndex();
    s.lines.clear();
    s.stampMs = 0;
}

/// 画面与「OCR 索引那一帧」基本一致 → 索引里的字坐标依然成立，给它续期。
/// 否则静态菜单上连续点几个文字按钮时，6s 一过就又掉回「识图那套」。
void TouchOcrScreenIndex() {
    auto& s = OcrScreenIndex();
    if (!s.lines.empty()) s.stampMs = static_cast<long long>(GetTickCount64());
}

/// ★★「上一帧 OCR 行」缓存（未变帧复用用）——**提到文件作用域**是为了让
///   `observeScreenForAgent` 的**未变帧提前返回**能问一句"到底有没有东西可复用"。
///
///   2026-10-02 真机事故（Web 反代路）：界面一直"结构安静"（模型还没成功动作过），
///   于是每轮都在提前返回处 `return r;` —— **整段 OCR / 元素索引构建被跳过**，
///   而那份索引本来就不存在 ⇒ `obs.textIndex/elementIndex` 永远为空 ⇒
///   模型手里既没有新帧也没有可点清单，只能盯着第一张旧图空转
///   （日志形态：`可点清单尚未建立 …` 与 `界面未变但本轮尚未执行任何动作` 交替刷屏）。
///   ⇒ 判据：**没有可复用的 OCR 行时不许走提前返回**（这一轮必须真的把索引建起来）。
std::vector<OcrTextLine>& LastOcrLinesCache() {
    static thread_local std::vector<OcrTextLine> s;
    return s;
}

thread_local int g_lastOcrCx1 = 0, g_lastOcrCy1 = 0, g_lastOcrCx2 = 0, g_lastOcrCy2 = 0;
thread_local int g_ocrReuseStreak = 0;

namespace {

/// 回执里「这一击落在哪」的**如实描述**。
///
/// ★★模型自己的坐标系是 **upload 截图像素**（`mouseClick` / `locateAndClick` 的入参、
/// 元素索引/文字索引给的那一对）—— 所以第一套必须是它。旧文案把「屏幕绝对像素」
/// 和「computer 归一化」摆在最前面，末尾还补一句「这两个**都不是**你要的坐标」，
/// 却**始终没给**那个真的坐标：模型要照着调位置只能自己去除以 2.5（实测它就那么在算）。
/// 回执必须描述**消费者要的那个东西**（§51 的同一形状）。
std::wstring DescribeClickPointForModel(int x, int y, const AiCaptureMapping* map) {
    const int sw = (std::max)(1, GetSystemMetrics(SM_CXSCREEN));
    const int sh = (std::max)(1, GetSystemMetrics(SM_CYSCREEN));
    std::wstring uploadPart;
    if (map && map->apiWidth > 0 && map->apiHeight > 0
        && map->capX2 > map->capX1 && map->capY2 > map->capY1) {
        int ux = 0, uy = 0;
        MapScreenPointToApi(*map, x, y, ux, uy);
        uploadPart = L"upload 截图像素(" + std::to_wstring(ux) + L"," + std::to_wstring(uy) + L")";
    } else {
        // 算不出来就如实说算不出来（别拿屏幕像素冒充）
        uploadPart = L"upload 截图像素(**算不出来**：本帧没有可用的截取映射；要按坐标点"
                     L"先 computer(action=screenshot) 看一轮)";
    }
    return uploadPart + L"｜屏幕绝对像素(" + std::to_wstring(x) + L"," + std::to_wstring(y)
        + L")｜computer 归一化(" + std::to_wstring(x * 1000 / sw) + L","
        + std::to_wstring(y * 1000 / sh)
        + L")（★ `mouseClick` / `locateAndClick` 收的是**第一套 upload 像素**；"
          L"另两套只是同一处的另外两种叫法）";
}

}  // namespace

}  // namespace

struct NestedModeFrame {
    windowmode::WindowModeScriptConfig cfg;
    bool wasActive = false;
    double breakoutTime = 0;
    std::wstring launchPath;
};

static void MergeNestedWindowRelative(windowmode::WindowModeScriptConfig& cfg,
    const ScriptFileData& nested) {
    bool anyRel = cfg.windowRelativeCoordinates || nested.windowMode.windowRelativeCoordinates;
    for (const auto& act : nested.actions) {
        if (act.windowRelative) { anyRel = true; break; }
    }
    if (!anyRel) return;
    windowmode::FinalizeWindowModeForPlayback(cfg, true, false);
    if (cfg.recordClientWidth <= 0 && nested.windowMode.recordClientWidth > 0) {
        cfg.recordClientWidth = nested.windowMode.recordClientWidth;
        cfg.recordClientHeight = nested.windowMode.recordClientHeight;
    }
}

// 合成桌面区域采集（微信截图同原理）：
// GDI BitBlt+CAPTUREBLT 拍不到 DirectComposition 表面（TSF 输入法候选框/组字框
// 是独立合成层），WGC 整屏采集的是 DWM 合成后的完整桌面，能拍到输入法状态。
// 优先 WGC 整屏后裁剪到目标区域；WGC 不可用/失败时回退 GDI BitBlt。
// cx1..cy2 为屏幕坐标区域。
static std::string JsonEscapeUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

static HBITMAP CaptureAiRegionComposed(int cx1, int cy1, int cx2, int cy2) {
    HBITMAP bmp = nullptr;
    bool usedWgc = false;
    if (windowmode::IsWgcCaptureAvailable()) {
        const int cxm = (cx1 + cx2) / 2;
        const int cym = (cy1 + cy2) / 2;
        HMONITOR hMon = MonitorFromPoint({ cxm, cym }, MONITOR_DEFAULTTONEAREST);
        if (hMon) {
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfoW(hMon, &mi)) {
                int mw = 0, mh = 0;
                HBITMAP full = windowmode::CaptureMonitorWgc(hMon, mw, mh);
                if (full) {
                    const int rx1 = std::max(cx1, static_cast<int>(mi.rcMonitor.left));
                    const int ry1 = std::max(cy1, static_cast<int>(mi.rcMonitor.top));
                    const int rx2 = std::min(cx2, static_cast<int>(mi.rcMonitor.right));
                    const int ry2 = std::min(cy2, static_cast<int>(mi.rcMonitor.bottom));
                    if (rx2 > rx1 && ry2 > ry1) {
                        bmp = windowmode::CropBitmapScreenRegion(full,
                            mi.rcMonitor.left, mi.rcMonitor.top, rx1, ry1, rx2, ry2);
                        usedWgc = (bmp != nullptr);
                    }
                    DeleteBitmapHandle(full);
                }
            }
        }
    }
    if (!bmp) bmp = CaptureScreenRegion(cx1, cy1, cx2, cy2);
    static std::once_flag diagOnce;
    std::call_once(diagOnce, [usedWgc] {
        if (qst::desktop_tools::MacroDebug().IsCreated()) {
            qst::desktop_tools::MacroDebug().AppendLog(
                usedWgc ? L"  [诊断] 观察帧：合成桌面采集(WGC)已启用（微信同原理，能拍到输入法组字框/候选框）"
                        : L"  [诊断] 观察帧：WGC 不可用，回退 GDI BitBlt 采集");
        }
    });
    return bmp;
}

// was engine_host_window.h:11614-11621
void EngineHost::RunScriptByIndex(int index) {
        if (index < 0 || index >= static_cast<int>(scripts_.size())) return;
        const std::wstring path = scripts_[static_cast<size_t>(index)].path;
        // 编辑器正在编辑同一文件：用内存动作（含未保存修改）+ 已有 currentPath_
        if (page_ == Page::Editor && !currentPath_.empty()
            && _wcsicmp(path.c_str(), currentPath_.c_str()) == 0) {
            RunCurrentActions();
            return;
        }
        // 热键/主页：必须带磁盘路径启动，否则逻辑转化写回拿到空路径会静默跳过
        // （LoadScriptFile 不会设置 currentPath_，旧逻辑 RunCurrentActions 会传空 selfPath）
        RunActionsFromPath(path);
    }

void EngineHost::RunRecordingByIndex(int index) {
        if (index < 0 || index >= static_cast<int>(recordings_.size())) return;
        const std::wstring path = recordings_[static_cast<size_t>(index)].path;
        if (page_ == Page::Editor && !currentPath_.empty()
            && _wcsicmp(path.c_str(), currentPath_.c_str()) == 0) {
            RunCurrentActions();
            return;
        }
        RunActionsFromPath(path);
    }

bool EngineHost::HotkeyChordConflicts(UINT vk, UINT modifiers, const std::wstring& excludePath,
    bool excludeGlobal, std::wstring& errOut) const {
        errOut.clear();
        if (!vk) return false;
        auto sameChord = [&](const Hotkey& hk) {
            return hk.enabled && hk.vk == vk && hk.modifiers == modifiers;
        };
        if (!excludeGlobal && sameChord(globalHotkey_)) {
            errOut = L"与全局启停热键冲突";
            return true;
        }
        for (const auto& s : scripts_) {
            if (!excludePath.empty() && _wcsicmp(s.path.c_str(), excludePath.c_str()) == 0) continue;
            if (sameChord(s.hotkey)) {
                errOut = L"与脚本「" + s.name + L"」热键冲突";
                return true;
            }
        }
        for (const auto& r : recordings_) {
            if (!excludePath.empty() && _wcsicmp(r.path.c_str(), excludePath.c_str()) == 0) continue;
            if (sameChord(r.hotkey)) {
                errOut = L"与录制「" + r.name + L"」热键冲突";
                return true;
            }
        }
        return false;
    }

bool EngineHost::DedicatedHotkeyInScope(bool isRecording) const {
    // 判据抽到 engine/hotkey_scope.h —— 产品与 HotkeyScopeSelfTest 共用一份，
    // 避免「测试测的是副本」导致口径漂移。
    return qst::hotkey_scope::DedicatedInScope(
        homeHotkeyScopeAll_, isRecording,
        activeHomeTab_ == quickscript::MainTab::Macro,
        activeHomeTab_ == quickscript::MainTab::Recorder);
}

bool EngineHost::GlobalHotkeyConflicts(UINT vk, UINT modifiers) const {
    if (!vk) return false;
    return globalHotkey_.enabled && globalHotkey_.vk == vk
        && globalHotkey_.modifiers == modifiers;
}

void EngineHost::CollectDedicatedHotkeyOwners(UINT vk, UINT modifiers,
    const std::wstring& excludePath, std::vector<std::wstring>& out) const {
    out.clear();
    if (!vk) return;
    auto sameChord = [&](const Hotkey& hk) {
        return hk.enabled && hk.vk == vk && hk.modifiers == modifiers;
    };
    for (const auto& s : scripts_) {
        if (!excludePath.empty() && _wcsicmp(s.path.c_str(), excludePath.c_str()) == 0) continue;
        if (sameChord(s.hotkey)) out.push_back(s.path);
    }
    for (const auto& r : recordings_) {
        if (!excludePath.empty() && _wcsicmp(r.path.c_str(), excludePath.c_str()) == 0) continue;
        if (sameChord(r.hotkey)) out.push_back(r.path);
    }
}

// was engine_host_window.h:12346-12387
void EngineHost::RunCurrentActions() {
        if (running_) {
            // 避免「点了没反应」：正在运行时再次点击视为请求停止。
            StopRun();
            if (qst::desktop_tools::MacroDebug().IsCreated()) {
                qst::desktop_tools::MacroDebug().AppendLog(L"脚本正在运行，已请求停止");
            }
            return;
        }
        SyncFormIntoActionsBeforeRun();
        EnsureEditorFullyParsed();
        windowmode::WindowModeScriptConfig runCfg{};
        if (!PrepareWindowModeRunConfig(runCfg)) return;
        // 不把启动时解析出的临时窗口身份写回编辑器，避免污染「选择窗口方式」与保存内容。
        // 有路径且允许自动打开时，不要因为“当前没有窗口”而拦截运行——真正打开交给 BeginRun。
        if (runCfg.enabled && appSettings_.windowMode.blockRunWhenUnhealthy
            && !windowmode::ShouldAutoLaunchTarget(runCfg)) {
            std::wstring err;
            if (!windowmode::WindowModeExecutor::CheckRunHealth(runCfg, err)) {
                RestoreMainWindowForUser();
                promptModal_.ShowInfo(err.empty() ? L"窗口/后台窗口模式未就绪" : err);
                return;
            }
        }

        CoordMeta execMeta = ScriptCoordMetaForExecution(loadedCoordMeta_);
        std::vector<ScriptAction> execActions = actions_;
        SyncNormFieldsFromPixels(execActions,
            CaptureCurrentCoordMeta(runCfg.enabled ? &runCfg : nullptr));
        std::wstring keyBalanceWarn;
        execActions = PrepareScriptActionsForExecution(execActions, execMeta, &keyBalanceWarn);
        // 录制按键不平衡（只有按下没松开）会在这里补上抬起并打日志 ——
        // 现场（用户 10-01）：后台回放 `→ 按下=7 松开=6` 导致角色一路往一个方向漂移。
        if (!keyBalanceWarn.empty()) {
            AppendDebugLog(keyBalanceWarn);
            qst::desktop_tools::AppendRecorderDiagLog(keyBalanceWarn);
        }
        // 旧录制里相对移动间隔可能被 Raw 积压压成 0~1ms；回放前按设备报告间隔拉开。
        if (IsRecordingScriptPath(currentPath_) || ScriptIsTimedInputSequence(execActions))
            PreparePlaybackTimeline(execActions, appSettings_.playback.spreadRelativeMovePackets);

        const double breakoutTime = runCfg.enabled ? 0.0 : ParseBreakoutTimeFromEditor();
        Hotkey scriptHotkey{};
        for (const auto& s : scripts_) {
            if (_wcsicmp(s.path.c_str(), currentPath_.c_str()) == 0) {
                scriptHotkey = s.hotkey;
                break;
            }
        }
        if (!scriptHotkey.enabled || !scriptHotkey.vk) {
            for (const auto& r : recordings_) {
                if (_wcsicmp(r.path.c_str(), currentPath_.c_str()) == 0) {
                    scriptHotkey = r.hotkey;
                    break;
                }
            }
        }
        StartActionsWorker(execActions, currentPath_, runCfg, execMeta, breakoutTime,
            scriptHotkey);
    }

// 编辑器调试：从指定动作开始单次执行（与保存/热键同一套窗口模式；不受「宏执行次数」设置影响）
bool EngineHost::EngineDebugRunActions(const std::vector<ScriptAction>& actions, int startIndex,
    bool stepMode, const std::vector<int>& breakpoints,
    const Hotkey& debugHotkey, const std::wstring& displayName,
    const windowmode::WindowModeScriptConfig& wmCfgIn, std::wstring& err) {
        if (running_) {
            StopRun();
            err = L"脚本正在运行，已请求停止";
            return false;
        }
        if (actions.empty()) {
            err = L"没有可调试的动作";
            return false;
        }
        if (startIndex < 0 || startIndex >= static_cast<int>(actions.size())) {
            err = L"调试起点超出动作列表";
            return false;
        }
        for (int b : breakpoints) {
            if (b < 0 || b >= static_cast<int>(actions.size())) {
                err = L"断点序号超出动作列表";
                return false;
            }
        }
        // 起点位于容器体内时（defineBlock/watchImage/Loop/If/Else），直接从中部执行会丢失容器
        // 上下文（块体被主流程跳过、循环变量/条件未建立、EndLoop/Goto 可能逃逸）。
        // 统一追溯到最外层祖先容器并从其绝对序号开始；主流程跳过的顶层容器（定义块/找图监视）
        // 在调试中按流程执行一次（嵌套块继续向上追溯）。Else 的容器归属其所属 If。
        debugBlockEntrySet_.clear();
        if (actions[static_cast<size_t>(startIndex)].type == ActionType::Else) {
            // 起点本身是 Else（结构性动作）：归属到所属 If 开始，让分支按条件执行
            for (int k = startIndex - 1; k >= 0; --k) {
                if (actions[static_cast<size_t>(k)].type == ActionType::If
                    && actions[static_cast<size_t>(k)].indent
                        == actions[static_cast<size_t>(startIndex)].indent) {
                    startIndex = k;
                    break;
                }
            }
        }
        int ancestorCursor = startIndex;
        int outer = -1;
        int outerIndent = INT_MAX;
        for (;;) {
            if (SkipsInMainFlow(actions[static_cast<size_t>(ancestorCursor)].type)) {
                debugBlockEntrySet_.insert(ancestorCursor);
            }
            const auto& acur = actions[static_cast<size_t>(ancestorCursor)];
            const int directParent = FindDirectParentIndex(
                actions, static_cast<size_t>(ancestorCursor), acur.indent);
            if (directParent < 0) break;
            int effParent = directParent;
            const auto& aDirect = actions[static_cast<size_t>(directParent)];
            if (aDirect.type == ActionType::Else) {
                // Else 自身在主流程中会被跳过：归属到同一缩进最近的 If
                for (int k = directParent - 1; k >= 0; --k) {
                    if (actions[static_cast<size_t>(k)].type == ActionType::If
                        && actions[static_cast<size_t>(k)].indent == aDirect.indent) {
                        effParent = k;
                        break;
                    }
                }
            }
            if (SkipsInMainFlow(actions[static_cast<size_t>(effParent)].type)) {
                debugBlockEntrySet_.insert(effParent);
            }
            if (actions[static_cast<size_t>(effParent)].indent < outerIndent) {
                outer = effParent;
                outerIndent = actions[static_cast<size_t>(effParent)].indent;
            }
            ancestorCursor = effParent;
        }
        if (outer >= 0) {
            startIndex = outer;
        }
        // 编辑器动作是当前屏幕像素坐标：与正常执行一致做「归一化→反归一化」，
        // 让 findImage 偏移等 n* 字段就绪（当前屏幕不变时是恒等变换）。
        const CoordMeta captureMeta = CaptureCurrentCoordMeta(nullptr);
        CoordMeta execMeta = ScriptCoordMetaForExecution(captureMeta);
        std::vector<ScriptAction> execActions = actions;
        SyncNormFieldsFromPixels(execActions, captureMeta);
        std::wstring keyBalanceWarn;
        execActions = PrepareScriptActionsForExecution(execActions, execMeta, &keyBalanceWarn);
        if (!keyBalanceWarn.empty()) {
            AppendDebugLog(keyBalanceWarn);
            qst::desktop_tools::AppendRecorderDiagLog(keyBalanceWarn);
        }
        if (IsRecordingScriptPath(displayName) || ScriptIsTimedInputSequence(execActions)) {
            PreparePlaybackTimeline(execActions, appSettings_.playback.spreadRelativeMovePackets);
        }
        windowmode::WindowModeScriptConfig wmCfg = wmCfgIn;
        bool anyRel = wmCfg.windowRelativeCoordinates;
        for (const auto& a : execActions) {
            if (a.windowRelative) { anyRel = true; break; }
        }
        windowmode::FinalizeWindowModeForPlayback(wmCfg, anyRel, false);
        if (!ResolveWindowModeSelectMethod(wmCfg)) {
            RestoreMainWindowForUser();
            err = L"窗口/后台窗口模式未能绑定目标窗口";
            return false;
        }
        wmCfg.autoLaunchTarget = windowmode::ShouldAutoLaunchTarget(wmCfg);
        StartActionsWorker(execActions, displayName, wmCfg, execMeta, 0.0, Hotkey{},
            startIndex, stepMode, &breakpoints, debugHotkey);
        return true;
    }

// was engine_host_window.h:14229-14235
void EngineHost::StopRun() {
        if (scheduledInterruptStop_) {
            scheduledInterruptStop_ = false;  // 本次停止来自「定时脚本优先」，保留排队
        } else {
            pendingScheduledPaths_.clear();
            ClearScheduledYield();
        }
        stopFlag_ = true;
        ghEmergencyStop.store(true, std::memory_order_release);
        aiHttpAbort_.Abort();
        // 扩展桥可能卡在 WS/CDP 等待：先 Abort 再清闩锁，保证热键能强行中止。
        windowmode::WindowModeExecutor::NotifyCancel();
        // ★★ 必须**马上**把闩锁放掉（这里 Abort 只是为了打断「本次」在途等待）。
        //   此前只在 WindowModeExecutor::EndRun() 里清，而 EndRun 只在**窗口模式会话真的开了**
        //   的时候才跑 ⇒ 若用户跑的是「窗口模式关闭」的脚本，StopRun 置位后无人清，
        //   闩锁**永久为 true** ⇒ 常开桥的 HandleClient/RequestOnSock 见 abort_ 就立刻失败，
        //   于是「只要停过一次脚本，桥就永久变聋」：新连接 TCP 连得上但被直接重置、
        //   且不留任何日志（症状与「扩展没连上」一模一样）。这个坑已在 ext_bridge_server.cpp
        //   的 HandleClient 处补了第二道防线（那里不再看 abort_），这里是第一道。
        //   ⚠ 顺序不能反：必须在 NotifyCancel() **之后**清，否则打断不了在途等待。
        windowmode::ExtBridgeServer::Instance().ClearAbort();
        ClearToggleHotkeyLatches();
    }

void EngineHost::EnqueueScheduledPath(const std::wstring& path) {
        if (path.empty()) return;
        for (const auto& existing : pendingScheduledPaths_) {
            if (_wcsicmp(existing.c_str(), path.c_str()) == 0) return;
        }
        if (pendingScheduledPaths_.size() >= 16) return;
        pendingScheduledPaths_.push_back(path);
    }

void EngineHost::TryStartPendingScheduled() {
        while (!running_ && !pendingScheduledPaths_.empty()) {
            const std::wstring path = pendingScheduledPaths_.front();
            pendingScheduledPaths_.erase(pendingScheduledPaths_.begin());
            nextRunFromScheduled_ = true;
            RunActionsFromPath(path);
            if (running_) return;
            nextRunFromScheduled_ = false;
            runningFromScheduled_ = false;
        }
    }

void EngineHost::RequestScheduledYield(const std::wstring& path) {
        if (path.empty()) return;
        std::lock_guard<std::mutex> lock(scheduledYieldMu_);
        // 已有待插入的定时：保留先到的，避免同秒第二条把路径盖掉
        if (scheduledYieldRequested_.load(std::memory_order_relaxed)
            && !scheduledYieldPath_.empty()) {
            return;
        }
        scheduledYieldPath_ = path;
        scheduledYieldRequested_.store(true, std::memory_order_release);
    }

bool EngineHost::TakeScheduledYieldPath(std::wstring& out) {
        if (!scheduledYieldRequested_.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lock(scheduledYieldMu_);
        if (!scheduledYieldRequested_.load(std::memory_order_relaxed)) return false;
        out = std::move(scheduledYieldPath_);
        scheduledYieldPath_.clear();
        scheduledYieldRequested_.store(false, std::memory_order_relaxed);
        return !out.empty();
    }

void EngineHost::ClearScheduledYield() {
        std::lock_guard<std::mutex> lock(scheduledYieldMu_);
        scheduledYieldPath_.clear();
        deferredScheduledAfter_.clear();
        scheduledYieldRequested_.store(false, std::memory_order_relaxed);
    }

void EngineHost::DeferScheduledPath(const std::wstring& path) {
        if (path.empty()) return;
        std::lock_guard<std::mutex> lock(scheduledYieldMu_);
        if (deferredScheduledAfter_.empty()) {
            deferredScheduledAfter_ = path;
            return;
        }
        if (_wcsicmp(deferredScheduledAfter_.c_str(), path.c_str()) == 0) return;
    }

std::wstring EngineHost::TakeDeferredScheduledPath() {
        std::lock_guard<std::mutex> lock(scheduledYieldMu_);
        std::wstring out = std::move(deferredScheduledAfter_);
        deferredScheduledAfter_.clear();
        return out;
    }

void EngineHost::OnScheduledTaskFire(const std::wstring& path) {
        if (path.empty()) return;
        if (running_ && workerFinished_.load(std::memory_order_relaxed)) OnRunDone();
        const bool busy = running_ || extRunPending_.load(std::memory_order_relaxed);
        const auto policy = ClampScheduledTaskConflictPolicy(
            appSettings_.playback.scheduledTaskConflictPolicy);
        const bool runningIsScheduled = runningFromScheduled_
            || scheduledYieldActive_.load(std::memory_order_relaxed)
            || scheduledYieldRequested_.load(std::memory_order_relaxed);
        const bool autoResume = appSettings_.playback.scheduledTaskAutoResume;
        const auto action = DecideScheduledTaskFire(policy, busy, runningIsScheduled, autoResume);
        auto dbg = [&](const std::wstring& msg) {
            windowmode::WindowModeLog(msg);
            if (qst::desktop_tools::MacroDebug().IsCreated()) {
                qst::desktop_tools::MacroDebug().AppendLog(msg);
            }
        };
        const std::wstring name = [&]() {
            const auto slash = path.find_last_of(L"\\/");
            return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
        }();
        switch (action) {
        case ScheduledTaskFireAction::Skip:
            dbg(L"定时任务跳过（当前脚本未结束）：" + name);
            return;
        case ScheduledTaskFireAction::RunNow:
            dbg(L"定时任务触发：" + name + L" → " + path);
            if (!pendingScheduledPaths_.empty()) {
                EnqueueScheduledPath(path);
                TryStartPendingScheduled();
                return;
            }
            nextRunFromScheduled_ = true;
            RunActionsFromPath(path);
            if (!running_) {
                nextRunFromScheduled_ = false;
                runningFromScheduled_ = false;
            }
            return;
        case ScheduledTaskFireAction::InterruptAndQueue:
            EnqueueScheduledPath(path);
            dbg(L"定时脚本优先：已请求停止当前脚本，随后执行 " + name);
            if (running_) {
                scheduledInterruptStop_ = true;
                StopRun();
            } else {
                TryStartPendingScheduled();
            }
            return;
        case ScheduledTaskFireAction::Queue:
            EnqueueScheduledPath(path);
            dbg(L"当前脚本结束后将执行定时：" + name);
            if (!running_) TryStartPendingScheduled();
            return;
        case ScheduledTaskFireAction::YieldAndResume:
            RequestScheduledYield(path);
            dbg(L"定时插入：暂停当前步骤，执行 " + name + L" 后继续");
            if (!running_ || workerFinished_.load(std::memory_order_relaxed)) {
                ClearScheduledYield();
                EnqueueScheduledPath(path);
                if (!running_) TryStartPendingScheduled();
            }
            return;
        default:
            return;
        }
    }

// was engine_host_window.h:12323-12337
void EngineHost::SyncFormIntoActionsBeforeRun() {
        if (page_ != Page::Editor) return;
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(actions_.size())) return;
        const ScriptAction& existing = actions_[static_cast<size_t>(selectedIndex_)];
        if (ComboSelForType(existing.type) != popupAction_.sel) return;
        ScriptAction action = ActionFromForm();
        if (action.type == ActionType::DefineBlock && !IsValidBlockName(action.blockName)) return;
        if (action.type == ActionType::EndLoop
            && !HasLoopParentAt(actions_, static_cast<size_t>(selectedIndex_), existing.indent)) {
            return;
        }
        action.originalNo = existing.originalNo;
        action.indent = existing.indent;
        actions_[static_cast<size_t>(selectedIndex_)] = action;
    }

// was engine_host_window.h:12341-14179
/// ★★ **确保「任务所在的窗口」在前台**（2026-09-30 抽成共享 helper）。
///
/// 为什么需要（两类真机事故，症状都是"模型像瞎了一样"）：
///   ① **前台是别的程序**（QQ/微信/别的页）：观察帧是**整屏截图** ⇒ 模型看到的是别人，
///      于是靠记忆猜坐标、连发十几轮（用户体感"莫名断了"）；
///      识图定位那条路上更早的事故是：截到 QQ ⇒ VLM 一直 `NOT_FOUND` ⇒ 单轮 168 秒。
///   ② 我们自己的调试窗抢前台（已从根上修掉：调试窗改为 SW_SHOWNOACTIVATE）。
///
/// 判据**只用事实**：扩展贴着某个网页（`AiLastPageUrl()` 非空）而前台**不是**浏览器
///   ⇒ 枚举浏览器窗口（类名判据复用 `WindowClassIsBrowserClass`，不另抄类名表）、
///     取它的进程名切回前台。
/// ⚠ 只做"把浏览器拉回前台"这一件事，不猜用户想干什么；切不到就**如实记一笔**，
///   不静默继续瞎找。
/// ⚠ **两个调用点共用这一份**：观察帧采集（`captureObservationNow`）与视觉定位前。
///   不许再抄第二份 —— 同一逻辑两份必然漂移（本仓同类教训已多次）。
/// 返回是否真的切过（调用方可据此决定要不要等一下重绘）。
/// ⚠ `AppendAiDebugLog` 是 `EngineHost` 的成员（engine_host_window.h），**文件作用域的 helper
///   调不到它** ⇒ 这里只**把要说的话填进 `outNote`**，由调用方（都是成员函数内）去打日志。
bool EnsureTaskWindowForeground(std::wstring* outNote = nullptr) {
    auto note = [&](const std::wstring& s) { if (outNote) *outNote = s; };
    if (AiLastPageUrl().empty() || ForegroundWindowIsBrowserClass()) return false;
    struct BrowserWinCtx { HWND hit; };
    BrowserWinCtx bctx{ nullptr };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<BrowserWinCtx*>(lp);
        if (!IsWindowVisible(h)) return TRUE;
        if (GetWindow(h, GW_OWNER) != nullptr) return TRUE;
        if (!WindowClassIsBrowserClass(h)) return TRUE;
        wchar_t t[8]{};
        if (GetWindowTextW(h, t, 8) <= 0) return TRUE;   // 无标题壳窗跳过
        c->hit = h;
        return FALSE;
    }, reinterpret_cast<LPARAM>(&bctx));
    if (!bctx.hit) {
        note(L"想把浏览器拉回前台但没找到浏览器窗口");
        return false;
    }
    DWORD bpid = 0;
    GetWindowThreadProcessId(bctx.hit, &bpid);
    std::wstring bproc;
    if (bpid) {
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, bpid);
        if (hp) {
            wchar_t path[MAX_PATH]{};
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(hp, 0, path, &n)) {
                const std::wstring full = path;
                const size_t slash = full.find_last_of(L"\\/");
                bproc = slash == std::wstring::npos ? full : full.substr(slash + 1);
            }
            CloseHandle(hp);
        }
    }
    if (bproc.empty()) return false;
    std::wstring actErr;
    if (windowmode::ActivateByProcessName(bproc, nullptr, actErr)) {
        note(L"已把浏览器拉回前台（前台原本不是浏览器）→ " + bproc);
        Sleep(120);   // 给它一点时间完成重绘
        return true;
    }
    note(L"想把浏览器拉回前台但失败：" + actErr);
    return false;
}
void EngineHost::StartActionsWorker(const std::vector<ScriptAction>& actions, const std::wstring& selfPath, const windowmode::WindowModeScriptConfig& wmCfg, const CoordMeta& execCoordMeta, double breakoutTime, const Hotkey& scriptHotkey, int debugStartIndex, bool debugStepMode, const std::vector<int>* debugBreakpoints, const Hotkey& debugHotkey) {
        if (running_) {
            StopRun();
            if (qst::desktop_tools::MacroDebug().IsCreated()) {
                qst::desktop_tools::MacroDebug().AppendLog(L"脚本正在运行，已请求停止");
            }
            return;
        }
        ClearScheduledYield();
        const bool debugMode = debugStartIndex >= 0;
        if (debugMode) {
            debugMode_.store(true, std::memory_order_relaxed);
            debugStepMode_.store(debugStepMode, std::memory_order_relaxed);
            debugPaused_.store(false, std::memory_order_relaxed);
            debugStepSignal_.store(false, std::memory_order_relaxed);
            debugStartIndex_ = debugStartIndex;
            debugBreakpoints_.clear();
            if (debugBreakpoints) {
                for (int b : *debugBreakpoints) debugBreakpoints_.insert(b);
            }
            debugHotkeyVk_ = debugHotkey.vk;
            debugHotkeyMods_ = debugHotkey.modifiers;
        }
        workerFinished_.store(false, std::memory_order_relaxed);
        runningFromScheduled_ = nextRunFromScheduled_;
        nextRunFromScheduled_ = false;
        running_ = true; stopFlag_ = false; breakoutUserInput_ = false; breakoutPaused_ = false;
        playbackPaused_.store(false, std::memory_order_relaxed);
        ghEmergencyStop.store(false, std::memory_order_release);
        ghWorkerCancelFlag = &stopFlag_;
        executedSteps_.store(0, std::memory_order_relaxed);
        playbackActionIndex_.store(0, std::memory_order_relaxed);
        playbackActionTotal_.store(static_cast<int>(actions.size()), std::memory_order_relaxed);
        SuspendHotkeysForPlayback();
        if (debugMode) {
            // 编辑器打开时 EngineSetUiMode(1) 会置「界面热键静音」并放行 LL 钩子，
            // 导致调试热键/通用启停热键全部失效。调试期间临时解除静音，结束后恢复。
            debugRestoreUiMute_ = ghUiModeHotkeysMuted.load(std::memory_order_relaxed);
            if (debugRestoreUiMute_) {
                ghUiModeHotkeysMuted.store(false, std::memory_order_relaxed);
            }
        }
        if (debugMode && debugHotkeyVk_ && hwnd_ && IsWindow(hwnd_)) {
            if (!RegisterHotKey(hwnd_, HOTKEY_DEBUG_ID, debugHotkeyMods_, debugHotkeyVk_)
                && qst::desktop_tools::MacroDebug().IsCreated()) {
                qst::desktop_tools::MacroDebug().AppendLog(
                    L"调试热键注册失败（可能与脚本/全局热键冲突），单步/暂停将不可用");
            }
        }
        {
            std::lock_guard<std::mutex> lock(extScriptStateMu_);
            runningScriptPath_ = selfPath;
            runningWindowMode_ = wmCfg;
            windowmode::WindowModeLogEventf(
                L"[窗口/后台窗口模式] 本次运行解析后配置：enabled=%d executionKind=%s targetExe=%ls autoLaunch=%d selectMethod=%d",
                wmCfg.enabled ? 1 : 0,
                wmCfg.executionKind == windowmode::WindowModeExecutionKind::HiddenDesktop
                    ? L"HiddenDesktop" : L"BackgroundWindow",
                wmCfg.targetExePath.c_str(),
                wmCfg.autoLaunchTarget ? 1 : 0,
                static_cast<int>(wmCfg.selectMethod));
            const auto slash = selfPath.find_last_of(L"\\/");
            runningScriptName_ = (slash == std::wstring::npos)
                ? selfPath : selfPath.substr(slash + 1);
            const auto dot = runningScriptName_.rfind(L'.');
            if (dot != std::wstring::npos) runningScriptName_.resize(dot);
        }
        ghHotkeySessionBusy.store(true, std::memory_order_relaxed);
        EnsureHotkeyAuxTimers();
        MouseInputRouter::Instance().Configure();
        BeginHighResTimer(); // 宏内短等待（录制轨迹）需要 1ms 定时器精度
        breakoutTaskbarShown_ = false;
        breakoutUiVisibleOnScreen_ = false;
        breakoutPlacement_ = BreakoutTaskbarPlacement{};
        workerBreakoutTime_ = (!wmCfg.enabled && breakoutTime > 0) ? breakoutTime : 0;
        aiHttpAbort_.Clear();
        wasVisibleBeforeRun_ = false;
        wasMinimizedBeforeRun_ = false;
        if (HWND face = UserFacingMainHwnd()) {
            wasVisibleBeforeRun_ = (IsWindowVisible(face) == TRUE);
            wasMinimizedBeforeRun_ = (IsIconic(face) == TRUE);
        }
        runSavedRectValid_ = GetWindowRestoredRect(hwnd_, &runSavedRect_);
        runSavedExStyle_ = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
        SetWindowCloaked(hwnd_, false);
        CloseEditorPopup(); CancelQuickInputTip();
        if (appSettings_.other.playSoundOnStart) PlayAppStartupSound();
        EnsureTrayIcon();
        if (debugMode) {
            // 调试期间主界面/编辑界面一律隐藏（不受「自动隐藏」设置影响）
            HideUserFacingMainWindow(true);
        } else if (wmCfg.enabled
            && wmCfg.executionKind != windowmode::WindowModeExecutionKind::BackgroundWindow
            && !windowmode::UsesCdpInput(wmCfg)) {
            // 窗口/后台窗口模式假前台 SendInput 必须在 UI 线程先把壳藏掉，否则工作线程
            // SetForegroundWindow 抢不到游戏（调试能点、主页/热键不能点）。
            HideUserFacingMainWindow(true);
        } else if (wasMinimizedBeforeRun_) {
            // 运行期间主窗口不保留绿色按钮；辅助窗口不受影响并继续显示绿色按钮。
            HideUserFacingMainWindow(false);
        } else if (appSettings_.other.autoHideMainWindow) {
            // 自动隐藏前让 DWM 保存真实界面，供脱离时的最小化任务栏预览使用。
            HideUserFacingMainWindow(true);
        } else {
            KeepEngineHostHiddenIfHeadless();
        }
        UpdateStatusTip();
        windowmode::SetWindowModeLogSink([this](const std::wstring& line) {
            // 扩展桥常开：未开窗口/后台窗口模式时勿把桥心跳灌进宏调试窗（与 AI/默认宏无关）
            {
                std::lock_guard<std::mutex> lock(extScriptStateMu_);
                if (!runningWindowMode_.enabled) return;
            }
            if (!qst::desktop_tools::MacroDebug().IsCreated()) return;
            if (deferPlaybackDebugUi_.load(std::memory_order_relaxed)) {
                PushDeferredDebugLine(line);
                return;
            }
            qst::desktop_tools::MacroDebug().AppendLog(line);
        });
        // ⚠ 不再按 `IsCreated()` 门住（docs §61）：清空是「窗口 + 落盘」两件事，
        //   窗没建时窗口那侧本来就是 no-op，而**落盘那份必须清掉**（否则上一局的
        //   现场会混进这一局）。每次开始运行 = 一份干净的日志。
        qst::desktop_tools::MacroDebug().ClearLog();
        breakoutHookState_ = BreakoutHookState{};
        breakoutHookState_.running = &running_;
        breakoutHookState_.simulatingDepth = &simulatingInputDepth_;
        breakoutHookState_.userInput = &breakoutUserInput_;
        if (globalHotkey_.enabled && globalHotkey_.vk) {
            breakoutHookState_.ignoreHotkeys.push_back(globalHotkey_);
        }
        for (const auto& script : scripts_) {
            if (script.hotkey.enabled && script.hotkey.vk) {
                breakoutHookState_.ignoreHotkeys.push_back(script.hotkey);
            }
        }
        if (scriptHotkey.enabled && scriptHotkey.vk) {
            breakoutHookState_.ignoreHotkeys.push_back(scriptHotkey);
        }
        // 始终装脱离钩：嵌套 runMacro/mousePlayback 切到默认模式时可中途打开脱离
        breakout_input::InstallBreakoutHooks(breakoutHookState_);
        worker_ = std::thread([this, actions, selfPath, wmCfg, execCoordMeta]() {
            if (debugMode_.load(std::memory_order_relaxed)) {
                // 只对调试脚本的主序列启用闸门/断点（嵌套宏/指令块指向其它 actions 副本）
                debugActionsPtr_ = &actions;
            }
            if (wmCfg.enabled) {
                windowmode::WindowModeLog(
                    wmCfg.executionKind == windowmode::WindowModeExecutionKind::BackgroundWindow
                        ? L"[窗口/后台窗口模式] 后台窗口模式：工作线程已启动"
                        : L"[窗口/后台窗口模式] 独立桌面模式：工作线程已启动");
            }
            bool usesOcr = ScriptUsesTextRecognition(actions);
            workerUsesOcrVars_ = usesOcr;
            matchVars_.clear();
            matchListVars_.clear();
            if (usesOcr) ocrVars_.clear();
            loopVars_.clear();
            timerStarts_.clear();
            aiVars_.clear();
            userVars_.clear();
            ClearImageVars(imageVars_);
            curLoops_ = 0;
            const unsigned long long imageVarRunId = GetTickCount64();
            bool ocrSessionHeld = false;
            // OCR 文本核对预算：只给「不确定」的定位做核对，且一次运行最多这么多次
            //（OCR 是可选能力，装了引擎才走；没装时这一段完全静默跳过）
            int ocrVerifyBudget = 8;
            auto holdOcrSession = [&ocrSessionHeld]() {
                if (ocrSessionHeld) return;
                EnsureOcrSession();
                ocrSessionHeld = true;
            };
            UINT heldKeyVk = 0; // 兼容单键跟踪；多键用 heldKeys
            std::unordered_set<UINT> heldKeys;
            HBITMAP lockedScreen_ = nullptr;
            int lockedVirtX_ = 0;
            int lockedVirtY_ = 0;

            windowmode::WindowModeExecutor wmExec;
            windowmode::WindowModeExecutor* wmExecPtr = &wmExec;
            windowmode::WindowModeScriptConfig activeWmCfg = wmCfg;
            std::vector<NestedModeFrame> nestedModeStack;
            wmExec.SetEnableFakeFocusInjection(
                appSettings_.windowMode.enableFakeFocusInjection);
            // 窗口变速：独立于假焦点注入的开关。关掉注入时窗口/后台窗口模式会改为「仅注入时钟补丁」，
            // 否则这个开关在用户关掉注入后就永远静默失效（曾因此被当成「功能没做」）。
            wmExec.SetEnableWindowTimeScale(
                appSettings_.windowMode.enableWindowTimeScale);
            wmExec.SetInjectionTechnique(
                windowmode::inject::TechniqueFromInt(
                    appSettings_.windowMode.injectionTechnique));
            wmExec.SetHideInjectedModule(
                appSettings_.windowMode.hideInjectedModule);

            // 窗口变速（变速齿轮）：回放倍速同时作用于**目标窗口进程**的时钟，
            // 让游戏冷却 / 动画与脚本一起加速（否则 2 倍速回放时游戏冷却不变，
            // 脚本会在冷却结束前就再次操作）。实现见 src/window_mode/time_scale_clock.h。
            // 会话结束（本作用域退出）时无条件复位，避免把目标窗口留在变速状态。
            struct WindowTimeScaleReset {
                windowmode::WindowModeExecutor* exec = nullptr;
                ~WindowTimeScaleReset() {
                    if (exec) exec->SetWindowTimeScale(0.0);
                }
            } wmTimeScaleReset{wmExecPtr};
            // playbackTimeScale 是「秒数系数」（1/倍速）；下发给目标进程的是「时钟倍速」。
            // 倍速 == 1.0 时**彻底不挂钩**（下发 0 = 让 DLL 卸载补丁）而不是「挂钩后原速转发」：
            // 原速回放本来就不需要改目标时钟，少一次 IAT 改写就少一分被反作弊盯上的面。
            auto wmApplyTimeScale = [this, wmExecPtr](double timeScale) {
                if (!wmExecPtr) return;
                if (!appSettings_.windowMode.enableWindowTimeScale) return;
                const double speed = timeScale > 0.0 ? 1.0 / timeScale : 1.0;
                if (speed == 1.0) {
                    wmExecPtr->SetWindowTimeScale(0.0);
                    return;
                }
                wmExecPtr->SetWindowTimeScale(speed);
            };

            // 计算模板缩放比例（用于找图跨分辨率适配，嵌套宏可切换 activeCoordMeta）
            int execTargetW = 0, execTargetH = 0, execVirtX = 0, execVirtY = 0;
            GetVirtualScreenBounds(execVirtX, execVirtY, execTargetW, execTargetH);
            CoordMeta activeCoordMeta = execCoordMeta;
            auto currentTmplScale = [&]() -> TemplateScale {
                if (wmExecPtr && wmExecPtr->IsActive() && activeWmCfg.windowRelativeCoordinates) {
                    const TemplateScale wmTs = wmExecPtr->FindImageTemplateScale();
                    if (wmTs.sx > 0.0 && wmTs.sy > 0.0) return wmTs;
                }
                if (activeCoordMeta.refWidth <= 0 || activeCoordMeta.refHeight <= 0) {
                    return TemplateScale{};
                }
                return ComputeTemplateScale(activeCoordMeta, execTargetW, execTargetH);
            };

            if (wmCfg.enabled) {
                std::wstring wmErr;
                windowmode::BeginRunOptions wmBeginOpts;
                wmBeginOpts.launchTarget = true;
                wmBeginOpts.cancelFlag = &stopFlag_;
                {
                    const auto slash = selfPath.find_last_of(L"\\/");
                    wmBeginOpts.launchSearchDir = slash == std::wstring::npos ? L"" : selfPath.substr(0, slash);
                }
                if (!wmExec.BeginRun(wmCfg, wmErr, wmBeginOpts)) {
                    if (StopRequested()) {
                        PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
                        return;
                    }
                    if (hwnd_) {
                        promptPendingMessage_ = wmErr.empty()
                            ? L"窗口/后台窗口模式启动失败" : wmErr;
                        // 先结束运行并恢复主窗口，再弹提示，避免遮罩坐标错位导致「确定」点不到。
                        PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
                        PostMessageW(hwnd_, WM_APP_PROMPT, 0, 0);
                    } else {
                        PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
                    }
                    return;
                }
                wmExec.SetCoordMeta(activeCoordMeta);
                windowmode::WindowModeLog(L"[窗口/后台窗口模式] 已绑定目标，开始运行");
                windowmode::WindowModeLogDesktopSnap(L"绑定后", wmExec.TargetHwnd());
                // 绑定校验：**报警不静默**（2026-09-29）。
                // 双开同一款游戏时两份客户端「同类名 + 同标题」，自动化只能按本会话绑定的
                // 那个 hwnd 投递；用户反馈过"动作跑到另外一份客户端上 / 一会走A一会原地A"，
                // 而日志里以前完全看不出这件事。这里把"还有几个同名兄弟 + 按 pid/客户区区分"
                // 明确打出来：既不静默猜，也不阻断（用户仍可继续，只是知道该看哪一项）。
                if (HWND boundTop = wmExec.TargetHwnd()) {
                    const int peers = windowmode::CountSameNamePeers(boundTop);
                    if (peers > 0) {
                        wchar_t idbuf[320]{};
                        swprintf_s(idbuf,
                            L"[窗口/后台窗口模式] ⚠ 绑定校验：还有 %d 个同类名且同标题的窗口（双开/多开）——"
                            L"本会话只按此 hwnd 投递；若动作跑到另一份客户端上，请停止后在窗口列表里"
                            L"按 pid/客户区 重新选择（别只按标题判断）", peers);
                        windowmode::WindowModeLog(idbuf);
                    }
                }
                if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                    HWND th = wmExec.TargetHwnd();
                    wchar_t cls[128]{};
                    if (th) GetClassNameW(th, cls, 128);
                    wchar_t buf[192]{};
                    swprintf_s(buf, L"窗口/后台窗口模式已绑定 hwnd=0x%p class=%s%s",
                        th, cls,
                        wmExec.UsesBackgroundWindow() ? L" [后台]"
                            : (wmExec.IsCdpInputMode() ? L" [独立桌面·扩展]" : L" [独立桌面]"));
                    AppendDebugLog(buf);
                }
            } else if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                bool anyRel = wmCfg.windowRelativeCoordinates;
                for (const auto& act : actions) {
                    if (act.windowRelative) { anyRel = true; break; }
                }
                if (anyRel) {
                    AppendDebugLog(
                        L"窗口/后台窗口模式未启用：脚本含窗口相对坐标/找图，将误走全屏桌面"
                        L"（游戏常见匹配度约 50% 失败）。请用后台窗口模式+图片定位重新录制。");
                }
            }
            if (usesOcr) holdOcrSession();

            {
                const auto backend = wmCfg.enabled
                    ? quickscript::ForegroundInputBackend::Software
                    : appSettings_.playback.foregroundInputBackend;
                ForegroundInputRouter::Instance().BeginSession(backend);
                // 脱离=0：不装脱离钩、不记移动/滚轮指纹；热键仍靠键/鼠标按钮指纹。
                if (ForegroundInputRouter::Instance().IsHidActive()) {
                    synthetic_input::SetBreakoutTracking(workerBreakoutTime_ > 0);
                }
                if (backend != quickscript::ForegroundInputBackend::Software
                    && ForegroundInputRouter::Instance().IsHidActive()) {
                    AppendDebugLog(std::wstring(L"前台注入后端：")
                        + quickscript::ForegroundInputBackendName(
                            ForegroundInputRouter::Instance().ActiveBackend()));
                    AppendDebugLog(L"HID模式：VirtualHid 绝对移标用 SetCursorPos（不发绝对 HID，避免 mouhid 主屏映射乱漂）；相对/按键/滚轮仍走驱动");
                    if (workerBreakoutTime_ > 0) {
                        AppendDebugLog(L"驱动注入：脱离=LL；VirtualHid 绝对移标不记移动指纹(靠INJECTED)，真人挪鼠/点击可脱离");
                    } else {
                        AppendDebugLog(L"驱动注入：仅已登记启停热键指纹（脱离=0）");
                    }
                } else if (ForegroundInputRouter::Instance().DidFallback()) {
                    const std::wstring fb = ForegroundInputRouter::Instance().FallbackReason();
                    AppendDebugLog(fb);
                    // 回放开始时再打一行醒目摘要，避免只扫过调试窗时漏看。
                    AppendDebugLog(L"【警告】当前不是驱动级注入，安全软件/反作弊可能仍按系统模拟处理。");
                }
            }

            auto wmSetPos = [this, wmExecPtr](int x, int y, int rx, int ry) {
                if (wmExecPtr && wmExecPtr->IsActive()) {
                    const bool hw = wmExecPtr->PreferHardwareInput();
                    if (hw) MarkSimulatedInput();
                    wmExecPtr->MoveMouseClient(x, y, rx, ry, [this](int r) { return RandomInt(r); }, true);
                    if (hw) UnmarkSimulatedInput();
                } else {
                    MarkSimulatedInput();
                    SetCursorScreenPos(x + RandomInt(rx), y + RandomInt(ry));
                    UnmarkSimulatedInput();
                }
            };
            auto wmSetLivePos = [this, wmExecPtr](int x, int y, int rx, int ry) {
                if (wmExecPtr && wmExecPtr->IsActive()) {
                    const bool hw = wmExecPtr->PreferHardwareInput();
                    if (hw) MarkSimulatedInput();
                    wmExecPtr->MoveMouseClient(x, y, rx, ry, [this](int r) { return RandomInt(r); }, false);
                    if (hw) UnmarkSimulatedInput();
                } else {
                    MarkSimulatedInput();
                    SetCursorScreenPos(x + RandomInt(rx), y + RandomInt(ry));
                    UnmarkSimulatedInput();
                }
            };

            auto wmUsesTarget = [wmExecPtr]() {
                return wmExecPtr && wmExecPtr->IsActive();
            };
            auto wmUsesBackground = [wmExecPtr]() {
                return wmExecPtr && wmExecPtr->IsActive() && wmExecPtr->UsesBackgroundWindow();
            };
            auto wmSendKey = [this, wmExecPtr, wmUsesTarget](UINT vk, bool down) {
                if (wmUsesTarget()) {
                    const bool hw = wmExecPtr->PreferHardwareInput();
                    if (hw) MarkSimulatedInput();
                    wmExecPtr->PostKeyToTarget(vk, down);
                    if (hw) UnmarkSimulatedInput();
                } else {
                    SendKey(vk, down);
                }
            };
            auto wmSendHeldModifiers = [wmSendKey](const ScriptAction& act, bool down) {
                if (act.holdLeftWin) wmSendKey(VK_LWIN, down);
                if (act.holdRightWin) wmSendKey(VK_RWIN, down);
                if (act.holdLeftCtrl) wmSendKey(VK_LCONTROL, down);
                if (act.holdRightCtrl) wmSendKey(VK_RCONTROL, down);
                if (act.holdLeftAlt) wmSendKey(VK_LMENU, down);
                if (act.holdRightAlt) wmSendKey(VK_RMENU, down);
                if (act.holdLeftShift) wmSendKey(VK_LSHIFT, down);
                if (act.holdRightShift) wmSendKey(VK_RSHIFT, down);
            };
            auto activateDesktopAt = [this](int x, int y) {
                POINT pt{x, y};
                HWND hit = WindowFromPoint(pt);
                if (!hit) return;
                HWND root = GetAncestor(hit, GA_ROOT);
                if (!root) root = hit;
                DWORD pid = 0;
                GetWindowThreadProcessId(root, &pid);
                if (pid == GetCurrentProcessId()) return;
                if (GetForegroundWindow() == root) return;
                std::wstring err;
                if (!windowmode::ActivateWindow(root, err)
                    && appSettings_.playback.autoOutputKeyFunctionDebug) {
                    wchar_t cls[160]{};
                    GetClassNameW(root, cls, 160);
                    AppendDebugLog(std::wstring(L"默认模式点击未能激活窗口 class=") + cls
                        + L"：" + err);
                }
            };
            auto wmMouseButton = [this, wmExecPtr, wmUsesTarget](int cx, int cy, MouseButtonType btn, bool down) {
                if (wmUsesTarget()) {
                    const bool hw = wmExecPtr->PreferHardwareInput();
                    if (hw) MarkSimulatedInput();
                    wmExecPtr->PostMouseButtonAtClient(cx, cy, btn, down);
                    if (hw) UnmarkSimulatedInput();
                } else {
                    MouseButtonEvent(btn, down);
                }
            };
            auto wmMouseClick = [this, wmExecPtr, wmUsesTarget, &activateDesktopAt](int cx, int cy, MouseButtonType btn) {
                if (wmUsesTarget()) {
                    const bool hw = wmExecPtr->PreferHardwareInput();
                    if (hw) MarkSimulatedInput();
                    wmExecPtr->PostMouseClickAtClient(cx, cy, btn);
                    if (hw) UnmarkSimulatedInput();
                } else if (cx != 0 || cy != 0) {
                    activateDesktopAt(cx, cy);
                    SendMouseClickAtScreen(cx, cy, btn);
                } else {
                    MouseClick(btn);
                }
            };
            auto wmSendShortcut = [wmSendKey](const ScriptAction& action) {
                ScriptAction tmp = action;
                ApplyShortcutPreset(tmp, action.shortcutPreset);
                const UINT playVk = NormalizeScriptKeyVk(tmp.keyVk, tmp.keyText);
                if (tmp.holdLeftWin) wmSendKey(VK_LWIN, true);
                if (tmp.holdLeftCtrl) wmSendKey(VK_LCONTROL, true);
                if (tmp.holdLeftAlt) wmSendKey(VK_LMENU, true);
                if (tmp.holdLeftShift) wmSendKey(VK_LSHIFT, true);
                wmSendKey(playVk, true);
                wmSendKey(playVk, false);
                if (tmp.holdLeftShift) wmSendKey(VK_LSHIFT, false);
                if (tmp.holdLeftAlt) wmSendKey(VK_LMENU, false);
                if (tmp.holdLeftCtrl) wmSendKey(VK_LCONTROL, false);
                if (tmp.holdLeftWin) wmSendKey(VK_LWIN, false);
            };
            // 用户想用 Ctrl+Space / Win+Space / Alt+Shift 主动切换 IME：发键前不预切英文
            auto isImeToggleShortcut = [](const ScriptAction& a) {
                if (a.keyVk == VK_SPACE
                    && (a.holdLeftCtrl || a.holdRightCtrl
                        || a.holdLeftWin || a.holdRightWin))
                    return true;
                if ((a.holdLeftAlt || a.holdRightAlt)
                    && (a.keyVk == VK_LSHIFT || a.keyVk == VK_RSHIFT))
                    return true;
                return false;
            };

            auto clearLockedScreen = [&]() {
                if (lockedScreen_) {
                    DeleteBitmapHandle(lockedScreen_);
                    lockedScreen_ = nullptr;
                }
            };

            const std::vector<ScriptAction>* activeActions = &actions;
            std::wstring runningScriptPath = selfPath;

            // 本轮「引擎**实际执行到**的相对位移」。回放保真诊断必须用这个，不能用
            // `SumRelativeMoves(actions)` —— 那是脚本静态条数，含 Loop/Goto 分支时
            // 与执行次数不等（`ScriptIsTimedInputSequence` 明确把 Loop/Goto 算作
            // 时间轴脚本），拿静态值去比实际注入量必然误报「注入层改动了位移」。
            // 每轮开头与 `MouseInputRouter::ResetStats()` 一起清零。
            long long reqRelDx = 0;
            long long reqRelDy = 0;
            uint64_t reqRelPackets = 0;

            auto containerBodyEnd = [&activeActions](size_t containerIndex) -> size_t {
                return static_cast<size_t>(ContainerBodyEnd(*activeActions, static_cast<int>(containerIndex)));
            };

            enum class RunRangeResult { Normal, BreakLoop, GotoPending };
            std::optional<size_t> pendingGoto;
            std::optional<size_t> loopEntryGotoTarget;
            bool pendingBreakLoop = false;
            std::function<RunRangeResult(size_t, size_t)> runRange;
            std::function<RunRangeResult(const std::wstring&)> runBlockByName;
            std::unordered_set<std::wstring> blockCallStack;

            auto notifyBreakoutUi = [&]() {
                HWND h = hwnd_;
                if (!h || !IsWindow(h)) return;
                DWORD_PTR result = 0;
                SendMessageTimeoutW(h, WM_APP_BREAKOUT_UI, 0, 0,
                    SMTO_ABORTIFHUNG | SMTO_BLOCK, 3000, &result);
            };
            auto waitBreakoutCooldown = [&]() {
                if (workerBreakoutTime_ <= 0 || StopRequested()) return;
                breakoutUserInput_ = false;
                breakoutPaused_ = true;
                const double t = workerBreakoutTime_;
                const auto idleMs = std::chrono::milliseconds(static_cast<int>(t * 1000.0));
                AppendBreakoutDebugLog(L"脱离时间：宏已中断，松开按键后等待 "
                    + FormatBreakoutTimeForEditor(t) + L" 秒再继续");
                notifyBreakoutUi();
                breakout_input::BreakoutCooldownState cool{};
                while (!StopRequested()) {
                    breakout_input::BreakoutReconcileUserHolds();
                    const bool holding = breakout_input::BreakoutUserHolding();
                    const bool fresh = breakoutUserInput_.exchange(false, std::memory_order_relaxed);
                    const auto now = std::chrono::steady_clock::now();
                    if (!breakout_input::BreakoutCooldownStillWaiting(
                            holding, fresh, now, idleMs, cool)) {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                breakoutPaused_ = false;
                if (!StopRequested()) {
                    AppendBreakoutDebugLog(L"脱离时间：等待结束，从当前步骤重试后继续");
                }
                notifyBreakoutUi();
            };

            auto makeVarCtx = [&]() {
                MacroVariableContext ctx;
                ctx.matchVars = &matchVars_;
                ctx.matchListVars = &matchListVars_;
                ctx.ocrVars = usesOcr ? &ocrVars_ : nullptr;
                ctx.aiVars = &aiVars_;
                ctx.userVars = &userVars_;
                ctx.imageVars = &imageVars_;
                ctx.loopVars = &loopVars_;
                ctx.timerStarts = &timerStarts_;
                ctx.curLoops = curLoops_;
                return ctx;
            };

            auto resolveTemplatePath = [&](bool useVar, const std::wstring& stored) -> std::wstring {
                if (useVar) return ResolveRuntimeImagePath(stored, &imageVars_);
                const std::wstring resolved = ResolveImagePath(stored);
                return resolved.empty() ? stored : resolved;
            };

            AiSessionStore aiSessions;
            int aiLoopDepth = 0;
            AiStepBudgetState aiRootBudget{};
            AiStepFrame* aiCurFrame = nullptr;
            const ScriptAction* aiInheritParent = nullptr;

            std::function<void(const ScriptAction&, const ScriptAction*)> runAiActionExecute;
            auto executeOne = std::function<void(const ScriptAction&)>();

            runAiActionExecute = [this, &usesOcr, &holdOcrSession, &ocrVerifyBudget, &heldKeyVk, &runRange, &runningScriptPath,
                &activeActions, &lockedScreen_, &lockedVirtX_, &lockedVirtY_, &clearLockedScreen, &makeVarCtx,
                &resolveTemplatePath,
                &executeOne, &runAiActionExecute, &aiSessions, &aiLoopDepth, &aiRootBudget, &aiCurFrame, &aiInheritParent,
                &pendingBreakLoop, wmExecPtr, &wmUsesTarget, &activeWmCfg, &currentTmplScale, imageVarRunId](
                const ScriptAction& action,                 const ScriptAction* inheritFrom) {
                if (StopRequested()) return;
                AiActionExecuteNestGuard nestGuard;
                if (!nestGuard.entered()) {
                    AppendAiDebugLog(L"AI动作执行：嵌套已达上限（最多顶层+1层），跳过本步");
                    return;
                }
                ScriptAction eff = inheritFrom ? InheritAiActionFields(action, *inheritFrom) : action;
                aiInheritParent = &eff;

                // 逻辑转化：仅顶层录制；嵌套 AI 不开会话。
                // else 回退 AI（remark 含「逻辑转化回退」）走 heal 会话：可 promote，不清 memo。
                const bool logicConvertHeal = eff.aiLogicConvert
                    && AiActionExecuteNestDepth() <= 1
                    && eff.remark.find(L"逻辑转化回退") != std::wstring::npos;
                const bool logicConvertTop = eff.aiLogicConvert
                    && AiActionExecuteNestDepth() <= 1;
                if (logicConvertTop) {
                    std::wstring convertPath = runningScriptPath;
                    if (convertPath.empty()) {
                        std::lock_guard<std::mutex> lock(extScriptStateMu_);
                        convertPath = runningScriptPath_;
                    }
                    AiLogicConvertSessionBegin(true, eff.aiLogicBlockName, eff.aiPrompt,
                        convertPath, action.originalNo, logicConvertHeal);
                }

                auto flushLogicConvertWriteback = [&](const wchar_t* reasonTag) -> bool {
                    if (!AiLogicConvertSessionActive()) return false;
                    std::wstring pathFb = runningScriptPath;
                    if (pathFb.empty()) {
                        std::lock_guard<std::mutex> lock(extScriptStateMu_);
                        pathFb = runningScriptPath_;
                    }
                    std::wstring summary, err;
                    if (!TryFlushAiLogicConvertSession(pathFb, summary, err)) {
                        if (!err.empty()) {
                            AppendAiDebugLog(L"逻辑转化：写回未完成 — " + err
                                + (reasonTag && reasonTag[0]
                                    ? (L"（" + std::wstring(reasonTag) + L"）") : L""));
                        }
                        return false;
                    }
                    AppendAiDebugLog(L"逻辑转化：已写回脚本 → " + summary
                        + (reasonTag && reasonTag[0]
                            ? (L"（" + std::wstring(reasonTag) + L"）") : L""));
                    const std::wstring sp = AiLogicConvertSessionScriptPath().empty()
                        ? pathFb : AiLogicConvertSessionScriptPath();
                    NotifyLogicConvertUi(sp, summary);
                    NotifyAgentScriptLibraryChanged();
                    if (hwnd_ && !sp.empty()
                        && _wcsicmp(sp.c_str(), currentPath_.c_str()) == 0) {
                        PostMessageW(hwnd_, WM_APP_LOGIC_CONVERT_DONE, 0, 0);
                    }
                    return true;
                };

                AiStepFrame childFrame{};
                // Agent 闭环会执行大量原子动作（locate=move+click、wait…）；
                // aiMaxSteps 同时影响轮次，步骤预算需放大，否则中途「预算用尽」点不动。
                const int stepBudget = (eff.aiMaxSteps < 0)
                    ? -1
                    : std::clamp(std::max(eff.aiMaxSteps * 5, 40), 40, 200);
                childFrame.localMax = stepBudget;
                if (aiCurFrame && aiCurFrame->shared) {
                    childFrame.shared = aiCurFrame->shared;
                } else {
                    aiRootBudget = AiStepBudgetState{};
                    aiRootBudget.maxSteps = stepBudget;
                    childFrame.shared = &aiRootBudget;
                }
                AiStepFrame* prevFrame = aiCurFrame;
                aiCurFrame = &childFrame;

                auto propagateAiHistory = [&](AgentCore* core, const ScriptAction& action, size_t histBefore,
                    const std::wstring& sysPrompt, bool withTools, int maxTokens, int timeoutMs) {
                    if (core && action.aiContextMode != 0) {
                        aiSessions.PropagateHistoryAfterCall(
                            action.aiContextMode, aiLoopDepth, core, histBefore,
                            action, sysPrompt, appSettings_, timeoutMs, withTools, maxTokens);
                    }
                };

                auto resolveAiRegion = [&](int& x1, int& y1, int& x2, int& y2) -> bool {
                    if (wmUsesTarget()) {
                        return wmExecPtr->ResolveAiScreenRect(
                            eff, x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_);
                    }
                    int sx = 0, sy = 0, rsw = 0, rsh = 0;
                    GetVirtualScreenRect(sx, sy, rsw, rsh);
                    int searchX1 = sx, searchY1 = sy, searchX2 = sx + rsw, searchY2 = sy + rsh;
                    if (eff.aiSearchX2 > eff.aiSearchX1 && eff.aiSearchY2 > eff.aiSearchY1) {
                        searchX1 = eff.aiSearchX1;
                        searchY1 = eff.aiSearchY1;
                        searchX2 = eff.aiSearchX2;
                        searchY2 = eff.aiSearchY2;
                    }
                    // 勾选「根据图片」：在绝对屏幕区域内找图，用匹配框作为最终截屏区
                    if (eff.aiRegionByImage && !eff.aiTargetImagePath.empty()) {
                        const TemplateScale tmplScale = currentTmplScale();
                        const std::wstring tmplPath = resolveTemplatePath(
                            eff.aiImageUseVar, eff.aiTargetImagePath);
                        HBITMAP tmpl = LoadBitmapFromFile(tmplPath);
                        if (!tmpl) return false;
                        ImageMatchOptions opt = BuildExecutionFindImageOptions(eff, tmplScale);
                        RestrictFindImageToSingleAnchor(opt);
                        opt.maxOverlap = 0.5;
                        ImageMatchOutput output;
                        if (lockedScreen_) {
                            output = FindTemplateInFrozenScreenMulti(
                                lockedScreen_, lockedVirtX_, lockedVirtY_,
                                searchX1, searchY1, searchX2, searchY2, tmpl, opt);
                        } else {
                            output = FindTemplateOnScreenMulti(
                                searchX1, searchY1, searchX2, searchY2, tmpl, opt);
                        }
                        DeleteBitmapHandle(tmpl);
                        if (output.matches.empty()) return false;
                        const ImageMatchResult& match = output.matches.front();
                        return ApplyImageRegionToMatch(eff,
                            match.topLeftX, match.topLeftY,
                            match.bottomRightX, match.bottomRightY,
                            x1, y1, x2, y2);
                    }
                    x1 = searchX1; y1 = searchY1; x2 = searchX2; y2 = searchY2;
                    return x2 > x1 && y2 > y1;
                };

                MacroVariableContext ctx = makeVarCtx();
                MacroClipboardSnapshot clipSnap;
                std::vector<std::string> clipExtraJpeg;
                if (PromptMentionsCtrlClipboard(eff.aiPrompt)) {
                    clipSnap = ReadMacroClipboardSnapshot();
                    ctx.clipboardSnapshot = &clipSnap;
                    ctx.clipboardExpandMode = ClipboardExpandMode::Ai;
                    clipExtraJpeg = EncodeClipboardSnapshotImages(clipSnap);
                    if (!clipExtraJpeg.empty()) {
                        AppendAiDebugLog(L"AI动作执行：附加剪贴板图片 "
                            + std::to_wstring(clipExtraJpeg.size()) + L" 张");
                    } else if (clipSnap.hasBitmap || std::any_of(
                        clipSnap.files.begin(), clipSnap.files.end(), LooksLikeImageFilePath)) {
                        AppendAiDebugLog(L"AI动作执行：提示词引用了剪贴板图片，但未能编码附图");
                    }
                }
                const std::wstring resolvedPrompt = ResolveMacroVariables(eff.aiPrompt, ctx);
                const std::wstring effModel = EffectiveAiModelName(eff);
                ScriptAction prepAction = eff;
                prepAction.aiModelName = effModel;

                AiCaptureMapping liveMap{};
                bool liveMapValid = false;
                // openWebpage 等动作后的本地 settle 结果（注入下一轮 Agent 观察提示）
                std::wstring lastUiSettleHint;
                bool lastUiSettleSuggestRefresh = false;
                int lastUiSettleElapsedMs = 0;
                bool lastUiSettleReacted = false;
                /// 本批「无反应」是否**确定**（`UiVisualSettleResult::reactionConclusive`）
                bool lastUiSettleConclusive = true;
                bool lastUiSettleSettled = false;
                // ── 快路径命中记账（跨 lambda，故放这个外层作用域）────────────────
                // 捷径（元素索引 / 文字直点）会**提前 return**，而「点了没反应就作废那条捷径」
                // 的记账写在识图链路内部 —— 于是对捷径路径**永远走不到**：索引过期也不作废。
                // 实测后果（docs §27.1）：同一坐标连点 11 次、每次「settle：无反应」、
                // 最后撞 locateAndClick 上限、任务以「无法继续点选卡片」收尾 —— 白烧 35s。
                // 现在统一成：捷径命中时记下键，同批 settle 判「无反应」再回头处理它。
                // ⚠ 加新的捷径时**必须**在这里登记，否则同一个坑会再来一次。
                bool aiOcrDirectIndexThisBatch = false;
                /// ★本帧的「可点元素索引」（UIA 控件 ∪ OCR 文字，见 BuildAiElementIndex）。
                /// 观察时构建一次，`locateAndClick(target)` 先在这里查坐标（0 次 VLM）；
                /// 索引里没有才回落识图。
                /// ⚠ 只在**重新构建的那条路径**里覆盖（见 observeScreenForAgent）：
                ///   「界面未变」会提前 return 且不重跑 OCR，那时保留旧索引是**正确的**
                ///   （坐标仍然成立）；若在这里每帧清空，未变帧之后的定位就永远查不到索引。
                ///   界面真变了必然走重建路径 → 索引随之刷新，所以不会拿旧坐标点。
                std::vector<AiElementEntry> aiElementIndexThisFrame;

// ★★ 本次 AI 动作里**点过的 DOM 目标**（按名字）——只用于**如实提醒"这个你刚点过"**。
//   为什么需要（用户实测 2026-09-29）：模型点「点赞（Q）」后无法从树里确认状态
//   （B站点赞态在 CSS class 里，扩展采不到 aria-pressed）⇒ 它凭"我点过了"宣称完成
//   ⇒ 被嘴炮闸抓回来 ⇒ **又点一次** ⇒ 而开关类再点一次是**取消**（把赞撤了）。
//   ⚠ 批 D 删掉的是「刚点过就**拒绝**」的闸（引擎替模型决定这一击该不该发，docs §47）；
//     这里**只回报事实**，点不点仍由模型决定 —— 两者不是一回事，别混。
std::vector<std::wstring> aiDomClickedKeys;
                /// ★元素索引也是**提前 return 的快路径**（查表命中就不识图了），
                /// 所以同样要在动作级登记：settle 判「无反应」时整表作废，逼下一次重新枚举。
                bool aiElementIndexThisBatch = false;
                std::wstring lastUiChangeRoisText;
                std::vector<ScreenChangeRoi> lastUiChangeRois;
                /// `lastUiChangeRois` 所在的**截图区域**（原点是它，不是屏幕原点）。
                /// ⚠ ROI 是**位图局部坐标**：拿屏幕坐标去和它比，只有在
                ///   「截图区域恰好 = 全屏 (0,0)」时才碰巧对。区域截图（窗口/后台窗口模式/限定区域）
                ///   下会整体错位 —— 与 §39.4 那条同一个坑，所以在这里把原点一起记下来。
                int lastUiRegionX1 = 0;
                int lastUiRegionY1 = 0;
                int lastUiRegionW = 0;
                int lastUiRegionH = 0;
                /// settle 刚写入 aiObs 后，下一轮观察必须上传，避免被「未变」短路
                bool forceNextObserveUpload = false;
                /// ★★「存下来的那张观察帧，模型**是否真的看过**」（docs §64）。
                /// `settled.lastFrame` 会被存成比对基线，但**那一轮并没有上传**——
                /// 拿它当基线比对，等于让"自己和自己比"⇒ 回执写「界面未变」而模型手上
                /// 还是动作之前的图 ⇒ 它永远看不到自己动作的结果（实测白烧 11 轮）。
                bool obsImageSeenByModel = true;
                /// ★上一帧的 OCR 行表：帧间文字差分用（docs §65）。空 = 还没有上一帧可比。
                std::vector<OcrTextLine> prevOcrTextLines;
                /// 最近一次定位/点击的屏幕坐标（动作局部验收，抑制视频区抢注意力）
                int lastActionScreenX = -1;
                int lastActionScreenY = -1;
                /// ★观察帧落点标注（通用机制，见 AiFrameClickMark）：
                /// 把「上一次真的点在哪」画进观察帧，让规划模型下次识图时顺手验收，
                /// 而不是只能靠「点完界面变没变」反推落点。坐标在真正点下去时记，
                /// 目标描述在进入定位时记 —— 两侧各写自己知道的那半。
                /// ⚠ 必须在 `executeActionsJsonNow` 之前声明：点击记账也在那条链里。
                int clickMarkX = -1;
                int clickMarkY = -1;
                int consecutiveDynamicOnlyObserves = 0;
                /// Alt+Tab 预览期间按住左 Alt（勿一按即松）
                bool altTabAltHeld = false;
                /// 预览已跨过的 API 轮数：Alt 悬太久会挡住整个桌面，超限强制松开
                int altTabHoldRounds = 0;

                auto releaseAltTabIfHeld = [&]() {
                    if (!altTabAltHeld) return;
                    SendKeyboardKey(VK_LMENU, false);
                    altTabAltHeld = false;
                    altTabHoldRounds = 0;
                };

                std::wstring lastPointerClickFgTitle;
                auto foregroundTitle = []() -> std::wstring {
                    HWND fg = GetForegroundWindow();
                    if (!fg) return {};
                    wchar_t buf[512]{};
                    GetWindowTextW(fg, buf, 512);
                    return buf;
                };
                auto notePointerClick = [&](int sx, int sy) {
                    if (sx < 0 || sy < 0) return;
                    const std::wstring fgTitle = foregroundTitle();
                    if (!lastPointerClickFgTitle.empty() && fgTitle != lastPointerClickFgTitle) {
                        lastActionScreenX = -1;
                        lastActionScreenY = -1;
                    }
                    lastPointerClickFgTitle = fgTitle;
                    // 落点记账：画进帧里的落点 + 「上一次真实落点」（逐步校验要用）。
                    clickMarkX = sx;
                    clickMarkY = sy;
                    MarkLastAiClickScreenPoint(sx, sy);
                    lastActionScreenX = sx;
                    lastActionScreenY = sy;
                };

                // 光标挪到虚拟屏角落（直接 SetCursor，勿走 executeActionsJson）
                auto parkCursorAwayFromUi = [&]() {
                    if (wmExecPtr && wmExecPtr->IsActive() && wmExecPtr->PreferHardwareInput()) {
                        return;
                    }
                    int vx = 0, vy = 0, vw = 0, vh = 0;
                    GetVirtualScreenRect(vx, vy, vw, vh);
                    if (vw < 32 || vh < 32) return;
                    SetCursorScreenPos(vx + std::max(8, vw) - 4, vy + std::max(8, vh) - 4);
                };

                /// 本批 settle 的定性结论（供调用方写回执用）。
                /// 为什么不用解析返回文本：**文本匹配脆弱**（措辞一改就静默失效），
                /// 而「这一击到底有没有生效」是回执里最要紧的一条信息（docs §36）。
                enum class AiBatchOutcome {
                    Unknown = 0,   ///< 没做 settle（例如不触发交互的动作）
                    Reacted,       ///< 界面确实变了 → 这一步生效了
                    NoReaction,    ///< 界面毫无变化 → 这一步很可能没生效
                };
                AiBatchOutcome lastBatchOutcome = AiBatchOutcome::Unknown;
                auto executeActionsJsonNow = [&](const std::wstring& rawJson) -> std::wstring {
                    // 不变量：Alt 只在连续的 switchWindow 之间按住。
                    // 一旦改做别的动作就先松开（=confirm 落到选中窗），
                    // 否则 Alt 悬着会把后续点击变成 Alt+点击，且预览一直挂在屏幕上。
                    std::wstring altNote;
                    if (altTabAltHeld) {
                        releaseAltTabIfHeld();
                        Sleep(120);
                        AppendAiDebugLog(L"  [诊断] 非 switchWindow 动作：已先松开 Alt 结束预览");
                        altNote = L"；已自动松开 Alt 结束 Alt+Tab 预览（切到当时选中的窗口）";
                    }
                    // 点击/键鼠前藏壳与调试窗，避免挡桌面目标或进截屏
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    std::wstring jsonStr = Trim(rawJson);
                    // ★必须用「括号配对 + 跳过字符串」的取法：构建器会在数组后追加
                    // 「[提示] 已自动…stopMacro…」，提示自带 [ ]，裸 rfind(']') 会把提示里的
                    // ] 当数组结尾 → 整段非法 JSON →「JSON 解析失败」（runCommand 实测踩到）。
                    if (const std::wstring arr = ExtractActionJsonArrayText(jsonStr); !arr.empty()) {
                        jsonStr = arr;
                    }
                    nlohmann::json steps;
                    try {
                        steps = nlohmann::json::parse(ToUtf8(jsonStr));
                    } catch (const std::exception& e) {
                        // ★★ 必须把**模型自己写的东西**回显给它（2026-10-02 真机：模型连续
                        //   10+ 轮发同一份坏 JSON —— `parse_error.101 at column 38` 这种报错
                        //   对它**毫无用处**，它不知道错在自己哪一段 ⇒ 原样重发 ⇒ 白烧十几轮。
                        //   回显原文 + 给一个最小例子，它才可能自我纠正。）
                        const std::wstring what = FromUtf8(std::string(e.what()));
                        std::wstring raw = jsonStr;   // 本就是 wstring，别再 FromUtf8
                        if (raw.size() > 160) raw = raw.substr(0, 160) + L"…";
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：动作 JSON 解析失败：" + what
                            + L"（原文前 160 字：「" + raw + L"」）");
                        return L"[错误] 动作 JSON 解析失败：" + what
                            + L"。你上一条输出开头是：「" + raw + L"」。"
                              L"请检查：只输出**一个 JSON 数组**；字段之间用英文逗号 `,`；"
                              L"键与值之间用英文冒号 `:`；字符串用英文双引号；"
                              L"例：`[{\"action\":\"locateAndClick\",\"target\":\"1.544Mbps\"}]`";
                    }
                    try {
                        if (!steps.is_array())
                            return L"[错误] 返回内容不是 JSON 数组";
                        // ★「本批 N 条」里的 stopMacro 要摘出来单独说（docs §72）：构建器会在
                        //   末尾自动追加一条 `stopMacro`，于是「本批 2 个动作」里真正会跑的
                        //   只有 1 条 —— 日志报 2 会让人（和排查者）以为有两条动作。
                        const auto countStopMacro = [](const nlohmann::json& arr) -> int {
                            int n = 0;
                            if (!arr.is_array()) return 0;
                            for (const auto& s : arr) {
                                if (s.is_object() && s.contains("type") && s["type"].is_string()
                                    && s["type"].get<std::string>() == "stopMacro") {
                                    ++n;
                                }
                            }
                            return n;
                        };
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：即时执行本批 "
                            + std::to_wstring(steps.size()) + L" 条"
                            + (countStopMacro(steps) > 0
                                ? (L"（其中 " + std::to_wstring(countStopMacro(steps))
                                    + L" 条是自动追加的 stopMacro）")
                                : std::wstring()));
                        bool settleNoReactionThisBatch = false;
                        /// 本批的「无反应」是否**确定**（见 AiUiReactionVerdict::conclusive）。
                        /// 只有确定的「无反应」才允许触发惩罚性动作（作废文字直点/元素索引）——
                        /// 动态画面上「画面自己在动」只能得出「无法归因」（docs §40.1）。
                        bool settleNoReactionConclusive = false;

                        // 截图坐标系 → 屏幕坐标（与 CompositeClick 一致；缩放/选区未映射会点偏）
                        const bool remapApi = liveMapValid
                            && liveMap.apiWidth > 0 && liveMap.apiHeight > 0
                            && liveMap.capX2 > liveMap.capX1 && liveMap.capY2 > liveMap.capY1;
                        // false = 应跳过本步（坐标无法解释，禁止放大飞点）
                        // ★ 坐标口径回执（2026-10-01）：把"我们按哪个口径解释"攒起来，收尾时回给模型
                        std::wstring coordApiNote;
                        // ★★ **坐标空间按「整批」决定，不逐点猜**（2026-10-01 实测致命）：
                        //   实测同一批里 `(173,576)` 被当**上传图像素**（→屏幕 1437），
                        //   而 `(173,911)` 因超出图高被当 **0~1000 归一化**（→屏幕 1310）
                        //   ⇒ 模型想指的两个选项落到了两个地方 ⇒ 它收到的反馈自相矛盾 ⇒
                        //   连续 16 批原地重发（用户体感"不会往下滚/选了又取消/效率很低"）。
                        //   ⇒ 先扫一遍整批：只要有一个坐标**超出附图**而**全部都在 0..1000**，
                        //     整批就统一按 0~1000 解释；否则整批按 upload 像素。
                        enum class BatchCoordSpace { Pixel, Normalized1000 };
                        BatchCoordSpace batchSpace = BatchCoordSpace::Pixel;
                        auto remapStepCoords = [&](nlohmann::json& params) -> bool {
                            if (!remapApi || !params.is_object()) return true;
                            if (!params.contains("type") || !params["type"].is_string()) return true;
                            const std::string type = params["type"].get<std::string>();
                            if (type != "moveMouse" && type != "mouseClick"
                                && type != "mouseDown" && type != "mouseUp") {
                                return true;
                            }
                            // locateAndClick 等已映射为屏幕绝对坐标，禁止再乘截图缩放
                            if (params.contains("coordSpace") && params["coordSpace"].is_string()
                                && params["coordSpace"].get<std::string>() == "screen") {
                                return true;
                            }
                            if (params.contains("moveFromVar")) {
                                const auto& mv = params["moveFromVar"];
                                if (mv.is_boolean() && mv.get<bool>()) return true;
                                if (mv.is_number_integer() && mv.get<int>() != 0) return true;
                            }
                            if (!params.contains("x") || !params.contains("y")) return true;
                            int apiX = 0, apiY = 0;
                            try {
                                if (params["x"].is_number()) apiX = params["x"].get<int>();
                                else if (params["x"].is_string()) apiX = std::stoi(params["x"].get<std::string>());
                                else return true;
                                if (params["y"].is_number()) apiY = params["y"].get<int>();
                                else if (params["y"].is_string()) apiY = std::stoi(params["y"].get<std::string>());
                                else return true;
                            } catch (...) {
                                return true;
                            }
                            const int rawX = apiX, rawY = apiY;
                            std::wstring coordNote;
                            if (batchSpace == BatchCoordSpace::Normalized1000) {
                                // 整批统一按 0~1000：**连"恰好落在图内"的点也一起换算**，
                                // 否则同一批里又是两种空间（这正是本次事故的形态）。
                                apiX = std::clamp(static_cast<int>(
                                    static_cast<long long>(rawX) * liveMap.apiWidth / 1000),
                                    0, liveMap.apiWidth - 1);
                                apiY = std::clamp(static_cast<int>(
                                    static_cast<long long>(rawY) * liveMap.apiHeight / 1000),
                                    0, liveMap.apiHeight - 1);
                                coordNote = L"整批统一按0~1000归一化";
                            } else if (!ResolveAgentPointerToApiImage(apiX, apiY,
                                    liveMap.apiWidth, liveMap.apiHeight,
                                    liveMap.srcWidth, liveMap.srcHeight, &coordNote)) {
                                AppendAiDebugLog(L"  跳过越界坐标：("
                                    + std::to_wstring(rawX) + L"," + std::to_wstring(rawY)
                                    + L") 不在截图 " + std::to_wstring(liveMap.apiWidth) + L"×"
                                    + std::to_wstring(liveMap.apiHeight)
                                    + L"；请用 locateAndClick，勿猜绝对坐标");
                                return false;
                            }
                            int screenX = apiX, screenY = apiY;
                            MapApiPointToScreen(liveMap, apiX, apiY, screenX, screenY);
                            // ★★ **落点在任务栏 ⇒ 拒绝**（2026-10-02 实测事故）。
                            //
                            //   实测：整屏观察帧**包含底部任务栏**，模型按画面比例给坐标时给出
                            //   upload y=576（=屏幕 y≈1437，屏幕高 1440）⇒ **点到任务栏**
                            //   ⇒ 焦点被桌面/任务栏抢走（下一轮 `观察帧画面主体：前台 = explorer.exe`）
                            //   ⇒ 之后的动作全落在错误的窗口上、整轮跑偏。
                            //   判据只看事实：落点是否在任务栏矩形内（`Shell_TrayWnd`）。
                            //   ⚠ 任务栏**不是任何应用窗口的一部分**，点它从来不是"操作目标界面"。
                            {
                                HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
                                RECT tr{};
                                if (tray && GetWindowRect(tray, &tr)
                                    && screenX >= tr.left && screenX < tr.right
                                    && screenY >= tr.top && screenY < tr.bottom) {
                                    coordApiNote = L"\n[事实] 你这次的落点 屏幕("
                                        + std::to_wstring(screenX) + L"," + std::to_wstring(screenY)
                                        + L")在**任务栏**上（不属于任何应用窗口）⇒ **已跳过这一步**。"
                                        L"⚠ 观察帧是**整屏**截图，最底下那条是系统任务栏，"
                                        L"**不是页面内容**：别按它的位置给坐标。"
                                        L"要看到页面下方的内容请用 scrollWheel 滚动页面，"
                                        L"要切窗口请用 activateWindow(match=…)。";
                                    AppendAiDebugLog(L"  [诊断] 跳过落点（在任务栏上）：屏幕("
                                        + std::to_wstring(screenX) + L"," + std::to_wstring(screenY) + L")");
                                    return false;
                                }
                            }
                            params["x"] = screenX;
                            params["y"] = screenY;
                            if ((rawX != apiX || rawY != apiY) && !coordNote.empty()) {
                                AppendAiDebugLog(L"  [诊断] 指针坐标("
                                    + std::to_wstring(rawX) + L"," + std::to_wstring(rawY)
                                    + L")→api(" + std::to_wstring(apiX) + L","
                                    + std::to_wstring(apiY) + L") " + coordNote
                                    + L" → 屏幕(" + std::to_wstring(screenX) + L","
                                    + std::to_wstring(screenY) + L")");
                            }
                            // ★★ **把"我们按哪个口径解释你的坐标"告诉模型**（2026-10-01）。
                            //
                            //   实测：模型一轮里给 (172,863) —— 而它收到的图上高只有 576
                            //   ⇒ 我们只能按 **0~1000 归一化**解释（`coordNote` 就是这么来的）。
                            //   但模型**不知道我们这么解释了**：它以为那是像素 ⇒ 它以为点到了
                            //   y=863 的位置，实际落在 y≈497 的位置（屏幕下半部）⇒
                            //   连续十几轮"点了没反应"、反复重发同一批（用户看到"随便点/还是断"）。
                            //   ⇒ 把口径如实回执出去（一行事实），模型下一轮就能自己校正。
                            //   ⚠ 只陈述"我们怎么解释的"，不训它"该怎么给"（那是提示词的职责）。
                            if (!coordNote.empty()) {
                                coordApiNote = L"\n[事实] 你这次的坐标("
                                    + std::to_wstring(rawX) + L"," + std::to_wstring(rawY)
                                    + L")被按**" + coordNote + L"**解释 → 图上("
                                    + std::to_wstring(apiX) + L"," + std::to_wstring(apiY)
                                    + L") → 屏幕(" + std::to_wstring(screenX) + L","
                                    + std::to_wstring(screenY) + L")。"
                                    L"本软件给模型的坐标口径是**upload 截图像素**"
                                    L"（见每帧清单标题里的「图像尺寸 W×H」）；"
                                    L"给 0~1000 的归一化值也能收，但请**整批统一**，"
                                    L"别在同一批里混两种口径。";
                            }
                            return true;
                        };

                        int stepCount = 0;
                        int skippedBadPointer = 0;
                        int skippedInvalid = 0;
                        // ★★「什么都没做」必须能自证原因（docs §72）。
                        //   旧实现在每条静默 continue 上**一声不响**：真机日志里半批
                        //   （20 击里的 10 击）回的是同一句
                        //   `[错误] 本批 0 步：没有可执行的动作（空数组或全部被跳过）` ——
                        //   模型**无法从中读出任何可行动信息**，只能照原样再点一遍；
                        //   排查的人（我）也只能靠读源码猜是哪条 continue。
                        //   现在每条丢弃都计数 + 记第一条原因，回执直接点名。
                        int skippedStopMacro = 0;
                        int skippedNoType = 0;
                        std::wstring firstSkipReason;
                        // ★★本批的定性结论**必须先清零**（docs §38.3）。
                        //   它是 `[结果] 界面已经变化/没有变化` 的唯一判据，而写它要走到
                        //   settle 那一步；本批 0 步时会**提前 return**，
                        //   于是回执读到的还是**上一批**的结论 —— 实测「本批完成 0 步」的
                        //   回执里写着「[结果] 界面已经变化 → 这一步已经生效，不要重做」，
                        //   而这一批什么都没点。回执说谎比没有回执更糟：模型据此认为做完了。
                        lastBatchOutcome = AiBatchOutcome::Unknown;
                        std::wstring firstInvalidError;
                        // ★逐步生效校验（通用）：盲批量里「先选中/切换 → 再作用」这类依赖链，
                        //   第一步没生效后面全是空转；而动态画面上的整批 settle 只会说「仍在变化」，
                        //   看不出第一步其实没点上。这里记下**第一次点击的落点**（点完光标就在那儿）
                        //   与点击步数，稍后用 settle 的变化区判断它到底有没有引起局部变化。
                        int firstClickScreenX = -1;
                        int firstClickScreenY = -1;
                        /// ★★「本批**第一次点击之前**」的画面基线（docs §60）。
                        ///   旧实现拿 `settled.lastChangeRois` 去分析第一次点击，而那个 settle 的
                        ///   基线是在第一次点击**已经执行之后**才截的（`settleBaseline` 只在
                        ///   `wantsSettle` 的那一步截，而多击批次里只有**最后**一个交互步 settle）
                        ///   ⇒ 第一次点击自己的效果**根本不在差分里** ⇒ 只要这句回执开口，
                        ///   它 100% 会说「第一次点击附近没有任何局部变化（该步很可能没生效）」——
                        ///   即使那一步明明生效了。实测后果：模型信了这句，回头**又点一次同一张卡**，
                        ///   而在 toggle 式选卡界面上「再点一次」= 取消选择/退出面板
                        ///   （用户主诉：「选了一张卡就退出选卡界面了」）。
                        ///   ⇒ 判据的宾语必须是**这段时间里真正发生的事**，基线必须是那一刻的。
                        HBITMAP firstClickBaseline = nullptr;
                        int firstClickBlX1 = 0, firstClickBlY1 = 0;
                        int firstClickBlX2 = 0, firstClickBlY2 = 0;
                        /// 本批有 6 条以上提前 return 的路径 ⇒ 用析构函数兜住释放，别漏
                        struct BitmapGuard {
                            HBITMAP* h = nullptr;
                            ~BitmapGuard() {
                                if (h && *h) { DeleteBitmapHandle(*h); *h = nullptr; }
                            }
                        } firstClickBitmapGuard{ &firstClickBaseline };
                        /// 本批实际点过的左键落点（settle 用它判「这一击附近有没有反应」）
                        std::vector<POINT> clickedPointsThisBatch;
                        /// 本批落点里「附近有局部变化」的个数（-1 = 本批不适用/只有一次落点）。
                        /// ★★这是**纯本地像素比较**：复用 settle 已经算好的同一批 ROI，
                        /// 零 API 调用、零额外识图，回执仍只多**一句**（docs §58）。
                        /// 它补的是「**投丢了**」与「**投了没用**」在模型眼里长得一模一样这件事：
                        /// 落点**全无**变化 ⇒ 这一批很可能根本没落地（被吞/节拍）；
                        /// 落点**多数有**变化 ⇒ 落地了、但目标状态没变 ⇒ 该换打法而不是重投。
                        /// ⚠ 不按落点逐个发裁决 —— Verify 是 agent 自己的推理
                        /// （VeriGUI/TVAE 的消融正说明它该由模型做，见 docs §58）。
                        int batchLandingHits = -1;
                        int clickedSteps = 0;
                        std::wstring stepEffectFact;
                        // 批量配方里每行都有 Enter：只在「本批最后一个会触发界面变化的步骤」上 settle，
                        // 中间行狂等会把 10 行填表拖成十几秒空转
                        auto stepWantsInteractionSettle = [](const nlohmann::json& raw) -> bool {
                            if (!raw.is_object()) return false;
                            std::string type;
                            nlohmann::json p = raw;
                            if (raw.contains("action") && raw["action"].is_string()) {
                                type = raw["action"].get<std::string>();
                                p = raw.value("params", nlohmann::json::object());
                            } else if (raw.contains("type") && raw["type"].is_string()) {
                                type = raw["type"].get<std::string>();
                            } else {
                                return false;
                            }
                            if (type == "mouseMove") type = "moveMouse";
                            if (type == "mouseClick" || type == "hotkeyShortcut") return true;
                            if (type != "keyClick") return false;
                            try {
                                std::wstring kt;
                                if (p.contains("keyText") && p["keyText"].is_string())
                                    kt = FromUtf8(p["keyText"].get<std::string>());
                                for (auto& c : kt) {
                                    if (c >= L'a' && c <= L'z')
                                        c = static_cast<wchar_t>(c - L'a' + L'A');
                                }
                                auto flag = [&](const char* k) {
                                    return p.contains(k) && p[k].is_boolean() && p[k].get<bool>();
                                };
                                return kt == L"ENTER" || kt == L"RETURN" || kt == L"ESCAPE"
                                    || kt == L"F5"
                                    || flag("holdLeftCtrl") || flag("holdLeftAlt")
                                    || flag("holdLeftWin");
                            } catch (...) {
                                return false;
                            }
                        };
                        // ★★ **扫一遍整批，定下唯一的坐标空间**（见上面 `batchSpace` 的注释）。
                        //   判据只看事实：有坐标**超出附图**、且全部落在 0..1000 ⇒ 整批按归一化。
                        //   ⚠ 一个坐标都不在图上、或出现 >1000 的绝对值时不猜（保持逐点兜底/拒绝）。
                        if (remapApi) {
                            bool anyPoint = false, anyOutside = false, allWithin1000 = true;
                            for (const auto& st : steps) {
                                if (!st.is_object()) continue;
                                std::string bt;
                                if (st.contains("type") && st["type"].is_string())
                                    bt = st["type"].get<std::string>();
                                else if (st.contains("action") && st["action"].is_string())
                                    bt = st["action"].get<std::string>();
                                else continue;
                                if (bt == "mouseMove") bt = "moveMove";
                                if (bt != "moveMouse" && bt != "mouseClick"
                                    && bt != "mouseDown" && bt != "mouseUp") continue;
                                if (st.contains("coordSpace") && st["coordSpace"].is_string()
                                    && st["coordSpace"].get<std::string>() == "screen") continue;
                                if (!st.contains("x") || !st.contains("y")) continue;
                                if (!st["x"].is_number() || !st["y"].is_number()) continue;
                                const int vx = st["x"].get<int>();
                                const int vy = st["y"].get<int>();
                                anyPoint = true;
                                if (vx > 1000 || vy > 1000 || vx < 0 || vy < 0) allWithin1000 = false;
                                if (vx > liveMap.apiWidth - 1 || vy > liveMap.apiHeight - 1) anyOutside = true;
                            }
                            if (anyPoint && anyOutside && allWithin1000) {
                                batchSpace = BatchCoordSpace::Normalized1000;
                                AppendAiDebugLog(L"  [诊断] 本批坐标**超出附图**而全在 0~1000 内 ⇒ "
                                    L"整批统一按 0~1000 归一化解释（避免同批两套空间）");
                            } else if (anyPoint && allWithin1000 && !anyOutside) {
                                AppendAiDebugLog(L"  [诊断] 本批坐标全部落在附图内 ⇒ "
                                    L"整批按 upload 截图像素解释");
                            }
                        }                        for (size_t stepIdx = 0; stepIdx < steps.size(); ++stepIdx) {
                            const auto& step = steps[stepIdx];
                            if (StopRequested()) break;
                            if (!step.is_object()) {
                                ++skippedNoType;
                                if (firstSkipReason.empty())
                                    firstSkipReason = L"第 " + std::to_wstring(stepIdx + 1)
                                        + L" 条不是 JSON 对象（类型是 "
                                        + FromUtf8(step.type_name()) + L"）";
                                continue;
                            }

                            nlohmann::json params;
                            std::wstring actionType;
                            if (step.contains("action")) {
                                actionType = FromUtf8(step["action"].get<std::string>());
                                if (actionType == L"mouseMove") actionType = L"moveMouse";
                                params = step.value("params", nlohmann::json::object());
                                if (!params.is_object()) params = nlohmann::json::object();
                                // ★★ **顶层字段也要并进来**（2026-09-30 实测：16 轮全被跳过）。
                                //
                                //   模型给的形状是 `{"action":"mouseClick","x":168,"y":848}`
                                //   —— 坐标写在**顶层**、没有 `params` 包一层。旧实现只取
                                //   `step["params"]` ⇒ 拼出来的动作**没有坐标** ⇒
                                //   「mouseClick 缺少 x/y」⇒ 每一条都被跳过、模型原样重发、
                                //   一路撞到"批次上限 16 批"收尾（用户看到"没什么反应、就断了"）。
                                //   ⇒ 除形状自身的键（`action`/`params`/`type`）外，**顶层字段全部并入**
                                //     `params`（`params` 里已有的键优先，不被覆盖）。
                                for (auto it = step.begin(); it != step.end(); ++it) {
                                    const std::string& k = it.key();
                                    if (k == "action" || k == "params" || k == "type") continue;
                                    if (!params.contains(k)) params[k] = it.value();
                                }
                                params["type"] = ToUtf8(actionType);
                            } else if (step.contains("type")) {
                                params = step;
                                actionType = FromUtf8(step["type"].get<std::string>());
                            } else {
                                ++skippedNoType;
                                if (firstSkipReason.empty())
                                    firstSkipReason = L"第 " + std::to_wstring(stepIdx + 1)
                                        + L" 条既没有 `type` 也没有 `action` 字段"
                                          L"（键："
                                        + FromUtf8(step.dump().substr(0, 80)) + L"）";
                                continue;
                            }

                            // Agent 闭环：stopMacro 不占步骤预算（构建器常自动追加，否则 10 步很快耗尽）
                            if (actionType == L"stopMacro") {
                                ++skippedStopMacro;
                                continue;
                            }

                            if (!remapStepCoords(params)) {
                                ++skippedBadPointer;
                                continue;
                            }

                            auto built = BuildScriptActionFromJson(params);
                            if (!built.ok) {
                                AppendAiDebugLog(L"  跳过无效动作：" + built.error);
                                ++skippedInvalid;
                                if (firstInvalidError.empty()) firstInvalidError = built.error;
                                continue;
                            }
                            ScriptAction stepAction = InheritAiActionFields(built.action, eff);
                            if (stepAction.type == ActionType::MouseClick
                                && params.contains("x") && params.contains("y")) {
                                try {
                                    int sx = lastActionScreenX, sy = lastActionScreenY;
                                    if (params["x"].is_number()) sx = params["x"].get<int>();
                                    if (params["y"].is_number()) sy = params["y"].get<int>();
                                    // locateAndClick 已带 coordSpace=screen；此处只拦 Agent 盲点
                                    const bool fromLocate =
                                        params.contains("coordSpace") && params["coordSpace"].is_string()
                                        && params["coordSpace"].get<std::string>() == "screen";
                                    std::string btn = "left";
                                    if (params.contains("button") && params["button"].is_string())
                                        btn = params["button"].get<std::string>();
                                    if (!fromLocate) {
                                        notePointerClick(sx, sy);
                                    } else {
                                        lastActionScreenX = sx;
                                        lastActionScreenY = sy;
                                    }
                                } catch (...) {}
                            } else if (stepAction.type == ActionType::MoveMouse
                                && params.contains("x") && params.contains("y")) {
                                try {
                                    if (params["x"].is_number())
                                        lastActionScreenX = params["x"].get<int>();
                                    if (params["y"].is_number())
                                        lastActionScreenY = params["y"].get<int>();
                                } catch (...) {}
                            }
                            if (stepAction.type == ActionType::EndLoop) {
                                if (aiLoopDepth <= 0) {
                                    AppendAiDebugLog(L"  跳过结束循环：" + std::wstring(kEndLoopNeedsLoopParentMsg));
                                    continue;
                                }
                            }

                            if (!ConsumeAiStep(childFrame)) {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：步骤预算已用尽");
                                break;
                            }

                            // ★★批内节拍：批**内**相邻两步之间必须留出一帧（见 kAiBatchStepMinGapMs）。
                            //   少了它，全速连发的两次点击会落进游戏的同一帧，
                            //   「选卡→落点」这一对里的那一次投放就静默丢了（实测 10 次成 9 次）。
                            if (stepCount > 0) {
                                Sleep(kAiBatchStepMinGapMs);
                            }
                            AppendAiDebugLog(L"  执行 " + ActionName(stepAction)
                                + L" (步" + std::to_wstring(stepCount + 1) + L")");                            // 开网页/启动程序：操作前截基线，操作后本地「反应→稳定」二次校验
                            // （无固定死延时；程序冷启动慢时避免观察抢在窗口出现之前）
                            bool isDoubleClickStep = false;
                            if (stepAction.type == ActionType::MouseClick) {
                                try {
                                    if (params.contains("clickCount")
                                        && params["clickCount"].is_number()) {
                                        isDoubleClickStep = params["clickCount"].get<int>() >= 2;
                                    }
                                } catch (...) {}
                            }
                            // 双击通常等同「打开」，同样要等窗口起来再观察
                            const bool wantsLaunchSettle =
                                stepAction.type == ActionType::OpenWebpage
                                || stepAction.type == ActionType::RunProgram
                                || stepAction.type == ActionType::OpenFile
                                || isDoubleClickStep;
                            // 点击/回车/快捷键后界面常要几百毫秒才画完：宿主就地等一下，
                            // 省掉 Agent 为此单开一轮 wait（一轮 = 一次 API + 常带图）
                            bool opensUiKey = false;
                            if (stepAction.type == ActionType::KeyClick) {
                                try {
                                    std::wstring kt;
                                    if (params.contains("keyText") && params["keyText"].is_string())
                                        kt = FromUtf8(params["keyText"].get<std::string>());
                                    for (auto& c : kt) {
                                        if (c >= L'a' && c <= L'z')
                                            c = static_cast<wchar_t>(c - L'a' + L'A');
                                    }
                                    auto flag = [&](const char* k) {
                                        return params.contains(k) && params[k].is_boolean()
                                            && params[k].get<bool>();
                                    };
                                    opensUiKey = kt == L"ENTER" || kt == L"RETURN"
                                        || kt == L"ESCAPE" || kt == L"F5"
                                        || flag("holdLeftCtrl") || flag("holdLeftAlt")
                                        || flag("holdLeftWin");
                                } catch (...) {}
                            }
                            bool wantsInteractionSettle = !wantsLaunchSettle
                                && (stepAction.type == ActionType::MouseClick
                                    || stepAction.type == ActionType::HotkeyShortcut
                                    || opensUiKey);
                            if (wantsInteractionSettle) {
                                for (size_t j = stepIdx + 1; j < steps.size(); ++j) {
                                    if (stepWantsInteractionSettle(steps[j])) {
                                        wantsInteractionSettle = false;
                                        break;
                                    }
                                }
                            }
                            // ★★批**内**前瞻还不够，要看批**外**：本轮（同一条 assistant 消息）
                            //   后面还有没有别的工具调用（docs §72）。
                            //   模型一次并行发 20 个 `mouseClick` 时，**每个工具调用都是独立的一批**
                            //   （各自 1~2 个动作）⇒ 上面那个批内前瞻看不到后面的 19 次点击
                            //   ⇒ 每击一次都要等满一次 settle（游戏画面永远「仍在变化」，
                            //   每次都要等到超时 0.5~1.3s）。
                            //   实测真机日志：11 个工具的一轮 `本地执行 14985ms`、
                            //   16 个工具 `38125ms`、20 个 `25843ms` —— 绝大部分是这一项。
                            //   只有**本轮最后一次**交互才需要等界面稳定（那一次负责给
                            //   整轮一个「界面到底有没有反应」的判决）。
                            if (wantsInteractionSettle) {
                                const int laterToolCalls = AiToolCallsRemainingInRound();
                                if (laterToolCalls > 0) {
                                    wantsInteractionSettle = false;
                                    AppendAiDebugLog(L"  [诊断] 本步不等界面（settle）："
                                        L"本轮后面还有 " + std::to_wstring(laterToolCalls)
                                        + L" 个工具调用，最后一次才等");
                                }
                            }
                            const bool wantsSettle = wantsLaunchSettle || wantsInteractionSettle;
                            HBITMAP settleBaseline = nullptr;
                            if (wantsSettle && !StopRequested()) {
                                int bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
                                if (resolveAiRegion(bx1, by1, bx2, by2)) {
                                    if (wmUsesTarget()) {
                                        settleBaseline = wmExecPtr->CaptureScreenRegionFromWindow(
                                            bx1, by1, bx2, by2,
                                            lockedScreen_, lockedVirtX_, lockedVirtY_);
                                    } else {
                                        settleBaseline = CaptureScreenRegion(bx1, by1, bx2, by2);
                                    }
                                }
                            }
                            // ★★多击批次：在执行**第一个点击之前**留一份基线（docs §60）。
                            //   只在 `!wantsSettle` 时截 —— 那正是「后面还有交互步」的情形，
                            //   也正是这句回执唯一用得上、而旧实现唯一说错的场合
                            //   （`wantsSettle` 那一步的基线本来就是对的，别重复截）。
                            if (!wantsSettle && !firstClickBaseline && !StopRequested()
                                && stepAction.type == ActionType::MouseClick) {
                                int bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
                                if (resolveAiRegion(bx1, by1, bx2, by2)) {
                                    if (wmUsesTarget()) {
                                        firstClickBaseline =
                                            wmExecPtr->CaptureScreenRegionFromWindow(
                                                bx1, by1, bx2, by2,
                                                lockedScreen_, lockedVirtX_, lockedVirtY_);
                                    } else {
                                        firstClickBaseline = CaptureScreenRegion(bx1, by1, bx2, by2);
                                    }
                                    firstClickBlX1 = bx1; firstClickBlY1 = by1;
                                    firstClickBlX2 = bx2; firstClickBlY2 = by2;
                                }
                            }

                            if (stepAction.type == ActionType::AiActionExecute) {
                                // 允许有限嵌套；达上限时 NestGuard 会跳过并打日志
                                runAiActionExecute(stepAction, &eff);
                            } else {
                                executeOne(stepAction);
                            }
                            // ShellExecute 失败时勿假成功：立刻回报，引导走开始菜单搜索兜底
                            if ((stepAction.type == ActionType::RunProgram
                                    || stepAction.type == ActionType::OpenFile
                                    || stepAction.type == ActionType::OpenWebpage)
                                && !LastLaunchProgramError().empty()) {
                                return L"[错误] " + LastLaunchProgramError();
                            }
                            // 记「第一次点击的落点」：点完光标就在那儿，不需要坐标换算
                            if (stepAction.type == ActionType::MouseClick) {
                                ++clickedSteps;
                                if (firstClickScreenX < 0) {
                                    POINT cp{};
                                    if (GetCursorPos(&cp)) {
                                        firstClickScreenX = cp.x;
                                        firstClickScreenY = cp.y;
                                    }
                                }
                                // 记下本批**实际点过的**左键落点：settle 靠它判「落点附近有没有
                                // 局部反应」（没有它，动态画面上 reacted 恒真、结论不可归因）。
                                POINT cpNow{};
                                if (GetCursorPos(&cpNow)) {
                                    bool have = false;
                                    for (const auto& m : clickedPointsThisBatch) {
                                        if (m.x == cpNow.x && m.y == cpNow.y) {
                                            have = true;
                                            break;
                                        }
                                    }
                                    if (!have) clickedPointsThisBatch.push_back(cpNow);
                                }
                            }
                            // 逻辑转化：仅在真正执行成功后记轨迹（避免幽灵步骤）
                            if (AiLogicConvertSessionActive()
                                && stepAction.type != ActionType::AiActionExecute) {
                                AiLogicConvertNoteAction(stepAction);
                            }

                            if (wantsSettle && settleBaseline && !StopRequested()) {
                                int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                                resolveAiRegion(cx1, cy1, cx2, cy2);
                                UiVisualSettleOptions sopt;
                                // ★游戏/自绘前台：画面每帧都在变，等「反应→稳定」既等不到也没意义，
                                // 用户实测每个动作白等 1.5~2.6s（拿卡→放卡中间隔好几秒）。
                                // 只留一个很短的节拍让游戏把这一帧画完。
                                //
                                // ★但「算不算游戏」本身有把握高低之分（游戏覆盖率是抖的），
                                //   所以短节拍按**判断置信度**分档，而不是靠一个布尔：
                                //     · 有结构性证据（无控件树 / 画布页）→ 短节拍（0.9s 封顶）
                                //     · 只有「画面在持续变」→ 不跳，退回交互节拍（2.2s 封顶）
                                //   用 0.55 那条疑似证据去跳掉整段 settle，会在**非游戏**前台
                                //   （视频、动画广告）跳过必要的稳定等待。
                                const bool gameForeground = AiActionGameForegroundLikely();
                                const AiGameForegroundDecision gameGate =
                                    AiLastGameForegroundDecision();
                                const bool gameDecisive =
                                    AiGameForegroundDecisionIsDecisive(gameGate);
                                if (gameDecisive) {
                                    sopt.pollIntervalMs = 80;
                                    sopt.reactDeadlineMs = 350;
                                    sopt.stableHoldMs = 150;
                                    sopt.maxTotalMs = 900;
                                    sopt.refreshSuggestMs = 100000;  // 游戏里别提「建议刷新/重开」
                                } else if (gameForeground) {
                                    // 疑似游戏但证据不足：不按游戏跳 settle，也别按「冷启动」
                                    // 拖满 4.2s —— 取交互档，并在诊断里说清为什么没走短节拍。
                                    sopt.pollIntervalMs = 110;
                                    sopt.reactDeadlineMs = 700;
                                    sopt.stableHoldMs = 260;
                                    sopt.maxTotalMs = 2200;
                                    sopt.refreshSuggestMs = 2200;
                                    AppendAiDebugLog(L"  [诊断] 疑似游戏前台但证据不足，"
                                        L"settle 用交互档（未走短节拍）：" + gameGate.record.why);
                                } else if (wantsLaunchSettle) {
                                    sopt.pollIntervalMs = 150;
                                    sopt.reactDeadlineMs = 2200;
                                    sopt.stableHoldMs = 400;
                                    sopt.maxTotalMs = 4200;
                                    sopt.refreshSuggestMs = 4200;
                                } else if (wantsInteractionSettle) {
                                    // 交互反馈是毫秒级：预算给足 2.2s 就够，别把每步都拖成冷启动
                                    sopt.pollIntervalMs = 110;
                                    sopt.reactDeadlineMs = 700;
                                    sopt.stableHoldMs = 260;
                                    sopt.maxTotalMs = 2200;
                                    sopt.refreshSuggestMs = 2200;
                                }
                                // ★★把「本批真正点在哪」交给 settle（docs §39.4）：
                                //   没有它，settle 只能看「整屏变没变」，而游戏画面每帧都在变
                                //   ⇒ reacted 必真 ⇒ NoReaction 不可达 ⇒ 缓存作废与
                                //   回执的「界面没有变化」全部变成死代码。
                                //   ⚠ 坐标必须换算到**位图局部**（ROI 就是那个空间）。
                                for (const POINT& cp : clickedPointsThisBatch) {
                                    POINT lp{ cp.x - cx1, cp.y - cy1 };
                                    sopt.inputPoints.push_back(lp);
                                }
                                sopt.dynamicForeground = gameForeground;
                                const UiVisualSettleResult settled = WaitUiReactThenSettle(
                                    settleBaseline,
                                    [&]() -> HBITMAP {
                                        if (StopRequested()) return nullptr;
                                        if (wmUsesTarget()) {
                                            return wmExecPtr->CaptureScreenRegionFromWindow(
                                                cx1, cy1, cx2, cy2,
                                                lockedScreen_, lockedVirtX_, lockedVirtY_);
                                        }
                                        return CaptureScreenRegion(cx1, cy1, cx2, cy2);
                                    },
                                    stopFlag_, sopt);
                                DeleteBitmapHandle(settleBaseline);
                                settleBaseline = nullptr;
                                AppendAiDebugLog(L"  [诊断] " + settled.logLine);
                                lastUiSettleHint = settled.agentHint;
                                lastUiSettleSuggestRefresh = settled.suggestRefresh;
                                lastUiSettleElapsedMs = settled.elapsedMs;
                                lastUiSettleReacted = settled.reacted;
                                lastUiSettleConclusive = settled.reactionConclusive;
                                lastUiSettleSettled = settled.settled;
                                // 本批定性结论（供 locateAndClick 回执用，见 AiBatchOutcome）
                                lastBatchOutcome = settled.reacted ? AiBatchOutcome::Reacted
                                                                  : AiBatchOutcome::NoReaction;
                                // 权威的「点完到底有没有反应」：给「错点自纠」这类补点逻辑看
                                NoteAiUiSettleReacted(settled.reacted);
                                if (!settled.reacted) {
                                    settleNoReactionThisBatch = true;
                                    settleNoReactionConclusive = settled.reactionConclusive;
                                }
                                // 播放器大面积在动不算翻页：勿清近点计数，否则会再点一次把开关取消。
                                lastUiChangeRois = settled.lastChangeRois;
                                // ★★本批「落点命中数」：对**每一个**落点问一次同一把尺
                                //   （`AiJudgeUiReaction` 纯本地整数运算，N 个落点 = N 次微秒级调用），
                                //   然后把结果**压成一句**塞进批次回执 —— 不逐点发裁决、不额外请求。
                                batchLandingHits = -1;
                                if (clickedPointsThisBatch.size() >= 2 && cy2 > cy1) {
                                    int hits = 0;
                                    for (const POINT& cp : clickedPointsThisBatch) {
                                        std::vector<POINT> onePt{
                                            POINT{ cp.x - cx1, cp.y - cy1 } };
                                        const AiUiReactionVerdict rv = AiJudgeUiReaction(
                                            settled.lastChangeRois, onePt, gameForeground,
                                            /*nearPx=*/40, cx2 - cx1, cy2 - cy1);
                                        if (rv.kind == AiUiReactionKind::NearInput) ++hits;
                                    }
                                    batchLandingHits = hits;
                                }
                                lastUiRegionX1 = cx1;
                                lastUiRegionY1 = cy1;
                                lastUiRegionW = (std::max)(0, cx2 - cx1);
                                lastUiRegionH = (std::max)(0, cy2 - cy1);
                                // ★逐步生效判定：本批是「多点」时，第一次点击有没有在它附近
                                //   引起**小范围结构变化**（大面积动态区不算，那是播放器/游戏背景）。
                                //   这条对任何界面都成立：先选中/先切换没成功，后面点什么都是白点。
                                //   ⚠⚠ 基线必须是**第一次点击之前**那一张（`firstClickBaseline`）。
                                //   旧实现用的是 `settled.lastChangeRois` —— 那是「最后一次 settle」
                                //   的差分，而它的基线截于第一次点击**之后** ⇒ 第一次点击自己的
                                //   效果根本不在里面 ⇒ 这句回执只要开口就是错的（docs §60）。
                                //   没有可用基线时**如实说「无法归因」**，绝不退回会撒谎的版本
                                //   （与 §40.1 同一形状：无法归因不许升级成「已生效/没生效」）。
                                if (clickedSteps >= 2 && firstClickScreenX >= 0) {
                                    const int lx = firstClickScreenX - cx1;
                                    const int ly = firstClickScreenY - cy1;
                                    // ★★用**同一个判据、同一把尺**问「第一次点击附近有没有反应」
                                    //   （docs §40.1）。旧写法在这里内联了一套
                                    //   `rw<=220 && rh<=180 && area<=48000` 的**绝对**阈值，
                                    //   而 settle 那侧用的是另一套 —— 结果同一批数据写出
                                    //   「有局部变化落在落点附近 → 算反应」与
                                    //   「首次点击附近无变化（整屏无小范围变化）」两句**互相拆台**的话。
                                    //   实测那个被判「大面积」的真反应是 263×104（占画面 0.74%）。
                                    std::vector<POINT> firstPt{ POINT{ lx, ly } };
                                    const std::wstring firstPtText = DescribeClickPointForModel(
                                        firstClickScreenX, firstClickScreenY,
                                        liveMapValid ? &liveMap : nullptr);
                                    // 基线与 settle 必须是**同一个区域**，否则坐标对不上（宁可不判）
                                    const bool blUsable = firstClickBaseline != nullptr
                                        && settled.lastFrame != nullptr
                                        && firstClickBlX1 == cx1 && firstClickBlY1 == cy1
                                        && firstClickBlX2 == cx2 && firstClickBlY2 == cy2;
                                    if (!blUsable) {
                                        stepEffectFact = L"\n[事实] 本批点了 "
                                            + std::to_wstring(clickedSteps)
                                            + L" 次，第一次点击" + firstPtText
                                            + L"发生在后续步骤之前，而本地没有留下那一刻的画面"
                                              L"基线 ⇒ **无法判断**它有没有生效（本句不猜）。"
                                              L"★后面的步骤都建立在「它生效了」这个前提上。";
                                        AppendAiDebugLog(L"  [诊断] 批量逐步校验：点击 "
                                            + std::to_wstring(clickedSteps)
                                            + L" 次，但没有第一次点击之前的基线 ⇒ 如实报「无法归因」");
                                    } else {
                                        // 观察窗口 = 「第一次点击之前」→「本批结束」，所以落点附近
                                        // 的变化也可能是**后面某一步**造成的；这句话如实标出这一点。
                                        const ScreenChangeDiffResult fd = DiffBitmapsChangedRegions(
                                            firstClickBaseline, settled.lastFrame,
                                            /*channelTol=*/12);
                                        const AiUiReactionVerdict firstRv = AiJudgeUiReaction(
                                            fd.rois, firstPt, gameForeground,
                                            /*nearPx=*/40, cx2 - cx1, cy2 - cy1);
                                        // 变量名勿用 near/far/small（Windows 旧头历史宏：#define small char）
                                        const bool nearHit =
                                            firstRv.kind == AiUiReactionKind::NearInput;
                                        const bool sawSmallRoi =
                                            firstRv.kind != AiUiReactionKind::LargeMotionOnly
                                            && firstRv.kind != AiUiReactionKind::None;
                                        if (!nearHit && sawSmallRoi) {
                                            // ⚠ 旧文案在这句末尾教模型「或再点一次这一步」——
                                            //   那是**引擎替模型出策略**，而且在 toggle 式界面
                                            //   （选卡/开关）上直接有害：再点一次同一张卡 = 取消选择。
                                            //   引擎只说事实，怎么确认交给模型（决策链三条）。
                                            stepEffectFact = L"\n[事实] 本批**第一次点击**"
                                                + firstPtText
                                                + L"附近没有任何局部变化（自第一次点击之前到本批"
                                                  L"结束的差分；局部变化都发生在别处，动态画面上"
                                                  L"那多半是画面自身在动）⇒ **无法归因**到这一步"
                                                  L"（可能点错了/没选中/不可点）。"
                                                  L"★后面的步骤都建立在「它生效了」这个前提上。"
                                                  L"批量做「先选中/先切换 → 再作用于目标」这类链条时"
                                                  L"可改用 locateAndClick(targets=[…])"
                                                  L"（逐步校验、失败即停）。";
                                        } else if (!nearHit && !sawSmallRoi && !settled.reacted) {
                                            stepEffectFact = L"\n[事实] 本批点了 "
                                                + std::to_wstring(clickedSteps)
                                                + L" 次但整屏没有可归因的变化：很可能一步都没生效。"
                                                  L"先确认第一步（选中/切换）的状态，别继续往下堆动作。";
                                        } else if (!nearHit && settled.reacted) {
                                            // ★★ **补上"有反应但无法归因"这个空洞**（2026-10-01 实测）。
                                            //
                                            //   实测：`settle` 明说「已稳定 变化区 6 个（局部 6）」
                                            //   （整批**确实**有反应），但 `nearHit=false` 且
                                            //   `sawSmallRoi=false` ⇒ 上面两个分支**都不命中**
                                            //   ⇒ 模型**一条效果事实都收不到** ⇒ 只能继续瞎试同一处坐标，
                                            //   一路撞到 16 批上限（用户看到"咋这就断了"）。
                                            //   ⇒ 如实说明"有反应但归因不了"，并给**可执行的替代**，别留空。
                                            stepEffectFact = L"\n[事实] 本批**确实**让界面变了"
                                                L"（整批差分有局部反应），但**无法归因到某一次点击**"
                                                L"（变化可能来自后续步骤，或画面自身在动）⇒ "
                                                L"**别原地重发同一坐标**。要确认目标状态，"
                                                L"用 observePage 看控件树、或 zoom 放大那一小块；"
                                                L"知道名字就直接 locateAndClick(target=\"名字\")。";
                                        }
                                        AppendAiDebugLog(L"  [诊断] 批量逐步校验：点击 "
                                            + std::to_wstring(clickedSteps) + L" 次（基线=第一次点击"
                                              L"之前，与 settle 同区域），首次点击"
                                            + (nearHit ? L"附近已变" : L"附近无变化")
                                            + (sawSmallRoi ? L"" : L"（整屏无局部变化）")
                                            + L"；" + firstRv.why);
                                    }
                                }
                                lastUiChangeRoisText.clear();
                                for (size_t i = 0; i < settled.lastChangeRois.size() && i < 4; ++i) {
                                    const auto& r = settled.lastChangeRois[i];
                                    if (!lastUiChangeRoisText.empty()) lastUiChangeRoisText += L";";
                                    lastUiChangeRoisText += std::to_wstring(r.x1) + L","
                                        + std::to_wstring(r.y1) + L","
                                        + std::to_wstring(r.x2) + L","
                                        + std::to_wstring(r.y2);
                                }
                                if (settled.lastFrame) {
                                    CommitSavedImage(settled.lastFrame, kAiObsImageVarName,
                                        imageVarRunId, imageVars_);
                                    DeleteBitmapHandle(settled.lastFrame);
                                    // ★★这一帧是**动作之后**的画面，但**本轮并没有上传给模型**
                                    //   （它只是被存下来当"下一步比对的基线"）⇒ 必须记账（docs §64）。
                                    obsImageSeenByModel = false;
                                    // 交互后界面没动就别强推图：交给差分决定，省一张截图的钱
                                    if (wantsLaunchSettle || settled.reacted)
                                        forceNextObserveUpload = true;
                                }
                            } else if (settleBaseline) {
                                DeleteBitmapHandle(settleBaseline);
                            }
                            // ★★ 宏路径的 openWebpage 也要**记账网址**（2026-09-30）：
                            //   否则紧随其后的 searchOnPage 会说"还不知道当前站点"并反复重试
                            //   （实测连报 5 轮，用户看到"原地打转"）。工具路径早就在记了
                            //   （macro_execute_tools.cpp 的 openWebpage 工具），这里补上回放侧。
                            if (stepAction.type == ActionType::OpenWebpage) {
                                const std::wstring opened = stepAction.targetPath.empty()
                                    ? stepAction.inputText : stepAction.targetPath;
                                if (!opened.empty()) {
                                    AiNoteLastOpenWebpageUrl(opened);
                                    AppendAiDebugLog(L"  [诊断] 记账已打开网页：" + opened
                                        + L"（供 searchOnPage 解析当前站点）");
                                }
                            }
                            if ((stepAction.type == ActionType::OpenWebpage
                                    || stepAction.type == ActionType::RunProgram)
                                && !windowmode::ExtBridgeServer::Instance().IsExtensionConnected()) {
                                std::wstring hay = stepAction.targetPath + L" " + stepAction.inputText;
                                for (auto& c : hay) {
                                    if (c >= L'A' && c <= L'Z')
                                        c = static_cast<wchar_t>(c - L'A' + L'a');
                                }
                                const bool browserish = stepAction.type == ActionType::OpenWebpage
                                    || hay.find(L"msedge") != std::wstring::npos
                                    || hay.find(L"chrome") != std::wstring::npos
                                    || hay.find(L"firefox") != std::wstring::npos
                                    || hay.find(L"brave") != std::wstring::npos
                                    || hay.find(L"iexplore") != std::wstring::npos
                                    || (hay.find(L"edge") != std::wstring::npos
                                        && hay.find(L"edgedriver") == std::wstring::npos);
                                if (browserish) {
                                    auto& launchBridge = windowmode::ExtBridgeServer::Instance();
                                    launchBridge.RefreshDiscovery();
                                    AppendAiDebugLog(L"  [诊断] 打开浏览器/网页后等待配套扩展 port="
                                        + std::to_wstring(launchBridge.Port()) + L"…");
                                    std::wstring waitErr;
                                    if (launchBridge.WaitForExtension(10000, waitErr)) {
                                        AppendAiDebugLog(L"  [诊断] 扩展已连接本机桥");
                                    } else {
                                        AppendAiDebugLog(L"  [诊断] 扩展 10s 未握手（页导航应唤醒 SW；"
                                            L"未装扩展则后续 observePage 会走识图）");
                                    }
                                }
                            }
                            if (pendingBreakLoop) break;
                            ++stepCount;
                        }
                        // 点击/移标后把光标泊到角落，降低 hover 对后续观察的干扰
                        if (lastActionScreenX >= 0)
                            parkCursorAwayFromUi();
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：本批完成 "
                            + std::to_wstring(stepCount) + L" 步");
                        // 模态框（尤其覆盖确认）像素差分极小，会被「界面未变」吞掉让模型瞎猜。
                        // 这里用窗口枚举直报，并强制下一轮上传画面。
                        if (stepCount > 0) {
                            const windowmode::ForegroundDialogInfo dlg =
                                windowmode::ProbeForegroundDialog();
                            if (dlg.present) {
                                forceNextObserveUpload = true;
                                std::wstring note = L"\n[对话框] kind=" + dlg.kind
                                    + L"；" + dlg.title;
                                if (!dlg.buttons.empty()) note += L"；按钮：" + dlg.buttons;
                                AppendAiDebugLog(L"  [诊断] 前台对话框：" + dlg.kind + L" "
                                    + dlg.title);
                                altNote += note;
                            }
                        }
                        if (stepCount == 0 && skippedBadPointer > 0) {
                            return L"[错误] 坐标越界已拒绝 "
                                + std::to_wstring(skippedBadPointer)
                                + L" 步；请用 locateAndClick，勿猜绝对坐标" + altNote;
                        }
                        if (stepCount == 0 && skippedInvalid > 0) {
                            return L"[错误] 本批 0 步：" + firstInvalidError
                                + (skippedInvalid > 1
                                    ? (L"（另有 " + std::to_wstring(skippedInvalid - 1)
                                        + L" 个无效动作）")
                                    : L"")
                                + altNote;
                        }
                        if (stepCount == 0) {
                            // ★★不许只回一句「空数组或全部被跳过」（docs §72）：把**实际**原因点名。
                            std::wstring why;
                            if (!firstSkipReason.empty()) {
                                why = L"本批共 " + std::to_wstring(steps.size())
                                    + L" 条，全部被跳过：" + firstSkipReason;
                                if (skippedStopMacro > 0) {
                                    why += L"；其中 " + std::to_wstring(skippedStopMacro)
                                        + L" 条是自动追加的 stopMacro";
                                }
                            } else if (skippedStopMacro > 0
                                && skippedStopMacro == static_cast<int>(steps.size())) {
                                why = L"本批共 " + std::to_wstring(steps.size())
                                    + L" 条**全是**「自动追加的 stopMacro」，里面**一条真实动作"
                                      L"都没有** —— 这不是模型的错，是**动作在进入执行前就被丢掉**"
                                      L"（实现缺陷）。请把这条回执原文报给用户，别照着重点。";
                            } else if (steps.empty()) {
                                why = L"动作数组是空的";
                            } else {
                                // ⚠ 剩这条的唯一可能是「循环第一步就被中断」（紧急停止 / 脚本
                                //   结束 / 步骤预算）—— 那几条**各自有日志**。这里绝不许说
                                //   「空数组」（数组明明有 N 条），那正是回执说谎。
                                why = L"本批共 " + std::to_wstring(steps.size())
                                    + L" 条，但**一条都没进入执行**（执行前就被中断："
                                      L"紧急停止 / 脚本结束 / 步骤预算，原因见同批日志）。"
                                      L"请把这条回执原文报给用户。";
                            }
                            return L"[错误] 本批 0 步：" + why + altNote;
                        }
                        std::wstring settleFact;
                        if (settleNoReactionThisBatch) {
                            lastBatchOutcome = AiBatchOutcome::NoReaction;
                            // ★★措辞按「确不确定」分岔（docs §40.1）：动态画面上画面自己在动，
                            //   我们只能得出「无法归因」，**不能**说成「确定没生效」——
                            //   说错了会诱导模型重做已经做成的步骤。
                            settleFact = settleNoReactionConclusive
                                ? L"\n[事实] settle无反应：界面相对操作前**一动都没动**（确定没生效）。"
                                // ★只报事实，**不给做法**（用户定的口径：引擎 = 感知 + 执行 +
                                //   如实回执；「接下来该怎么点」是 Skill 的建议，不是引擎的
                                //   祈使句）。旧文案末尾还跟着「不要反复点同一处，先 screenshot
                                //   看清当前状态，或换个落点/换个描述」—— 那是引擎在替模型定
                                //   策略，而且在**自绘/游戏画面上必然逐击出现**（这类画面每帧
                                //   都在动，本地永远归因不了），实测模型就照着它每点一次都去
                                //   补一张截图验收，白烧一轮又一轮；何况动作执行**本来就**每轮
                                //   回传观察帧，「先 screenshot」既不必要也是多余动作。
                                : L"\n[事实] settle无法归因：画面自己在动，落点附近**没有**任何局部"
                                  L"变化（别处那些局部变化是画面自身在动，不能算作「这一击生效了」）。"
                                  L"**这一步有没有生效，本地判不出来**——画面每帧都在变的界面上，"
                                  L"本地只能测出「没有正证据」，测不出「有没有生效」。";
                            // ★★下面全是**惩罚性**动作（作废文字直点 OCR 索引 / 元素索引）—— 只在
                            //   「**确定**没反应」时做（docs §40.1）。动态画面上「画面自己在动」
                            //   只能得出「无法归因」：拿它去作废这些索引，
                            //   会把**正确的**坐标永久判死（§36.6）。
                            if (settleNoReactionConclusive) {
                            // 文字直点：点的是「上一次观察」的 OCR 坐标，界面没动就说明
                            // 那张索引已经不对了 → 整表作废，逼下一次重新 OCR。
                            // （索引本来有 90s TTL —— 对「点了没反应」这种情况太宽松。）
                            if (aiOcrDirectIndexThisBatch) {
                                ResetOcrScreenIndex();
                                aiOcrDirectIndexThisBatch = false;
                                AppendAiDebugLog(
                                    L"  [诊断] 文字直点后界面无反应 → OCR 屏幕索引已作废");
                            }
                            // 元素索引（UIA∪OCR 查表直点）同理：点了没反应 → 整表作废，
                            // 逼下一次重新枚举。不这么做的话，一个错坐标会被反复查表复用，
                            // 而这条捷径**绕过了识图链路里的「点了没反应就记账」**（它正是靠提前
                            // return 省事的）—— §27.1 同一口坑，别踩第二次。
                            if (aiElementIndexThisBatch) {
                                aiElementIndexThisFrame.clear();
                        aiDomClickedKeys.clear();   // 每次 AI 动作重新计数
                                aiElementIndexThisBatch = false;
                                AppendAiDebugLog(
                                    L"  [诊断] 元素索引直点后界面无反应 → 索引已作废，下次重新枚举");
                            }
                            } else {
                                AppendAiDebugLog(
                                    L"  [诊断] settle 无正证据但**无法归因**（画面自己在动）"
                                    L"→ 不作废文字直点/元素索引（只影响回执措辞）");
                            }
                        }
                        settleFact += stepEffectFact;
                        // ★★「这一批到底有没有生效」必须**每一次**都告诉模型（docs §36 / §54）：
                        //   这句原先只长在 `locateAndClick` 的回执里 ⇒ 模型改用**手算坐标**的
                        //   `mouseClick` 时只拿回「已执行:mouseClick」——既没有坐标也没有结果，
                        //   它无法判断卡片有没有被选中，只能反复点、反复开合同一个面板
                        //   （用户主诉：「光选卡，选完卡咋不会放僵尸」）。
                        //   现在放在**批次**这一层：无论走哪个入口（mouseClick / computer /
                        //   submitMacroActions / locateAndClick）都恰好说一次。判据用
                        //   `lastBatchOutcome` 枚举，**不解析文本**（§36.2）。
                        switch (lastBatchOutcome) {
                        case AiBatchOutcome::Reacted:
                            settleFact += L"\n[结果] 界面已经变化 → 这一步已经生效，不要重做。";
                            break;
                        case AiBatchOutcome::NoReaction:
                            // 与上面的 `[事实]` 同一把尺：动态画面上只能报「无法确认」。
                            settleFact += settleNoReactionConclusive
                                ? L"\n[结果] 界面一动都没动 → 这一击确定没生效。"
                                : L"\n[结果] 无法确认这一击是否生效（画面一直在动，"
                                  L"落点附近没有任何局部变化）。";
                            break;
                        case AiBatchOutcome::Unknown:
                        default:
                            break;
                        }
                        // ★★多落点批次补**一句**宾语（docs §58）：本批 N 个落点里几个附近
                        //   真的有局部变化。它不替模型下结论（所以措辞只说「有/没有局部变化」），
                        //   只把「投丢了」和「投了没用」分开 —— 这两种情况在此之前**长得一模一样**。
                        //   ⚠ 整段只加**一行**、零 API 调用：`batchLandingHits` 是本地像素比较的结果。
                        if (batchLandingHits >= 0) {
                            settleFact += L"\n[事实] 本批 " 
                                + std::to_wstring(clickedPointsThisBatch.size())
                                + L" 个落点里 " + std::to_wstring(batchLandingHits)
                                + L" 个附近有局部变化（同一批只比较一次，不额外识图）："
                                + (batchLandingHits == 0
                                    ? L"一个都没有 ⇒ 这一批很可能**根本没落地**，先确认界面/节拍再重投。"
                                    : (static_cast<size_t>(batchLandingHits) * 2
                                            >= clickedPointsThisBatch.size()
                                        ? L"多数有 ⇒ 落点是落下了，但目标状态没变 ⇒ 该换打法，别重投同一处。"
                                        : L"只有少数有 ⇒ 部分落点没生效，别整批重投。"));
                        }
                        // ★★ **最后一个落点"点到了什么"**（2026-10-01 实测：模型在同一处连点 14 次）。
                        //
                        //   现象：模型每轮发 `mouseClick(171,911)`（屏幕≈(437,1310)），回执只说
                        //   "已执行 1 步"，它**不知道自己点中的是答案选项还是空白** ⇒ 原样重发、
                        //   一路撞到 16 批上限收尾（用户看到"还是断"、白烧 2~3 分钟）。
                        //   画面差分对 1~2px 的单选钮/复选框太不敏感，"附近有没有变化"区分不出来。
                        //   ⇒ 在**引擎**这里补一句**元素事实**：这一刀只能放引擎 ——
                        //     屏幕坐标与 `ProbeUiElementAtPoint` 都在这边；工具层两样都拿不到
                        //     （我在工具层试过，需要 upload→屏幕的映射，那是引擎持有的状态）。
                        //   ⚠ 只陈述事实（有什么控件 / 什么都没有），不替模型判断该点哪儿。
                        if (!clickedPointsThisBatch.empty()) {
                            const POINT& lastPt = clickedPointsThisBatch.back();
                            const windowmode::UiElementState landState =
                                windowmode::ProbeUiElementAtPoint(lastPt.x, lastPt.y);
                            settleFact += L"\n[事实] 最后一个落点 屏幕("
                                + std::to_wstring(lastPt.x) + L"," + std::to_wstring(lastPt.y)
                                + L") 上的元素：";
                            if (landState.probed && !landState.name.empty()) {
                                settleFact += landState.name;
                                if (!landState.controlType.empty())
                                    settleFact += L"[" + landState.controlType + L"]";
                            } else if (landState.probed) {
                                settleFact += L"(有控件但系统没给名字)";
                            } else {
                                settleFact += L"**探测不到可交互控件**（很可能点在空白/图片上）"
                                              L" ⇒ 别在这一处重复点：换描述用 locateAndClick，"
                                              L"或先 computer(screenshot) 取一帧新画面再定位。";
                            }
                        }
                        if (skippedBadPointer > 0) {
                            return L"已执行 " + std::to_wstring(stepCount) + L" 步；另跳过越界坐标 "
                                + std::to_wstring(skippedBadPointer) + L" 步" + altNote
                                + settleFact;
                        }
                        if (skippedInvalid > 0) {
                            return L"已执行 " + std::to_wstring(stepCount)
                                + L" 步；另跳过无效动作 "
                                + std::to_wstring(skippedInvalid) + L" 个："
                                + firstInvalidError + altNote + settleFact + coordApiNote;
                        }
                        return L"已执行 " + std::to_wstring(stepCount) + L" 步" + altNote
                            + settleFact + coordApiNote;
                    } catch (const std::exception& e) {
                        // 走到这里说明 JSON 是好的，是**执行**环节抛了异常。
                        // 旧代码一律回「JSON 解析失败」，把模型和排查都带进沟里
                        //（实测 runCommand 的失败就是这么被误报的）。
                        const std::wstring what = FromUtf8(std::string(e.what()));
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：执行动作时异常：" + what);
                        return L"[错误] 执行动作时异常：" + what
                            + L"。已执行过的步骤不会回滚；请改参数重试，或换路线"
                              L"（runCommand / clickRef / invokeUiControl）。";
                    } catch (...) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：执行动作时未知异常");
                        return L"[错误] 执行动作时未知异常。请换路线或用 observePage 看当前状态。";
                    }
                };

                // maxLongEdge：观察默认 768（省 token）；locate 用 1152
                /// 组一帧落点标注：没点过/区域非法都返回空标注（宁可不标也不编造位置）
                auto makeFrameClickMark = [&](int capX1, int capY1, int capX2, int capY2)
                    -> AiFrameClickMark {
                    AiFrameClickMark m;
                    if (clickMarkX < 0 || clickMarkY < 0) return m;
                    if (capX2 <= capX1 || capY2 <= capY1) return m;
                    m.screenX = clickMarkX;
                    m.screenY = clickMarkY;
                    m.capX1 = capX1;
                    m.capY1 = capY1;
                    m.capX2 = capX2;
                    m.capY2 = capY2;
                    // 标签口径只有一处（ai_action_service）：图上画的和提示词说的同一句话
                    m.label = CurrentAiActionClickMarkLabel();
                    return m;
                };
                auto captureObservationNow = [&](std::string& outB64, int& outW, int& outH,
                    int maxLongEdge = 768, double scaleOverride = 0.0) -> bool {
                    outB64.clear();
                    outW = outH = 0;
                    // ★★ **观察帧的唯一收口：先确保"任务所在的窗口"在前台**（2026-09-30 抽共享 helper）。
                    //
                    //   为什么放在这里：观察帧是**整屏截图**，模型看到什么完全取决于
                    //   "此刻谁在前台"。实测两类事故都由此而来：
                    //     ① 我们自己的调试窗抢了前台 ⇒ 模型看到的是调试窗口（已另行从根上修掉：
                    //        调试窗改为 SW_SHOWNOACTIVATE、不再 ForceForegroundWindow）；
                    //     ② **用户切走了窗口**（去回消息/看别的页）⇒ 模型看到的是别人，
                    //        于是靠记忆猜坐标、连发十几轮（用户体感"莫名断了"）。
                    //   ⇒ 判据只用事实：**扩展贴着某个网页**（`AiLastPageUrl()` 非空）
                    //     而前台不是浏览器 ⇒ 把浏览器切回前台再截。
                    //   ⚠ 与识图定位那条路**共用同一个 helper**（`EnsureTaskWindowForeground`），
                    //     不许再抄第二份（同一逻辑两份必然漂移 —— 本仓同类教训已多次）。
                    {
                        std::wstring fgNote;
                        EnsureTaskWindowForeground(&fgNote);
                        if (!fgNote.empty()) AppendAiDebugLog(L"  [诊断] " + fgNote);
                    }
                    // ★★「半天没反应」的取证：观察链路**逐阶段留面包屑**（docs §66）。
                    //   起因（真机日志）：一次运行的最后一行是 `第 1 轮耗时`，之后**再无任何输出**，
                    //   而观察阶段（隐壳 → 截图 → 编码 → OCR → 建索引）**一行阶段日志都没有** ⇒
                    //   卡在哪一步**无从判断**。开源 CUA 的通用做法就是给每个可能阻塞的阶段
                    //   打「进入 / 离开 + 耗时」——挂住时**最后一条面包屑就是答案**。
                    //   ⚠ 只记**阶段边界**（每帧 5 条），不记进度，不刷屏。
                    ULONGLONG obsT0 = GetTickCount64();
                    AppendAiDebugLog(L"  [诊断] 观察阶段 ① 隐壳+让开光标…");
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    parkCursorAwayFromUi();
                    Sleep(40);
                    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                    if (!resolveAiRegion(cx1, cy1, cx2, cy2)) return false;
                    AppendAiDebugLog(L"  [诊断] 观察阶段 ② 截图（已耗 "
                        + std::to_wstring(GetTickCount64() - obsT0) + L"ms）…");
                    // 定位诊断：打印截图区域与前台窗口位置，便于判断截图是否盖住目标
                    // （如窗口底部的输入框被截在画面外）。
                    {
                        wchar_t title[256]{};
                        HWND fg = GetForegroundWindow();
                        RECT fr{};
                        if (fg) {
                            GetWindowTextW(fg, title, 256);
                            GetWindowRect(fg, &fr);
                        }
                        AppendAiDebugLog(L"  [诊断] 截图区域=("
                            + std::to_wstring(cx1) + L"," + std::to_wstring(cy1)
                            + L")-(" + std::to_wstring(cx2) + L"," + std::to_wstring(cy2)
                            + L") 前台窗口=「" + std::wstring(title) + L"」rect=("
                            + std::to_wstring(fr.left) + L"," + std::to_wstring(fr.top)
                            + L")-(" + std::to_wstring(fr.right) + L","
                            + std::to_wstring(fr.bottom)
                            + L") 窗口/后台窗口模式=" + (wmUsesTarget() ? L"是" : L"否"));
                    }
                    HBITMAP bmp = nullptr;
                    if (wmUsesTarget()) {
                        bmp = wmExecPtr->CaptureScreenRegionFromWindow(
                            cx1, cy1, cx2, cy2, lockedScreen_, lockedVirtX_, lockedVirtY_);
                    } else {
                        bmp = CaptureAiRegionComposed(cx1, cy1, cx2, cy2);
                    }
                    if (!bmp) return false;
                    AppendAiDebugLog(L"  [诊断] 观察阶段 ③ 编码（已耗 "
                        + std::to_wstring(GetTickCount64() - obsT0) + L"ms）…");
                    CommitSavedImage(bmp, kAiObsImageVarName, imageVarRunId, imageVars_);
                    // 定位等视觉关键路径可用 scaleOverride 强制高清（默认仍跟 aiImageScale）
                    const double scale = scaleOverride > 0.0
                        ? std::clamp(scaleOverride, 0.1, 1.0)
                        : std::clamp(
                            eff.aiImageScale > 0.0 ? eff.aiImageScale : 0.5, 0.1, 1.0);
                    // 输入法状态叠加到截图：让 Agent 直接「看图」识别中/英文与组字残留，
                    // 而非靠盲猜或乱按快捷键。TSF 输入法候选框 CAPTUREBLT 拍不到，
                    // 画到图上是最可靠的感知途径。仅中文模式时叠加（避免每帧红字遮挡左上角）。
                    std::wstring imeStatus;
                    if (!wmUsesTarget()) {
                        imeStatus = QueryForegroundImeStatusText();
                        if (imeStatus.find(L"[输入法] 英文") != std::wstring::npos)
                            imeStatus.clear();
                    }
                    // 观察帧落点标注：把「上一次真的点在哪」画进去（没点过则空标注）
                    const AiFrameClickMark frameClickMark =
                        makeFrameClickMark(cx1, cy1, cx2, cy2);
                    const AiImageEncodeResult enc = EncodeBitmapForAiAnalysis(
                        bmp, scale, maxLongEdge, imeStatus.empty() ? nullptr : &imeStatus,
                        &frameClickMark);
                    DeleteBitmapHandle(bmp);
                    if (enc.base64.empty()) return false;
                    outB64 = enc.base64;
                    outW = enc.outWidth;
                    outH = enc.outHeight;
                    liveMap.capX1 = cx1;
                    liveMap.capY1 = cy1;
                    liveMap.capX2 = cx2;
                    liveMap.capY2 = cy2;
                    liveMap.srcWidth = enc.srcWidth;
                    liveMap.srcHeight = enc.srcHeight;
                    liveMap.apiWidth = enc.outWidth;
                    liveMap.apiHeight = enc.outHeight;
                    liveMapValid = liveMap.apiWidth > 0 && liveMap.capX2 > liveMap.capX1;
                    AppendAiDebugLog(L"  [诊断] 观察阶段 ④ 完成 " 
                        + std::to_wstring(GetTickCount64() - obsT0) + L"ms（隐壳+截图+编码）");
                    return true;
                };

                auto captureAiRegionBmp = [&](int cx1, int cy1, int cx2, int cy2) -> HBITMAP {
                    if (wmUsesTarget()) {
                        return wmExecPtr->CaptureScreenRegionFromWindow(
                            cx1, cy1, cx2, cy2, lockedScreen_, lockedVirtX_, lockedVirtY_);
                    }
                    return CaptureAiRegionComposed(cx1, cy1, cx2, cy2);
                };

                /// 智能观察：三帧标动态区 → 结构差分；视频播放不触发反复上传
                auto observeScreenForAgent = [&](bool forceRefresh) -> AiObserveCaptureResult {
                    // Alt+Tab 预览期间勿藏窗/挪标，否则切换器会关掉
                    std::unique_ptr<qst::desktop_tools::ScopedHideOwnUiForCapture> hideOwn;
                    if (!altTabAltHeld) {
                        hideOwn = std::make_unique<qst::desktop_tools::ScopedHideOwnUiForCapture>(
                            UserFacingMainHwnd());
                        parkCursorAwayFromUi();
                    }
                    AiObserveCaptureResult r;
                    // ★前台是模态对话框（另存为/打开/确认）时，直接把「这是什么、该怎么收尾」
                    // 写进本轮指令。模型看不到窗口类，只能靠这条：实测另存为框弹出来后，
                    // 模型不知道那是保存框，去 locateAndClick「更多选项」、又反复 activateWindow
                    // 切窗（模态框会挡住 SetForegroundWindow，必然失败），整轮卡死。
                    {
                        const std::wstring probe = windowmode::FormatForegroundDialogProbe();
                        auto field = [&probe](const wchar_t* key) -> std::wstring {
                            const std::wstring needle = std::wstring(key) + L"=";
                            const size_t p = probe.find(needle);
                            if (p == std::wstring::npos) return {};
                            const size_t from = p + needle.size();
                            const size_t end = probe.find(L';', from);
                            return probe.substr(from,
                                end == std::wstring::npos ? std::wstring::npos : end - from);
                        };
                        const std::wstring kind = field(L"kind");
                        const std::wstring dlgTitle = field(L"title");
                        if (!kind.empty() && kind != L"none") {
                            const bool saveLike = kind == L"saveAs" || kind == L"saveAsMini";
                            const bool openLike = kind == L"openFile";
                            std::wstring f = L"★前台是一个**模态对话框**";
                            if (!dlgTitle.empty()) f += L"「" + dlgTitle + L"」";
                            f += L"（宿主探测 kind=" + kind + L"）：";
                            if (saveLike) {
                                f += L"这是保存/另存为框。收尾办法："
                                     L"① 用 quickInput 把**文件名**（或完整路径）打进文件名框 → "
                                     L"② keyClick(Enter) 或 locateAndClick(保存) 提交；"
                                     L"想放弃才 Escape。可以点左侧「桌面」再输入纯文件名。"
                                     L"★不要 locateAndClick「更多选项」等菜单、不要 activateWindow 切窗"
                                     L"（模态框挡着一定切不动）、不要 F12/切任务栏。";
                            } else if (openLike) {
                                f += L"这是打开框：quickInput 文件名 → Enter；或 Escape 取消后改用 openFile(路径)。"
                                     L"不要切窗/点更多选项。";
                            } else {
                                f += L"先看图处理它（确定/关闭/取消），再继续任务；"
                                     L"不要 Escape 关掉未完成的对话框，也不要反复切窗。";
                            }
                            r.foregroundFact = f;
                            AppendAiDebugLog(L"  [诊断] ★前台模态对话框 kind=" + kind
                                + (dlgTitle.empty() ? L"" : (L" title=" + dlgTitle))
                                + L" → 本轮注入处理指引");
                        }
                    }
                    if (!lastUiSettleHint.empty()) {
                        r.settleChecked = true;
                        r.uiReacted = lastUiSettleReacted;
                        r.uiSettled = lastUiSettleSettled;
                        r.suggestRefresh = lastUiSettleSuggestRefresh;
                        r.settleElapsedMs = lastUiSettleElapsedMs;
                        r.settleHint = lastUiSettleHint;
                        r.changeRoisText = lastUiChangeRoisText;
                        lastUiSettleHint.clear();
                    }
                    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                    if (!resolveAiRegion(cx1, cy1, cx2, cy2)) return r;

                    // 三帧短采样：标出持续运动格子（视频），再与 aiObs 做结构差分
                    HBITMAP f0 = captureAiRegionBmp(cx1, cy1, cx2, cy2);
                    if (!f0) return r;
                    Sleep(90);
                    HBITMAP f1 = captureAiRegionBmp(cx1, cy1, cx2, cy2);
                    Sleep(90);
                    HBITMAP f2 = captureAiRegionBmp(cx1, cy1, cx2, cy2);
                    if (!f1 || !f2) {
                        if (f1) DeleteBitmapHandle(f1);
                        if (f2) DeleteBitmapHandle(f2);
                        // 退化：单帧
                        f1 = f2 = nullptr;
                    }

                    ScreenBusyMask busy{};
                    HBITMAP bmp = f2 ? f2 : f0;
                    if (f1 && f2) {
                        busy = BuildBusyMaskFromTripleFrames(f0, f1, f2, 12, 32, 0.08);
                        DeleteBitmapHandle(f0);
                        DeleteBitmapHandle(f1);
                        f0 = f1 = nullptr;
                        // f2 作为当前帧
                    } else {
                        bmp = f0;
                        f0 = nullptr;
                    }

                    // ★模型明确要过截图（computer(action=screenshot)）→ 必须回传这一帧：
                    //   历史里的旧图已被剥成「(历史截图已省略)」，用「界面未变」省掉这帧
                    //   等于把模型变成瞎子（实测它反复自问「我看不到图」并空转好几轮）。
                    //
                    // ★★「界面未变」只有在**模型已经看过基线那一帧**时才是真话（docs §64）。
                    //   真机事故（落盘日志）：模型点开「自选僵尸卡牌」，面板整块换了，
                    //   但动态画面上「大面积变化」不算反应 ⇒ `settled.reacted=false`
                    //   ⇒ 不强制上传；而 settle 的末帧被存成了比对基线（它 **包含** 面板）
                    //   ⇒ 下一轮比对"自己和自己" ⇒ 0.000000% ⇒ 回执写「界面未变，跳过上传」。
                    //   **模型手上最新的图还是动作之前那一张** ⇒ 它永远看不到自己动作的结果
                    //   ⇒ 只能反复 zoom / 反复猜「我点上了吗」，实测白烧 11 轮。
                    //   ⇒ 判据改成「基线这一帧给模型看过了吗」，没看过就必须上传。
                    const bool baselineUnseen = !obsImageSeenByModel;
                    const bool skipUnchangedCheck = forceRefresh || forceNextObserveUpload
                        || AiTakeExplicitScreenshotRequest() || baselineUnseen;
                    forceNextObserveUpload = false;
                    if (baselineUnseen) {
                        AppendAiDebugLog(L"  [诊断] 上一批动作后的那一帧还没给模型看过 ⇒ 本轮强制"
                            L"上传（否则模型看不到自己动作的结果，只会反复确认）");
                    }

                    if (!skipUnchangedCheck) {
                        const auto it = imageVars_.find(kAiObsImageVarName);
                        if (it != imageVars_.end() && !it->second.empty()) {
                            HBITMAP baseline = LoadBitmapFromFile(it->second);
                            if (baseline) {
                                BITMAP bb{}, cb{};
                                const bool sizeOk =
                                    GetObjectW(baseline, sizeof(bb), &bb)
                                    && GetObjectW(bmp, sizeof(cb), &cb)
                                    && bb.bmWidth > 0 && bb.bmHeight > 0
                                    && bb.bmWidth == cb.bmWidth && bb.bmHeight == cb.bmHeight;
                                if (sizeOk) {
                                    const ScreenChangeDiffResult diff = DiffBitmapsChangedRegions(
                                        baseline, bmp, 12, 48, 6,
                                        busy.valid() ? &busy : nullptr);
                                    r.changedRatio = diff.changedRatio;
                                    r.rawChangedRatio = diff.rawChangedRatio;
                                    r.busyCoverageRatio = diff.busyCoverageRatio;
                                    r.onlyDynamicChanged = diff.onlyDynamicChanged;
                                    r.matchScore = diff.nearlyIdentical
                                        ? 100.0
                                        : std::max(0.0, (1.0 - diff.changedRatio) * 100.0);

                                    // 动作局部：点击附近是否有结构变化（点赞态等）
                                    bool actionLocalStructural = false;
                                    if (lastActionScreenX >= 0 && lastActionScreenY >= 0
                                        && !diff.rois.empty()) {
                                        const int lx = lastActionScreenX - cx1;
                                        const int ly = lastActionScreenY - cy1;
                                        for (const auto& rr : diff.rois) {
                                            if (lx >= rr.x1 - 80 && lx < rr.x2 + 80
                                                && ly >= rr.y1 - 80 && ly < rr.y2 + 80) {
                                                actionLocalStructural = true;
                                                break;
                                            }
                                        }
                                    }

                                    auto fillRois = [&]() {
                                        r.changeRoisText.clear();
                                        // 优先动作附近的结构 ROI，避免视频大框抢注意力
                                        std::vector<ScreenChangeRoi> ordered = diff.rois;
                                        if (lastActionScreenX >= 0) {
                                            const int lx = lastActionScreenX - cx1;
                                            const int ly = lastActionScreenY - cy1;
                                            std::stable_sort(ordered.begin(), ordered.end(),
                                                [&](const ScreenChangeRoi& a, const ScreenChangeRoi& b) {
                                                    const int acx = (a.x1 + a.x2) / 2;
                                                    const int acy = (a.y1 + a.y2) / 2;
                                                    const int bcx = (b.x1 + b.x2) / 2;
                                                    const int bcy = (b.y1 + b.y2) / 2;
                                                    const long long da =
                                                        1LL * (acx - lx) * (acx - lx)
                                                        + 1LL * (acy - ly) * (acy - ly);
                                                    const long long db =
                                                        1LL * (bcx - lx) * (bcx - lx)
                                                        + 1LL * (bcy - ly) * (bcy - ly);
                                                    return da < db;
                                                });
                                        }
                                        for (size_t i = 0; i < ordered.size() && i < 4; ++i) {
                                            const auto& rr = ordered[i];
                                            if (!r.changeRoisText.empty()) r.changeRoisText += L";";
                                            r.changeRoisText += std::to_wstring(rr.x1) + L","
                                                + std::to_wstring(rr.y1) + L","
                                                + std::to_wstring(rr.x2) + L","
                                                + std::to_wstring(rr.y2);
                                        }
                                    };

                                    const bool structurallyQuiet = diff.sameSize
                                        && (diff.nearlyIdentical || diff.changedRatio < 0.003
                                            || diff.onlyDynamicChanged);
                                    // 与「OCR 索引那一帧」结构一致（只忽略动态区）→ 字坐标仍成立，续期
                                    if (diff.sameSize
                                        && (diff.nearlyIdentical || diff.changedRatio < 0.003)) {
                                        TouchOcrScreenIndex();
                                    }
                                    // ★★ **没有可复用的 OCR 行时不许走这次提前返回**（2026-10-02 真机事故）。
                                    //   这一支会 `return r;` —— 把后面整段「本地 OCR → 文字索引 →
                                    //   元素索引」全部跳过，只回一个 `unchanged=true` 的空结果。
                                    //   界面一直安静（模型还没成功动作过）时 ⇒ 每轮都走这里 ⇒
                                    //   索引**永远建不起来**，模型只能盯第一张旧图空转。
                                    //   判据：复用缓存里**有东西**才允许省这一步。
                                    const bool ocrCacheReusable = !LastOcrLinesCache().empty();
                                    if (structurallyQuiet && !actionLocalStructural
                                        && ocrCacheReusable) {
                                        ++consecutiveDynamicOnlyObserves;
                                        DeleteBitmapHandle(baseline);
                                        DeleteBitmapHandle(bmp);
                                        r.ok = true;
                                        r.unchanged = true;
                                        r.width = liveMap.apiWidth;
                                        r.height = liveMap.apiHeight;
                                        if (diff.onlyDynamicChanged || busy.busyCoverageRatio >= 0.05) {
                                            r.onlyDynamicChanged = true;
                                            r.settleHint =
                                                L"本地观察：仅动态区在变，控件区已稳定。"
                                                L"细则 section=agent。";
                                            if (consecutiveDynamicOnlyObserves >= 2) {
                                                r.settleHint +=
                                                    L" 已连续多次仅动态变化。";
                                            }
                                        }
                                        return r;
                                    }
                                    consecutiveDynamicOnlyObserves = 0;
                                    fillRois();
                                }
                                DeleteBitmapHandle(baseline);
                            }
                        }
                    }

                    consecutiveDynamicOnlyObserves = 0;
                    CommitSavedImage(bmp, kAiObsImageVarName, imageVarRunId, imageVars_);
                    // 这一帧就是**本轮要交给模型**的观察帧 ⇒ 记账：基线已交付（docs §64）
                    obsImageSeenByModel = true;
                    // ★屏幕文字坐标索引（通用）：本地 OCR 出「哪些文字在哪」，
                    // 让模型不靠看图也能准确点。OCR 未安装则整段静默跳过。
                    // 这是「AI 看不懂界面/靠猜坐标」的正解，也与文档 §21 的 PP-OCR 评估对应。
                    int idxCount = 0;
                    std::wstring ocrIndex;
                    /// ★本帧的「屏幕文字变化」事实（docs §65）：由下面的 OCR 段算出，
                    /// 再挂到给模型看的索引最前面（那条消息一定会被注入）。
                    std::wstring textDeltaThisFrame;
                    std::vector<AiIndexRowInput> ocrIndexRows;
                    // ★★给模型看的两个索引（文字索引 / 元素索引）必须**同一套坐标口径**，
                    //   而那一套就是 `mouseClick` 最后会用的那一套 ⇒ **帧尺寸只算一次**，
                    //   两个消费者都从这里取（一处事实一份逻辑）。
                    //   ⚠ 旧实现只在元素索引那侧算了它，文字索引那侧直接发**屏幕绝对像素**
                    //   却写着「与 mouseClick 同一套」⇒ 同一条消息里两套坐标，模型必然点偏
                    //   （§33.1 修了一处，漏了另一处；docs §60.5）。
                    //   ⚠ 这里的算法必须与 `EncodeBitmapForAiAnalysis` 同尺（帧尺寸在编码
                    //   **之前**算出来，否则索引声明的尺寸和模型真正收到的图不是一张）。
                    // ★★ **统一 1024 长边**（2026-10-02 真机：浏览器前台原来用 768，导致
                    //   标题声明 768×432、而索引条目（UIA/OCR 按初次截图的 1024×576 空间算的，
                    //   坐标最大到 528）对不上 ⇒ 模型拿 768×432 读清单、拿条目数字写坐标 ⇒
                    //   系统性点偏、选错选项。而鼠标工具是按**本轮附图**解释坐标的 ⇒
                    //   只要附图、条目、声明三者都是 1024×576，就全部一致。
                    //   代价：每张附图约 31KB→49KB，换来的是不再有三种空间互相打架。）
                    const int aiIdxLongEdge = 1024;
                    const double aiIdxScale = std::clamp(
                        eff.aiImageScale > 0.0 ? eff.aiImageScale : 0.5, 0.1, 1.0);
                    const double aiIdxEff = ComputeEffectiveAiImageScale(
                        cx2 - cx1, cy2 - cy1, aiIdxScale, aiIdxLongEdge);
                    int aiIdxFrameW = (std::max)(1,
                        static_cast<int>((cx2 - cx1) * aiIdxEff));
                    int aiIdxFrameH = (std::max)(1,
                        static_cast<int>((cy2 - cy1) * aiIdxEff));
                    // ★★ **不要覆盖这里算出来的帧尺寸**（2026-10-02 真机实测，我上一版的错误修正）。
                    //
                    //   这段曾经把清单尺寸强制成 `liveMap.apiWidth/Height`（= **初次 AI 动作截图**
                    //   的 1024×576），理由是"以本轮附图为准"。但**本函数（观察）附给模型的图
                    //   是它自己按同一公式编码出来的那一张**（浏览器前台 = 768×432），
                    //   与 `liveMap`（另一条链路的截图映射）无关。
                    //   ⇒ 覆盖的后果：清单声明 1024×576、实际附图 768×432 ⇒
                    //     模型在 768×432 的图上量位置、我们按 1024×576 的口径解释
                    //     ⇒ **系统性点偏**（用户实测："总是选择了错误的选项"）。
                    //   ⇒ 结论：这里**公式算出来的就是对的**（它按 `aiIdxLongEdge` = 观察帧的同一条
                    //     长边、同一个缩放公式算），保持不动；只在**不一致时留痕**便于排查。
                    if (liveMapValid && liveMap.apiWidth > 0
                        && (aiIdxFrameW != liveMap.apiWidth || aiIdxFrameH != liveMap.apiHeight)) {
                        AppendAiDebugLog(L"  [诊断] 清单帧尺寸 " + std::to_wstring(aiIdxFrameW)
                            + L"×" + std::to_wstring(aiIdxFrameH)
                            + L"（本轮附图口径，按观察帧长边算）"
                            + L"；AI动作截图映射为 " + std::to_wstring(liveMap.apiWidth) + L"×"
                            + std::to_wstring(liveMap.apiHeight) + L"（另一条链路，仅记录不采用）");
                    }
                    /// 「标签 → 图标槽」配对结果（推断，通用版面感知；见下面那段注释）
                    std::vector<AiLabelIconPair> iconSlots;
                    if (AiFastPathsEnabled() && CheckOcrEnvironment(false).state == OcrEnvState::Ready) {
                        // ★★ **界面未变的帧不再重复 OCR**（2026-09-30，用户实测后加：③第一步）。
                        //
                        //   实测每帧 OCR 固定 6.9s（常驻会话 + 1280 降采样之后仍是大头），
                        //   而很多帧**结构差分 0**（日志里的「本地观察：界面未变…跳过上传」）
                        //   —— 那些帧的画面和上一帧一模一样，文字索引**必然也一模一样**，
                        //   重跑一次纯属白烧 7 秒。
                        //   ⚠ 三条兜底，避免"复用"变成"看过期数据"：
                        //     ① 只在 `r.unchanged`（结构差分判未变）时复用；
                        //     ② 捕获区域（cx1,cy1,cx2,cy2）必须与上一帧相同；
                        //     ③ 连续复用**不超过 4 帧**，之后强制跑一次全帧（防"缓慢变化"漏检）。
                        //   ⚠ 尚未做的是"只扫变化带"（ROI 裁剪）：那要先解决 `diff.rois` 的
                        //     作用域（它在 2630 的 `if (sizeOk)` 块里，OCR 这边取不到），
                        //     留作下一步。
                        auto& s_lastOcrLines = LastOcrLinesCache();   // ★ 改用文件作用域缓存（见 LastOcrLinesCache 的注释）
                        int& s_lastCx1 = g_lastOcrCx1; int& s_lastCy1 = g_lastOcrCy1;
                        int& s_lastCx2 = g_lastOcrCx2; int& s_lastCy2 = g_lastOcrCy2;
                        int& s_ocrReuseStreak = g_ocrReuseStreak;
                        const bool geomSame = s_lastCx1 == cx1 && s_lastCy1 == cy1
                            && s_lastCx2 == cx2 && s_lastCy2 == cy2;
                        const bool canReuse = r.unchanged && geomSame && !s_lastOcrLines.empty()
                            && s_ocrReuseStreak < 4;
                        // ★面包屑：OCR 是观察链路里**唯一会起外部进程**的一步（Python/WinRT），
                        //   也是历史上最可能长时间不返回的一步 —— 进入前必须留痕（docs §66）。
                        AppendAiDebugLog(canReuse
                            ? (L"  [诊断] 观察阶段 ⑤ 本地 OCR：**跳过**（界面未变 + 区域相同 + 复用未超 4 帧）"
                               L"⇒ 复用上一帧文字索引")
                            : L"  [诊断] 观察阶段 ⑤ 本地 OCR…");
                        const ULONGLONG ocrT0 = GetTickCount64();
                        OcrEngineOutput ocr;
                        if (canReuse) {
                            ocr.success = true;
                            ocr.lines = s_lastOcrLines;
                        } else {
                            ocr = RunOcrOnBitmap(bmp, cx1, cy1, false);
                            s_lastOcrLines = ocr.lines;
                            s_ocrReuseStreak = 0;
                        }
                        if (canReuse) {
                            ++s_ocrReuseStreak;
                        } else {
                            s_lastCx1 = cx1; s_lastCy1 = cy1;
                            s_lastCx2 = cx2; s_lastCy2 = cy2;
                        }
                        AppendAiDebugLog(L"  [诊断] 观察阶段 ⑥ OCR 返回 "
                            + std::to_wstring(canReuse ? 0 : static_cast<int>(GetTickCount64() - ocrT0)) + L"ms（"
                            + std::to_wstring(ocr.lines.size()) + L" 行"
                            // ★后端名必须跟着耗时一起报（docs §70）：OCR 是按偏好顺序
                            //   依次尝试的，Python 不可用会**静默**回退系统 OCR，
                            //   两个后端的耗时能差一个数量级 —— 不报后端，「慢」就无从归因。
                            + (canReuse ? L"，后端=(复用上一帧，未跑 OCR)"
                                        : (ocr.backend.empty() ? L"" : (L"，后端=" + ocr.backend)))
                            // ★★ 会话状态一并报（2026-09-29）：OCR 的常驻服务与 one-shot
                            //   **差一个数量级**（13~17s vs 1~2s）。不报这一项，"慢"到底是
                            //   "会话没起来"还是"会话内单次就慢"就无从归因（用户实测 6~9s，
                            //   正需要这一行来判）。
                            + (canReuse ? L"" : (IsOcrSessionActive() ? L"，会话=常驻" : L"，会话=无(one-shot)"))
                            + L"）");
                        // ★★帧间**屏幕文字**差分（docs §65）：上一帧的 OCR 文本 vs 这一帧。
                        //   这是「我的动作到底有没有发生、发生了什么」的**硬证据**，零额外识图、
                        //   零额外 API —— OCR 本来每帧都在跑（对标开源 GUI agent 的 state-diff：
                        //   那边比 a11y 树；游戏/画布类界面没有树，OCR 是同一角色的一等替代）。
                        //   实测代价（落盘日志）：模型放了一个僵尸，本地只说得出「落点附近有变化」
                        //   ⇒ 它连着 5 轮截图 + zoom 去找那个僵尸，最后跑去搜网页，再没放第二个；
                        //   而那一帧的 OCR 明明读到了 `29900`、上一帧是 `30000`。
                        textDeltaThisFrame = prevOcrTextLines.empty()
                            ? std::wstring()
                            : AiDescribeTextIndexDelta(prevOcrTextLines, ocr.lines, 6);
                        prevOcrTextLines = ocr.lines;
                        // 原始行表留一份给「文字直点」（textIndex 只是给模型看的裁剪版）
                        StoreOcrScreenIndex(ocr, cx1, cy1, cx2, cy2);
                        // ★文字索引改成「按画面行分组的可点清单」（docs §25.4②）。
                        //   原先是一串平铺的 `文字(x,y)；…`，模型**无法把它和画面上的
                        //   卡片对上**（不知道哪个价格属于哪张卡、卡片从左到右第几张），
                        //   于是反复猜 —— 实测一局里连点错卡片、还自问「哪个是 600 的卡」。
                        //   按行分组 + 标视觉行序，把「找第几张卡」从推理变成查表。
                        //   纯函数在 ai_locate_verify.cpp（可逐格自检）。
                        ocrIndex = FormatOcrTextIndex(ocr.lines, cx1, cy1, cx2, cy2,
                            aiIdxFrameW, aiIdxFrameH, &idxCount);
                        // 统一索引要**每段单独**入表：分组文本会让一段的坐标代表整组，
                        // 按它点会打到这一行的中间，而不是那一小段文字。
                        const auto ocrRowSpans = CollectOcrIndexRows(ocr.lines);
                        for (const auto& row : ocrRowSpans) {
                            for (const auto& span : row) {
                                if (span.text.empty()) continue;
                                ocrIndexRows.push_back(AiIndexRowInput{
                                    span.text, span.x1, span.y1, span.x2, span.y2 });
                            }
                        }
                        // ★★「短标签 → 它标注的图标槽」（通用版面推断，见 ai_locate_verify.h）。
                        //   实测那一局：卡槽里 OCR 只读得到价格数字，**读不到卡片是什么** ⇒
                        //   模型知道「600」这几个字在哪、不知道那张卡在哪 ⇒ 8 轮 zoom 猜卡，
                        //   最后只敢点唯一有把握的那张（最便宜的一张）—— 用户看到的
                        //   「只会放普通僵尸」就是这么来的。
                        //   做法：对「≥3 段短标签成排」的每一行，取它**正上方**一条横带，
                        //   按「列与同一行中位亮度的偏离」切出等距块，全部闸门过了才发布
                        //   （配不上就什么都不发布 = 回到今天的行为）。
                        //   出处：OmniParser/UFO² 的 icon+text 配对、PaddleOCR/tesseract 的
                        //   投影剖面版面分析；纯判据在 ai_locate_verify.cpp（可逐格自检）。
                        //   ⚠⚠ **这里必须用全量的行**（`maxNumericSpans` 给大值）：给模型看的
                        //   索引对纯数字限量（默认 16 条），而一个 8×10 的选卡网格有 ~50 个价签
                        //   ⇒ 限量后每行只剩 <3 段 ⇒ 配对**一条都不发布**，而且连原因都没打
                        //   （实测日志 `标签→图标槽推断 0 条` 就是这么来的）。限量是**展示层**
                        //   的事，不是感知层的事。
                        // ★★`maxGapFactor` 同理，而且更隐蔽（docs §56）：**一排卡片宽、价签小**
                        //   的卡槽里，价签间距 ≈43 upload px 而字高只有 ≈10 upload px
                        //   ⇒ 间距/字高 ≈ 4.3 > 默认的 2 ⇒ **每个价签各自成行** ⇒
                        //   `spans.size() >= 3` 永不成立 ⇒ 连"未配对"的诊断都不会打。
                        //   行内**等距/等高**由 PairCaptionRowWithIconBand 自己判，不需要间距闸。
                        const auto iconRowSpans = CollectOcrIndexRows(ocr.lines, 64, 4096, 64);
                        int prevRowBottom = INT_MIN;
                        // ★ **同一原因只报一次、带次数**（2026-09-29，用户反馈"刷了十几行"）。
                        //   原来每一行都被拒就各打一行 ⇒ 一帧十几行噪音，把其它诊断淹了。
                        //   ⚠ 但**不许**把原因吞掉：docs §「发布 0 条时原因必须能看见」——
                        //   这里按**原因聚合成一行**（原因 × 次数），排查时照样一眼看到闸在哪。
                        std::vector<std::pair<std::wstring, int>> iconBandSkips;
                        auto noteIconBandSkip = [&iconBandSkips](const std::wstring& why) {
                            if (why.empty()) return;
                            for (auto& kv : iconBandSkips) {
                                if (kv.first == why) { ++kv.second; return; }
                            }
                            iconBandSkips.push_back({ why, 1 });
                        };
                        for (const auto& row : iconRowSpans) {
                            std::vector<AiIndexRowInput> spans;
                            spans.reserve(row.size());
                            int top = INT_MAX, bot = 0, lx1 = INT_MAX, lx2 = 0;
                            for (const auto& sp : row) {
                                if (sp.text.empty()) continue;
                                spans.push_back(AiIndexRowInput{
                                    sp.text, sp.x1, sp.y1, sp.x2, sp.y2 });
                                top = (std::min)(top, sp.y1);
                                bot = (std::max)(bot, sp.y2);
                                lx1 = (std::min)(lx1, sp.x1);
                                lx2 = (std::max)(lx2, sp.x2);
                            }
                            const int rowBottomSnapshot = bot;
                            if (spans.size() >= 3) {
                                const int labelH = (std::max)(1, bot - top);
                                const int bandH = std::clamp(labelH * 5, 16, 220);
                                const int bandY2 = top - 2;
                                // ⚠ 条带上沿还要**让开上一行标签**：网格里一行行紧挨着，
                                //   条带若吃进上一行的价签，配出来的「块」就是上一行的卡片。
                                const int bandY1 = (std::max)(bandY2 - bandH,
                                    prevRowBottom == INT_MIN ? bandY2 - bandH
                                                             : prevRowBottom + 2);
                                // 条带必须完整落在**本帧捕获区域**内，否则采样会跨到别的界面。
                                // ⚠ 拒绝与**原因文案**都走同一个纯函数（`AiExplainIconBandSkip`）：
                                //   旧文案把「上一行标签压过来」也写成「条带出界（…捕获区 …）」
                                //   ⇒ 排查时被指向「捕获区域配错了」这个错误方向。回执不许说谎。
                                const std::wstring bandSkip =
                                    AiExplainIconBandSkip(bandY1, bandY2, prevRowBottom, cy1, bandH);
                                if (!bandSkip.empty()) {
                                    noteIconBandSkip(bandSkip);
                                } else {
                                    // 条带左右各留够一格：网格里最边上的卡片比标签宽，
                                    // 只按标签包围盒裁会把它们切掉（判据那边会如实拒绝）。
                                    const int margin = (std::max)(24, labelH * 3);
                                    const int bandX1 = (std::max)(cx1, lx1 - margin);
                                    const int bandX2 = (std::min)(cx2, lx2 + margin);
                                    AiIconBand band;
                                    if (!SampleIconBandFromBitmap(bmp, cx1, cy1,
                                            bandX1, bandY1, bandX2, bandY2, &band)) {
                                        noteIconBandSkip(L"条带采样失败（"
                                            + std::to_wstring(bandX2 - bandX1) + L"×"
                                            + std::to_wstring(bandY2 - bandY1) + L"）");
                                    } else {
                                        std::wstring why;
                                        const std::vector<AiLabelIconPair> pairs =
                                            PairCaptionRowWithIconBand(spans, band, &why);
                                            for (const auto& p : pairs) iconSlots.push_back(p);
                                            if (!pairs.empty()) {
                                                AppendAiDebugLog(L"  [诊断] 图标槽配对：这一行 "
                                                    + std::to_wstring(spans.size()) + L" 段标签 ⇒ 发布 "
                                                    + std::to_wstring(pairs.size())
                                                    + L" 条「标签→图标」（正上方检出等距块）");
                                            } else if (!why.empty()) {
                                                noteIconBandSkip(why);
                                            }
                                        }
                                    }
                                }
                            // 下一行的条带上沿要让开**这一行标签**的底边
                            if (rowBottomSnapshot > 0) prevRowBottom = rowBottomSnapshot;
                        }
                        // ★ 聚合输出（一行，原因 × 次数）：既不再刷屏，也不吞原因
                        if (!iconBandSkips.empty()) {
                            int total = 0;
                            std::wstring detail;
                            for (const auto& kv : iconBandSkips) {
                                total += kv.second;
                                if (!detail.empty()) detail += L"；";
                                detail += kv.first + L" ×" + std::to_wstring(kv.second);
                            }
                            AppendAiDebugLog(L"  [诊断] 图标槽未配对 ×" + std::to_wstring(total)
                                + L"（按原因聚合）：" + detail);
                        }
                    }
                    // ★★统一「可点元素索引」：把 UIA 控件与 OCR 文字**合并成一张带编号的表**。
                    // 这是用户问的「找图-定位-点击能不能合并」的落点：过去 UIA 控件表只服务
                    // invokeUiControl、OCR 索引只给模型看、locateAndClick 又走第三条独立阶梯，
                    // 三套各自枚举各自打分 —— 于是模型只能「看图 → 描述 → 再识图定位」。
                    // 现在一次枚举出一张表：模型按编号/名字说话，locateAndClick 直接查表拿坐标
                    // （0 次 VLM），索引里没有才回落识图。
                    // 对齐开源实现（UFO/UFO² 的 UIA+OmniParser merge、WAA/Navi 的元素 id 动作空间）。
                    {
                        std::vector<AiUiAnchor> uiAnchors;
                        // 窗口自身的标题栏按钮**不进索引**：它的名字（「关闭」）和应用内按钮
                        // 完全一样，实测模型想关游戏内面板却把整个游戏窗口关掉了。
                        // ⚠ 预算 60→100：这一层现在同时收滑块/数据项/列头/滚动条/菜单栏/标签栏，
                        //   以及可聚焦的文档区（见 UiActionVerbForControl 旁的类型表）。
                        //   仍**不设静默上限**：截断多少由调用方原样报回去（见 offscreenSkipped 同款做法）。
                        int uiSkipped = 0;
                        for (const auto& ctl : windowmode::ListInteractiveUiControls(
                                 GetForegroundWindow(), 100, &uiSkipped)) {
                            if (ctl.titleBarControl || !ctl.enabled) continue;
                            AiUiAnchor a;
                            a.x1 = ctl.rect.left;
                            a.y1 = ctl.rect.top;
                            a.x2 = ctl.rect.right;
                            a.y2 = ctl.rect.bottom;
                            a.name = ctl.name;
                            // ★★角色 / 动作能力 / 可读状态：这是「半视觉」这一轮的关键补强 ——
                            //   让模型**不必截图**就能知道「这是什么控件、支持哪类操作、现在什么状态」。
                            //   全部是 UIA 如实读出来的事实（value/range/toggle/focused/readonly…），
                            //   不含任何祈使句或建议（本项目总原则见 docs §48）。
                            a.role = ctl.controlType;
                            a.action = ctl.action;
                            a.state = ctl.state;
                            uiAnchors.push_back(std::move(a));
                        }
                        if (uiSkipped > 0) {
                            // 如实记账（不设静默上限）：模型据此知道「还有条目在视口外，要先滚动」
                            // —— 这正是 UIA 清单能替代「截图看一眼」的前提之一。
                            AppendAiDebugLog(L"  [诊断] UIA 清单：视口外另有 "
                                + std::to_wstring(uiSkipped) + L" 个可交互控件未收（需先滚动）");
                        }
                        std::vector<AiElementEntry> elements =
                            BuildAiElementIndexWithIcons(uiAnchors, ocrIndexRows, iconSlots, 80);
                        if (!elements.empty()) {
                            // ★坐标按 **upload 截图像素**给出（与 mouseClick 同一套）。
                            // 不能给屏幕像素：模型会拿它去 mouseClick，在 2560×1440 截成
                            // 1024×576 的帧里**整体偏 2.5 倍**；更糟的是它会察觉矛盾，
                            // 然后花 10~40KB 思考反复推敲「这是屏幕坐标还是图像坐标」。
                            // ★帧尺寸由**上面那一处**统一算出（`aiIdxFrame*`）——与文字索引、
                            //   与模型真正收到的那张图必须是同一个数，别在这里再算一遍。
                            const int idxFrameW = aiIdxFrameW;
                            const int idxFrameH = aiIdxFrameH;
                            r.elementIndex = FormatAiElementIndex(elements, cx1, cy1, cx2, cy2,
                                idxFrameW, idxFrameH);
                            r.elementIndexCount = static_cast<int>(elements.size());
                            for (const auto& e : elements) {
                                if (e.source == AiElementSource::UiAutomation)
                                    ++r.elementIndexUiCount;
                                else if (e.source == AiElementSource::LabeledIcon)
                                    ++r.elementIndexIconCount;
                            }
                            // 供「按描述定位」直查（0 次识图）：与给模型看的清单是**同一张表**
                            aiElementIndexThisFrame = std::move(elements);
                        }
                    }
                    if (idxCount > 0) {
                        r.textIndex = std::move(ocrIndex);
                        r.textIndexCount = idxCount;
                    }
                    // ★★把「这一帧屏幕文字变了什么」挂到**一定会被注入**的那条索引最前面
                    //   （docs §65）。元素索引优先注入、文字索引只在元素索引为空时注入，
                    //   所以两边都挂一次 —— 恰好有一条会走到模型面前，绝不重复注入。
                    if (!textDeltaThisFrame.empty()) {
                        if (!r.elementIndex.empty())
                            r.elementIndex = textDeltaThisFrame + L"\n" + r.elementIndex;
                        else if (!r.textIndex.empty())
                            r.textIndex = textDeltaThisFrame + L"\n" + r.textIndex;
                        AppendAiDebugLog(L"  [诊断] " + textDeltaThisFrame);
                    }
                    const double scale = std::clamp(
                        eff.aiImageScale > 0.0 ? eff.aiImageScale : 0.5, 0.1, 1.0);
                    std::wstring imeStatus;
                    if (!wmUsesTarget()) {
                        imeStatus = QueryForegroundImeStatusText();
                        if (imeStatus.find(L"[输入法] 英文") != std::wstring::npos)
                            imeStatus.clear();
                    }
                    // 观察帧长边上限：默认 768（省 token）。
                    // ★★ **统一 1024**（2026-10-02）：这里原来浏览器前台用 768 ⇒ 实际附图
                    //   是 768×432，而清单条目/声明（上方 aiIdxLongEdge=1024 那条）是
                    //   1024×576 ⇒ 三种空间打架，模型点哪都不对（用户："点击定位不准"）。
                    //   768 的本意是省 token；但**宁可图大一点，不许三个口径不一致**。
                    const int observeLongEdge = 1024;
                    // 观察帧落点标注：把「上一次真的点在哪」画进去（没点过则空标注）
                    const AiFrameClickMark frameClickMark =
                        makeFrameClickMark(cx1, cy1, cx2, cy2);
                    const AiImageEncodeResult enc = EncodeBitmapForAiAnalysis(
                        bmp, scale, observeLongEdge, imeStatus.empty() ? nullptr : &imeStatus,
                        &frameClickMark);
                    DeleteBitmapHandle(bmp);
                    if (enc.base64.empty()) return r;
                    r.ok = true;
                    r.unchanged = false;
                    r.base64 = enc.base64;
                    r.width = enc.outWidth;
                    r.height = enc.outHeight;
                    liveMap.capX1 = cx1;
                    liveMap.capY1 = cy1;
                    liveMap.capX2 = cx2;
                    liveMap.capY2 = cy2;
                    liveMap.srcWidth = enc.srcWidth;
                    liveMap.srcHeight = enc.srcHeight;
                    liveMap.apiWidth = enc.outWidth;
                    liveMap.apiHeight = enc.outHeight;
                    liveMapValid = liveMap.apiWidth > 0 && liveMap.capX2 > liveMap.capX1;
                    return r;
                };

                AiActionHostHooks agentHooks;
                agentHooks.onExecuteActions = [&](const std::wstring& actionsJson) {
                    return executeActionsJsonNow(actionsJson);
                };
                agentHooks.onObserveScreen = [&](bool forceRefresh) {
                    return observeScreenForAgent(forceRefresh);
                };
                // ★`zoom`：把一块区域按**原始分辨率**放大回传（通用感知能力，零游戏知识）。
                //   动机（实测那一局）：整帧降到 1024 宽后，15 张卡每张只剩 ≈40 upload 像素，
                //   模型整轮整轮在推「这张卡是什么/多少钱」，8 轮没落一个动作。
                //   依据：Efficient GUI Agents 系统综述（arXiv 2609.02309）把「区域化视觉感知」
                //   列为 Observation Efficiency 的专门一类，并把 image crops 当一等观测原语
                //   （docs §50）。
                agentHooks.onZoomRegion = [&](int zx1, int zy1, int zx2, int zy2,
                                              const std::wstring& target, int maxEdge) {
                    AiZoomResult zr;
                    // ⚠ `maxEdge` 由工具侧按 `kAgentAttachmentMaxLongEdge`（送图链路的硬上限）
                    //   收口；本钩子不再重复那道 clamp（一处事实一份逻辑），但回执侧
                    //   （`FormatAiZoomReceipt`）会按同一条规则再算一遍**交付尺寸** ——
                    //   万一这里收窄算错，回执写的仍是模型真正看到的那张图的倍率。
                    if (!liveMapValid || liveMap.apiWidth <= 0 || liveMap.apiHeight <= 0) {
                        zr.error = L"还没有可用的观察帧（先 computer(action=screenshot) 看一轮再放大）";
                        return zr;
                    }
                    // upload(帧像素) ↔ screen：screen = cap + upload / ratio
                    const double rx = static_cast<double>(liveMap.apiWidth)
                        / (std::max)(1, liveMap.capX2 - liveMap.capX1);
                    const double ry = static_cast<double>(liveMap.apiHeight)
                        / (std::max)(1, liveMap.capY2 - liveMap.capY1);
                    auto upX = [&](int sx) {
                        return static_cast<int>((sx - liveMap.capX1) * rx + 0.5);
                    };
                    auto upY = [&](int sy) {
                        return static_cast<int>((sy - liveMap.capY1) * ry + 0.5);
                    };

                    int ux1 = zx1, uy1 = zy1, ux2 = zx2, uy2 = zy2;
                    if (!target.empty()) {
                        // ①-a 文字标签入口：查**本帧元素索引** —— 与 locateAndClick 查表
                        //     是同一张表 ⇒「放大看的区域」和「会点的地方」保证一致。
                        AiIndexResolveResult ir;
                        if (!AiElementIndexResolve(aiElementIndexThisFrame, target, false, 0, 0, &ir)
                            || !ir.ok) {
                            zr.error = L"文字标签「" + target + L"」在本帧元素索引里没有可信命中"
                                + (ir.why.empty() ? L"" : L"（" + ir.why + L"）")
                                + L"；可改用坐标——文字索引/元素索引里都给了 (x,y)。";
                            return zr;
                        }
                        int hitIdx = -1;
                        for (size_t i = 0; i < aiElementIndexThisFrame.size(); ++i) {
                            if (aiElementIndexThisFrame[i].id == ir.id) {
                                hitIdx = static_cast<int>(i);
                                break;
                            }
                        }
                        if (hitIdx < 0) {
                            zr.error = L"内部不一致：解析到的编号 " + std::to_wstring(ir.id)
                                + L" 不在本帧索引里（实现缺陷）";
                            return zr;
                        }
                        // 条目是**屏幕像素** ⇒ 换成 upload 像素（回执一律按 upload 报）；
                        // 四周放 8px 余量：紧框会把目标本身切掉边，反而更难认。
                        const AiElementEntry& e = aiElementIndexThisFrame[hitIdx];
                        ux1 = upX(e.x1) - 8;
                        uy1 = upY(e.y1) - 8;
                        ux2 = upX(e.x2) + 8;
                        uy2 = upY(e.y2) + 8;
                        zr.byTarget = true;
                        zr.resolvedName = e.name;
                    }
                    AiZoomRect rect;
                    std::wstring why;
                    if (!AiZoomClampRect(ux1, uy1, ux2, uy2, liveMap.apiWidth, liveMap.apiHeight,
                            8, rect, why)) {
                        zr.error = why;
                        return zr;
                    }
                    zr.requested = rect;
                    // ② ★★**绝不静默裁掉模型要的部分**（实测事故）：旧实现遇到超过上限的区域
                    //    会**居中收窄**（把两边切掉）——模型 `zoom(0,0,1024,80)`（整条卡槽）
                    //    拿到的是中间 512 宽那一段，它对照回执发现区域不是自己要的，
                    //    却已经据此下了结论「卡槽是空的」，白烧两轮。
                    //    现在：装不下就**分块**（每块原生分辨率，整块都给全）；块数超过
                    //    `kMaxTiles` 才退回「整块降采样」，并把代价（放大倍数丢失 + 每块
                    //    该多大）如实写进回执 —— 让模型自己决定要不要改小区域。
                    const double nativePerUpX = 1.0 / (rx > 0 ? rx : 1.0);
                    const double nativePerUpY = 1.0 / (ry > 0 ? ry : 1.0);
                    constexpr int kMaxZoomTiles = 2;
                    int needTiles = 0;
                    std::vector<AiZoomRect> plans = PlanAiZoomTiles(rect, nativePerUpX,
                        nativePerUpY, maxEdge, kMaxZoomTiles, &needTiles);
                    if (plans.empty()) {
                        plans.push_back(rect);   // 降采样交付：编码那一步会按上限缩
                        const int tileUpW = (std::max)(8, static_cast<int>(maxEdge / nativePerUpX));
                        const int tileUpH = (std::max)(8, static_cast<int>(maxEdge / nativePerUpY));
                        zr.note = L"这块区域在原分辨率下要 " + std::to_wstring(needTiles)
                            + L" 张才装得下（一次最多 " + std::to_wstring(kMaxZoomTiles)
                            + L" 张）⇒ 本次**整块按上限缩过**才给你：放大倍数已经丢失，"
                              L"只够看大体位置/形状，小字仍然读不出来。要看清细部就把区域改小"
                              L"（每张约 " + std::to_wstring(tileUpW) + L"×"
                            + std::to_wstring(tileUpH) + L" upload 像素以内），分几次问不同的块。";
                    } else if (plans.size() > 1) {
                        zr.note = L"这块区域一张装不下，已**分成 " + std::to_wstring(plans.size())
                            + L" 张**给你（顺序见回执，各自带区域与倍率；每张都是原生分辨率）。"
                              L"你要的整块都在里面 —— 没有任何一块被裁掉。";
                    }
                    // ③ 逐块原生分辨率裁剪（WGC 优先、GDI 回退）→ **一次编成交付形态的 JPEG**
                    // ★文件名必须**每次不同**（实测事故，别再改回固定名）：
                    //   同一轮里模型可以调两次 zoom（它就是这么用的：先放大卡槽、再放大底栏）。
                    //   而 `[[AGENT_IMG:...]]` 的路径是**整批工具跑完之后**才被读盘编码的
                    //   （agent_core.cpp：本批的 pendingImages → 轮末统一
                    //   `AgentBuildImageParts`）⇒ 用固定名 `zoom_last.png` 时，第二次放大
                    //   会在第一次读盘之前把它覆盖掉 ⇒ 模型收到**两张一模一样的图**。
                    //   实测后果很重：模型从工具结果里的路径名看出「两张都是 zoom_last.png」，
                    //   判定「zoom 只会返回最后一张、不可靠」，**直接弃用了这个工具**，
                    //   回去靠 1024 宽的整帧猜卡价（那正是 zoom 存在的理由）。
                    //   ⚠ 旧注释写的是「提取时当场编 base64，之后不再读盘」—— 那句话是**错的**，
                    //   同一批里的多张图会互相覆盖；别照它推理。
                    // 目录大小：名字唯一 ⇒ 会累积 ⇒ 每次进来先按年龄清旧图（见 PruneZoomTempDir）。
                    wchar_t tmpBuf[MAX_PATH]{};
                    std::wstring zoomDir = (GetTempPathW(MAX_PATH, tmpBuf) > 0)
                        ? (std::wstring(tmpBuf) + L"QstZoom") : (AppDir() + L"\\zoom_tmp");
                    CreateDirectoryW(zoomDir.c_str(), nullptr);
                    PruneAiZoomTempDir(zoomDir);
                    static std::atomic<unsigned> s_zoomSeq{0};
                    // ★★给模型看的**任何**截图都不许含本软件自身窗口（docs §70）。
                    //   观察帧那条路一直在藏（`captureObservationNow` 起手就 ScopedHideOwnUiForCapture），
                    //   裁剪这条路**漏了** ⇒ 实测真机日志：模型在 zoom 放大图里看见了自己的
                    //   「调试信息输出窗口」（最顶层、且正在滚动追加**它自己的思考文本**），
                    //   于是花掉整整一轮（7 KB 思考 / 14.6 s）去分析「那是什么窗口、要不要点它的
                    //   最小化按钮」，整轮没推进任务。放大图是给模型**看目标**用的，
                    //   里面出现我们自己的浮窗就是纯粹的污染。
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwnForZoom(
                        UserFacingMainHwnd());
                    for (size_t ti = 0; ti < plans.size(); ++ti) {
                        const AiZoomRect& t = plans[ti];
                        int sx1 = 0, sy1 = 0, sx2 = 0, sy2 = 0;
                        MapApiPointToScreen(liveMap, t.x1, t.y1, sx1, sy1);
                        MapApiPointToScreen(liveMap, t.x2, t.y2, sx2, sy2);
                        HBITMAP crop = CaptureAiRegionComposed(sx1, sy1, sx2, sy2);
                        if (!crop) {
                            zr.error = L"截取该区域失败（窗口可能已最小化或被完全遮挡）";
                            return zr;
                        }
                        AiZoomTile tile;
                        tile.area = t;
                        tile.imagePath = FormatAiZoomTempPath(zoomDir,
                            GetCurrentProcessId(), s_zoomSeq.fetch_add(1) + 1);
                        // ★★落盘就用**送图链路那一套编码**（JPEG q82 + 长边 ≤ maxEdge）：
                        //   旧实现写的是原始分辨率 PNG（实测单张 1365 KB），而模型真正收到的是
                        //   附件链路重编码后的 JPEG（~200 KB）⇒ 回执按前者报字节数，等于对模型
                        //   说谎，而且白写一个几 MB 的中间产物。现在磁盘上的那张**就是**交付的那张，
                        //   `outWidth/outHeight/bytes` 是它的真值，回执直接引用 ⇒ 不可能说谎。
                        const bool saved = SaveHbitmapJpeg(crop, tile.imagePath,
                            kAgentAttachmentJpegQuality, maxEdge,
                            &tile.outWidth, &tile.outHeight, &tile.bytes);
                        DeleteBitmapHandle(crop);
                        if (!saved) {
                            zr.error = L"放大图编码失败（本机 OpenCV 不可用时会发生这种事）；路径："
                                + tile.imagePath;
                            return zr;
                        }
                        AppendAiDebugLog(L"  [诊断] zoom 交付 第" + std::to_wstring(ti + 1) + L"/"
                            + std::to_wstring(plans.size()) + L" 张 upload("
                            + std::to_wstring(t.x1) + L"," + std::to_wstring(t.y1) + L")-("
                            + std::to_wstring(t.x2) + L"," + std::to_wstring(t.y2) + L") ⇒ "
                            + std::to_wstring(tile.outWidth) + L"×" + std::to_wstring(tile.outHeight)
                            + L" JPEG " + std::to_wstring((tile.bytes + 1023) / 1024) + L" KB"
                            + (zr.byTarget ? L"（文字标签入口）" : L"（坐标入口）"));
                        zr.tiles.push_back(std::move(tile));
                    }
                    zr.ok = true;
                    return zr;
                };
                agentHooks.onCaptureScreen = [&](std::string& b64, int& w, int& h) {
                    return captureObservationNow(b64, w, h);
                };
                agentHooks.onProbeDisabledSubmit = [](std::wstring& disabledName) {
                    return windowmode::FindDisabledSubmitButtonInForeground(disabledName);
                };
                agentHooks.onProbeForegroundDialog = []() {
                    return windowmode::FormatForegroundDialogProbe();
                };
                agentHooks.onQueryImeStatus = []() {
                    return QueryForegroundImeStatusText();
                };
                agentHooks.onConfirmWebLaunch = [this](const std::wstring& detail) -> bool {
                    std::wstring msg = L"智能体刚抓取了网页，现在要启动程序。\n\n";
                    if (detail.size() > 400) msg += detail.substr(0, 400) + L"…";
                    else msg += detail;
                    msg += L"\n\n这可能来自网页内容，请确认是否允许。\n"
                        L"（脚本里直接「启动程序」不受影响，只有先抓网页再启动才会问。）";
                    const int r = MessageBoxW(UserFacingMainHwnd(), msg.c_str(),
                        L"键鼠工坊", MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
                    return r == IDYES;
                };
                agentHooks.onListWindows = [&]() -> std::wstring {
                    // 台账要排除本软件自身窗口；枚举期间也顺手藏壳，避免壳窗抢 Z 序
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    return windowmode::FormatWindowList(windowmode::ListSwitchableWindows());
                };
                agentHooks.onActivateWindow = [&](const std::wstring& query,
                                                  unsigned long pid) -> std::wstring {
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    // Alt 还按着时切窗会被预览吃掉，先落地
                    releaseAltTabIfHeld();
                    const auto all = windowmode::ListSwitchableWindows();
                    const auto hits = windowmode::MatchWindows(
                        all, query, static_cast<DWORD>(pid));
                    // 选择器的可读描述（pid 与 match 可以只给一个）
                    const std::wstring what = pid
                        ? (query.empty()
                               ? (L"(pid=" + std::to_wstring(pid) + L")")
                               : (L"「" + query + L"」(pid=" + std::to_wstring(pid) + L")"))
                        : (L"「" + query + L"」");
                    if (hits.empty()) {
                        return L"[错误] 没有匹配 " + what + L" 的窗口。"
                            L"当前窗口（Z 序）：\n" + windowmode::FormatWindowList(all)
                            + L"\n换个关键词再调；确实没开就用 runProgram/openFile 打开。";
                    }
                    // 多候选一律拒绝自动切：模糊 match（edge/excel）极易切错窗
                    if (hits.size() > 1) {
                        // ★★ 关键指引（2026-10-02 修）：以前只说「把 match 写具体」，
                        //   而台账里**明明有 pid** ⇒ 模型只能反复改 match 硬猜、绕圈。
                        //   现在明确把「用 pid 再调一次」作为首选下一步（pid 是精确的）。
                        const std::wstring advice = pid
                            ? L"该进程开了多个顶层窗口，请补 match 关键词"
                              L"（或从下面候选里挑标题更具体的那个）。"
                            : L"请**不要反复改 match 硬猜**：从下面候选里挑中意的那个，"
                              L"用它行尾的 `pid=…` 再调一次 activateWindow(pid=…)"
                              L"（pid 精确；双开同名窗口、多标签 Edge 只有它靠得住）。"
                              L"也可以把 match 写得更具体（如「历史记录」「浏览记录.xlsx」），"
                              L"但不要只写进程名 edge/excel/msedge。";
                        return L"[错误] activateWindow" + what + L" 匹配到 "
                            + std::to_wstring(hits.size())
                            + L" 个窗口，拒绝自动选择以免切错。" + advice
                            + L"\n候选：\n" + windowmode::FormatWindowList(hits);
                    }
                    const auto& target = hits.front();
                    std::wstring error;
                    const bool ok = windowmode::ActivateWindow(target.hwnd, error);
                    AppendAiDebugLog(L"  [诊断] activateWindow" + what + L" → "
                        + (ok ? L"已切到：" + target.title : L"失败：" + error));
                    if (!ok) {
                        return L"[错误] 切窗失败：" + error
                            + L"。可改用 switchWindow(action=openPreview, force=true) 兜底。";
                    }
                    if (AiLogicConvertSessionActive())
                        AiLogicConvertNoteWindowActivate(query.empty() ? what : query);
                    std::wstring out = L"已切到前台：" + target.title;
                    if (!target.processName.empty()) out += L" [" + target.processName + L"]";
                    return out;
                };
                // ★「按进程激活」抽成具名 lambda：既给 agentHooks，也给定位链路的
                // **遮挡自愈**用（定位点属于某个程序、但那个程序丢了前台时，把它拉回来
                // 再点，比回一句「请先 activateWindow」省 2~3 轮 —— 实测就是这么绕的）。
                auto activateByProcessFn = [&](const std::wstring& processName) -> std::wstring {
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    releaseAltTabIfHeld();
                    windowmode::SwitchableWindow hit;
                    std::wstring error;
                    // 刚 ShellExecute 完窗口可能尚未进 Alt+Tab 列表，短轮询
                    bool ok = false;
                    for (int i = 0; i < 8; ++i) {
                        if (i > 0) Sleep(150);
                        ok = windowmode::ActivateByProcessName(processName, &hit, error);
                        if (ok) break;
                    }
                    AppendAiDebugLog(L"  [诊断] activateByProcess「" + processName + L"」→ "
                        + (ok ? L"已切到：" + hit.title : L"失败：" + error));
                    if (!ok) {
                        return L"[错误] 按进程激活失败：" + error;
                    }
                    if (AiLogicConvertSessionActive() && !hit.title.empty())
                        AiLogicConvertNoteWindowActivate(hit.title);
                    std::wstring out = L"已按进程切到前台：" + hit.title;
                    if (!hit.processName.empty()) out += L" [" + hit.processName + L"]";
                    return out;
                };
                agentHooks.onActivateByProcess = activateByProcessFn;
                auto ensureExtensionConnected = [&](const wchar_t* what) -> std::wstring {
                    auto& bridge = windowmode::ExtBridgeServer::Instance();
                    if (bridge.IsExtensionConnected()) return {};
                    bridge.RefreshDiscovery();
                    std::wstring waitErr;
                    AppendAiDebugLog(L"  [诊断] 扩展未连接，等待本机桥 port="
                        + std::to_wstring(bridge.Port()) + L"…");
                    if (!bridge.WaitForExtension(12000, waitErr)) {
                        // ★ 文案要能指向**真正的第一步**（2026-09-27 真机）：
                        //   实测最常见的成因不是"扩展没装"，而是**扩展手里是陈旧 token**
                        //   （宿主每次启动都换 token）⇒ 先让它去点「重新连接」，
                        //   而不是让人去重装/重载扩展那条更长的路。
                        return std::wstring(L"[错误] 未连接配套扩展，无法 ")
                            + what
                            + L"。本机桥已监听 port=" + std::to_wstring(bridge.Port())
                            + L"（HTTP探测=" + std::to_wstring(bridge.HttpProbeCount())
                            + L" WS握手失败=" + std::to_wstring(bridge.WsHandshakeFailCount())
                            + L"）。请先到扩展**选项页**点「重新连接」（宿主每次启动都会换 token，"
                              L"扩展手里的旧凭证会失效）；若日志里有「WS token 不匹配」，"
                              L"那就是这一条。仍未连接再确认扩展已加载 extension\\edge；"
                              L"不想用扩展可改用 locateAndClick（纯识图）。";
                    }
                    AppendAiDebugLog(L"  [诊断] 扩展已连接");
                    return {};
                };
                agentHooks.onObservePage = [&](bool force, const std::wstring& titleHintIn,
                    const std::wstring& query, int offset) -> std::wstring {
                    if (const std::wstring wait = ensureExtensionConnected(L"observePage");
                        !wait.empty())
                        return wait;
                    auto& bridge = windowmode::ExtBridgeServer::Instance();
                    std::wstring hint = titleHintIn;
                    if (hint.empty()) {
                        HWND fg = GetForegroundWindow();
                        if (fg) {
                            wchar_t buf[512]{};
                            GetWindowTextW(fg, buf, 512);
                            hint = buf;
                        }
                    }
                    // 扩展按「标签页标题」匹配：窗口标题里的「和另外 N 个页面 - 个人 - Microsoft Edge」
                    // 必须剥掉，否则一直挂在旧标签上（实测前台已是设置页、树却还是游戏页）。
                    //
                    // ★注意顺序：剥完装饰后**不能再**用 LooksLikeBrowserWindowTitle 判断，
                    // 否则「历史记录」这种纯标签标题会被判定成「不是浏览器」而被清空 ——
                    // 那样 titleHint 根本没传给扩展，跟随标签失效，宿主侧「树与前台不一致」的
                    // 可信度校验也因为 hint 为空而永远判「可信」，截图被剥掉、模型彻底看不到界面。
                    // （这正是「卡在历史记录界面反复打开」的直接原因。）
                    const bool hintWasBrowserTitle = LooksLikeBrowserWindowTitle(hint);
                    if (hintWasBrowserTitle) {
                        const std::wstring stripped = StripBrowserWindowTitleDecorations(hint);
                        if (!stripped.empty() && stripped != hint) {
                            AppendAiDebugLog(L"  [诊断] 观察 hint 去掉窗口装饰：「" + hint
                                + L"」→「" + stripped + L"」（仅作标签标题提示）");
                            hint = stripped;
                        }
                    } else {
                        // 不是浏览器窗口标题：窗口/后台窗口模式配置/游戏绑窗名兜底，否则不给 hint
                        if (LooksLikeBrowserWindowTitle(activeWmCfg.windowName))
                            hint = activeWmCfg.windowName;
                        else
                            hint.clear();
                    }
                    std::string extra = std::string("\"force\":") + (force ? "true" : "false");
                    if (AiPageKindIsCanvas() && !force)
                        extra += ",\"light\":true";
                    if (!hint.empty())
                        extra += ",\"titleHint\":\"" + JsonEscapeUtf8(ToUtf8(hint)) + "\"";
                    const std::wstring urlHint = AiLastOpenWebpageUrl();
                    if (!urlHint.empty())
                        extra += ",\"urlHint\":\"" + JsonEscapeUtf8(ToUtf8(urlHint)) + "\"";
                    else if (!AiLastPageUrl().empty())
                        extra += ",\"urlHint\":\"" + JsonEscapeUtf8(ToUtf8(AiLastPageUrl())) + "\"";
                    if (!query.empty())
                        extra += ",\"query\":\"" + JsonEscapeUtf8(ToUtf8(query)) + "\"";
                    // ★分页：长列表页（作业/题库/搜索结果）一次给不完时，扩展按下标切片，
                    //   并回报 enumerateTotal/nextOffset。**不传 offset 时行为不变**。
                    if (offset > 0) extra += ",\"offset\":" + std::to_string(offset);
                    std::string result;
                    std::wstring err;
                    if (!bridge.Request("observePage", extra, result, err, 15000)) {
                        return L"[错误] observePage 失败：" + err;
                    }
                    PageSnapshot snap = ParsePageSnapshotJson(result);   // 可变：0 节点时重取一次
                    if (!snap.error.empty() && snap.kind == PageKind::Unknown)
                        return L"[错误] " + snap.error;
                    // ★★ **树为空（0 个可交互节点）时先"重取一次"再认输**（2026-10-02 真机实测）。
                    //
                    //   实测那一局：`nodes=0` 与 `nodes=64` **反复横跳** —— 模型就在"看图猜"
                    //   与"用控件树"之间摇摆；而那一次 VLM 大框误点（用户："还是点错位置，乱选"）
                    //   正发生在 `nodes=0` 的那一轮。
                    //   扩展刚挂载 / 页面刚跳转 / 标签切换的瞬间都可能取回空树 ⇒ 值得**重取一次**。
                    //   ⚠ 只重取一次、只等 350ms（不循环、不加长等待）：取不到就如实退回视觉，
                    //     并把"重取过仍为空"写进 why，让模型知道**这条结论来自视觉、可靠性较低**
                    //     （提示词与画面必须是同一句话；不说明就等于让它以为树是可信的）。
                    bool treeEmptyRetried = false;
                    if (snap.nodes.empty() && snap.error.empty()
                        && (snap.kind == PageKind::Dom || snap.kind == PageKind::Mixed)) {
                        AppendAiDebugLog(L"  [诊断] 控件树 0 节点 ⇒ **重取一次**"
                            L"（扩展刚挂载/页面跳转/标签切换都可能取回空树）");
                        std::this_thread::sleep_for(std::chrono::milliseconds(350));
                        std::string result2;
                        std::wstring err2;
                        if (bridge.Request("observePage", extra, result2, err2, 15000)) {
                            PageSnapshot snap2 = ParsePageSnapshotJson(result2);
                            if (!snap2.nodes.empty()) {
                                snap = snap2;
                                AppendAiDebugLog(L"  [诊断] 重取成功：nodes="
                                    + std::to_wstring(snap.nodes.size())
                                    + L"（前一次为空，已改用这一次的树）");
                            } else {
                                treeEmptyRetried = true;
                                AppendAiDebugLog(L"  [诊断] 重取仍为 0 节点 ⇒ 按「树为空」处理，"
                                    L"退回视觉兜底（结论可靠性较低）");
                            }
                        } else {
                            treeEmptyRetried = true;
                            AppendAiDebugLog(L"  [诊断] 重取失败：" + err2
                                + L" ⇒ 按「树为空」处理，退回视觉兜底");
                        }
                    }
                    AiNotePageSnapshot(snap);
                    // ★树可信度：titleHint 是「当前前台标签标题」（已剥窗口装饰），
                    // 若树上标题与它明显不符，说明扩展还挂在旧标签上。此时必须保留截图走
                    // 视觉兜底，否则模型会拿着旧页面树 + 无图地决策（实测连续 10 轮
                    // 都意识不到前台已经换成历史记录页）。
                    {
                        bool trusted = true;
                        std::wstring why;
                        const std::wstring treeTitle = Trim(snap.title);
                        const std::wstring wantTitle = Trim(hint);
                        auto normTitle = [](const std::wstring& in) {
                            std::wstring o;
                            for (wchar_t c : in) {
                                if (c == L' ' || c == L'\t') continue;
                                if (c >= L'A' && c <= L'Z')
                                    c = static_cast<wchar_t>(c - L'A' + L'a');
                                o.push_back(c);
                            }
                            return o;
                        };
                        if (!wantTitle.empty() && !treeTitle.empty()) {
                            const std::wstring a = normTitle(treeTitle);
                            const std::wstring b = normTitle(wantTitle);
                            if (a.find(b) == std::wstring::npos && b.find(a) == std::wstring::npos) {
                                trusted = false;
                                why = L"树「" + treeTitle + L"」≠ 前台「" + wantTitle + L"」";
                            }
                        }
                        // ★前台根本不是浏览器窗口（模态对话框/桌面软件）→ 树不可能是当前画面。
                        // 实测：另存为对话框（#32770）标题是空的，旧逻辑「标题读不出来就跳过比对」
                        // 于是判「可信」→ 截图被剥掉 → 模型看不到保存框，卡在保存界面乱切窗。
                        if (trusted && !AiForegroundIsBrowserWindow()) {
                            trusted = false;
                            wchar_t fgCls[64]{};
                            if (HWND fgWnd = GetForegroundWindow())
                                GetClassNameW(fgWnd, fgCls, 64);
                            why = L"前台不是浏览器窗口（"
                                + std::wstring(fgCls[0] ? fgCls : L"未知类") + L"），控件树已过期";
                        }
                        if (trusted && wantTitle.empty()) {
                            trusted = false;
                            why = L"前台标题读不出来，无法确认控件树是当前页";
                        }
                        // ★树「有标题但没用」的两种情形也必须保留截图：
                        //  · 0 个可交互节点 → 页面多半是 canvas / 内置页 / 侧边栏，DOM 看不到内容；
                        //  · 带 query 却 queryHits=0 → 树上没有模型要找的东西。
                        // 不判这两条时，模型会「拿着空树 + 没有截图」盲决策：
                        // 实测就是反复 Ctrl+H / 菜单 / 换措辞 locate 同一个目标，烧掉十几轮。
                        if (trusted && snap.nodes.empty() && snap.kind != PageKind::Unknown) {
                            trusted = false;
                            // ★ 如实说明"重取过没有"：模型据此知道自己拿的是**视觉兜底**的结论，
                            //   而不是可信控件树（两者可靠性不同，不该说成一样）。
                            why = treeEmptyRetried
                                ? L"控件树 0 个可交互节点（**已重取一次仍为空**；"
                                  L"本条结论来自视觉，可靠性较低）"
                                : L"控件树 0 个可交互节点（canvas/内置页/侧边栏，DOM 看不到）";
                        }
                        if (trusted && snap.queryHits == 0 && !snap.query.empty()) {
                            trusted = false;
                            why = L"树上没有「" + snap.query + L"」";
                        }
                        AiNotePageTreeTrusted(trusted, why);
                        if (!trusted) {
                            AppendAiDebugLog(L"  [诊断] ★控件树与前台标签不一致（" + why
                                + L"）→ 保留截图走视觉兜底，勿依赖旧树");
                        }
                    }
                    AppendAiDebugLog(L"  [诊断] 扩展 observePage pageKind="
                        + std::wstring(PageKindName(snap.kind))
                        + L" nodes=" + std::to_wstring(snap.nodes.size())
                        + (snap.url.empty() ? L"" : (L" url=" + snap.url.substr(0,
                            snap.url.size() > 60 ? 60 : snap.url.size()))));
                    std::wstring body = FormatPageSnapshotForAgent(snap);
                    // ★内置页导航核对（一次性）：上一轮 openWebpage(edge://…) 是「地址栏
                    // Ctrl+L→输入→Enter」合成的，焦点/IME 一旦不对就会静默失败；实测模型会
                    // 在「以为已经打开历史记录」的前提下继续决策，反复 Ctrl+H / 点菜单。
                    // 这里直接核对树上的 URL：没到就明说，并给出可靠的替代动作。
                    if (const std::wstring pending = AiConsumeInternalPagePendingVerify();
                        !pending.empty()) {
                        std::wstring treeLower = snap.url;
                        for (auto& c : treeLower)
                            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
                        std::wstring wantLower = pending;
                        for (auto& c : wantLower)
                            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
                        if (treeLower.rfind(wantLower, 0) != 0) {
                            AiNotePageTreeTrusted(false,
                                L"地址栏导航 " + pending + L" 未生效（树仍在 " + snap.url + L"）");
                            AppendAiDebugLog(L"  [诊断] ★内置页导航未生效：期望 " + pending
                                + L"，树仍在 " + snap.url + L" → 保留截图 + 给替代路线");
                            body += L"\n[事实] ★openWebpage(" + pending
                                + L") 没生效：控件树还停在 " + (snap.url.empty() ? L"(空)" : snap.url)
                                + L"（若 URL/标题变成「… - 搜索」，说明这个 URL 被打进搜索栏了）。"
                                  L"不要再用 Ctrl+H / Ctrl+L 重试。可靠替代："
                                  L"runProgram(targetPath=\"edge\", inputText=\""
                                + pending + L"\"，让浏览器自己打开)；"
                                  L"若你要的是浏览器侧边栏/面板（DOM 看不到），"
                                  L"用 listUiControls → invokeUiControl 或对截图 locateAndClick。";
                        }
                    }
                    return body;
                };
                auto formatRefAction = [&](const std::string& result, const std::wstring& actionLine)
                    -> std::wstring {
                    const PageSnapshot snap = ParsePageSnapshotJson(result);
                    if (!snap.error.empty() && snap.kind == PageKind::Unknown)
                        return L"[错误] " + snap.error;
                    const std::wstring prevUrl = AiLastPageUrl();
                    AiNotePageSnapshot(snap);
                    AppendAiDebugLog(L"  [诊断] 扩展 " + actionLine
                        + L" pageKind=" + std::wstring(PageKindName(snap.kind))
                        + L" nodes=" + std::to_wstring(snap.nodes.size()));
                    std::wstring body = FormatPageSnapshotForAgent(snap);
                    if (!prevUrl.empty() && !snap.url.empty()
                        && (prevUrl == snap.url || PageUrlsSameDocument(prevUrl, snap.url))) {
                        // ★只留**事实**（批 D，docs §47）：原先这两句后面还各带一条站点专属
                        //   药方（「搜人/搜词请 searchOnPage(query)」/「请点 href 含
                        //   space.bilibili.com 或 /video/ 的卡片」）——判据里写死具体域名，
                        //   而且「地址没变」本身就是完整的事实，怎么做由模型自己决定。
                        if (actionLine.find(L"typeRef") != std::wstring::npos) {
                            body += L"\n[事实] 提交后页面地址未变。";
                        } else if (actionLine.find(L"clickRef") != std::wstring::npos) {
                            body += L"\n[事实] 这一击之后页面地址未变（页内控件本来就不会换页）。";
                        }
                    }
                    if (actionLine.empty()) return body;
                    return actionLine + L"\n" + body;
                };
                agentHooks.onClickRef = [&](const std::wstring& ref, bool doubleClick) -> std::wstring {
                    if (const std::wstring wait = ensureExtensionConnected(L"clickRef");
                        !wait.empty())
                        return wait;
                    // 点击前的页面地址（下面 formatRefAction 会用新树改写会话里的 URL）
                    const std::wstring urlBeforeClick = AiLastPageUrl();
                    int sx = 0, sy = 0;
                    std::wstring clickName;
                    const bool havePt = AiLastSnapshotClickPoint(ref, sx, sy, clickName);
                    std::wstring tmpl;
                    if (AiLogicConvertSessionActive() && havePt) {
                        qst::desktop_tools::ScopedHideOwnUiForCapture hideForLogicTmpl(
                            UserFacingMainHwnd());
                        tmpl = CaptureAiLogicConvertTemplateAt(sx, sy);
                    }
                    auto& bridge = windowmode::ExtBridgeServer::Instance();
                    std::string extra = std::string("\"ref\":\"") + JsonEscapeUtf8(ToUtf8(ref))
                        + "\",\"doubleClick\":" + (doubleClick ? "true" : "false");
                    std::string result;
                    std::wstring err;
                    if (!bridge.Request("clickRef", extra, result, err, 15000)) {
                        if (err.find(L"STALE") != std::wstring::npos)
                            return L"[错误] ref 已过期，请先 observePage 再 clickRef。";
                        return L"[错误] clickRef 失败：" + err;
                    }
                    if (AiLogicConvertSessionActive()) {
                        const PageSnapshot after = ParsePageSnapshotJson(result);
                        // ★这里原先用两个**站点专属启发式**分流：某站用户空间 URL 优先记
                        //   openWebpage、「看起来像播放页」的 URL 当「不值得记的播放页」。
                        //   两个判据已删（批 D，docs §47 / D1：判据里写死域名/URL 形状 ⇒
                        //   换个网站就坏；函数名见 docs §47）。现在的口径与站点无关：
                        //   **地址变了就记 openWebpage**（逻辑转化要的是可回放的跳转），
                        //   地址没变但有定位模板才记 locate。⚠ 这段只服务「AI 逻辑转化」
                        //   会话（另一个子系统），不在 AI 动作执行链路上。
                        const bool navigated = !after.url.empty() && !urlBeforeClick.empty()
                            && after.url != urlBeforeClick
                            && !PageUrlsSameDocument(after.url, urlBeforeClick);
                        if (navigated) {
                            AiLogicConvertNoteOpenWebpage(after.url);
                        } else if (!tmpl.empty()) {
                            AiLogicConvertNoteLocate(clickName.empty() ? ref : clickName,
                                sx, sy, L"left", doubleClick ? 2 : 1, tmpl);
                        }
                    }
                    return formatRefAction(result, L"已 clickRef(" + ref + L")，新树如下（旧 ref 作废）");
                };
                agentHooks.onTypeRef = [&](const std::wstring& ref, const std::wstring& text,
                    bool clearFirst, bool submit) -> std::wstring {
                    if (const std::wstring wait = ensureExtensionConnected(L"typeRef");
                        !wait.empty())
                        return wait;
                    auto& bridge = windowmode::ExtBridgeServer::Instance();
                    std::string extra = std::string("\"ref\":\"") + JsonEscapeUtf8(ToUtf8(ref))
                        + "\",\"text\":\"" + JsonEscapeUtf8(ToUtf8(text))
                        + "\",\"clearFirst\":" + (clearFirst ? "true" : "false")
                        + ",\"submit\":" + (submit ? "true" : "false");
                    std::string result;
                    std::wstring err;
                    if (!bridge.Request("typeRef", extra, result, err, 15000)) {
                        if (err.find(L"STALE") != std::wstring::npos)
                            return L"[错误] ref 已过期，请先 observePage 再 typeRef。";
                        return L"[错误] typeRef 失败：" + err;
                    }
                    return formatRefAction(result, L"已 typeRef(" + ref + L")，新树如下（旧 ref 作废）");
                };
                agentHooks.onNavigatePage = [&](const std::wstring& url, const std::wstring& query)
                    -> std::wstring {
                    if (const std::wstring wait = ensureExtensionConnected(L"searchOnPage");
                        !wait.empty())
                        return wait;
                    auto& bridge = windowmode::ExtBridgeServer::Instance();
                    std::string extra = std::string("\"url\":\"") + JsonEscapeUtf8(ToUtf8(url))
                        + "\"";
                    if (!query.empty())
                        extra += ",\"query\":\"" + JsonEscapeUtf8(ToUtf8(query)) + "\"";
                    const std::wstring urlHint = AiLastOpenWebpageUrl();
                    if (!urlHint.empty())
                        extra += ",\"urlHint\":\"" + JsonEscapeUtf8(ToUtf8(urlHint)) + "\"";
                    else if (!AiLastPageUrl().empty())
                        extra += ",\"urlHint\":\"" + JsonEscapeUtf8(ToUtf8(AiLastPageUrl())) + "\"";
                    std::string result;
                    std::wstring err;
                    if (!bridge.Request("navigatePage", extra, result, err, 20000)) {
                        return L"[错误] searchOnPage 失败：" + err;
                    }
                    const PageSnapshot snap = ParsePageSnapshotJson(result);
                    if (!snap.error.empty() && snap.kind == PageKind::Unknown)
                        return L"[错误] " + snap.error;
                    AiNotePageSnapshot(snap);
                    // 导航是宿主显式发起并已切到目标标签，树必然对应前台 → 重置可信度
                    AiNotePageTreeTrusted(true);
                    if (snap.url.empty())
                        AiNotePageUrl(url);
                    AppendAiDebugLog(L"  [诊断] 扩展 navigatePage pageKind="
                        + std::wstring(PageKindName(snap.kind))
                        + L" nodes=" + std::to_wstring(snap.nodes.size())
                        + (snap.url.empty() ? L"" : (L" url=" + snap.url.substr(0,
                            snap.url.size() > 60 ? 60 : snap.url.size()))));
                    return FormatPageSnapshotForAgent(snap);
                };
                // ── 浏览器 DOM 优先点击（配套扩展针对性优化）───────────────────
                // 「点击 X」类目标在网页上先按控件名问扩展要树：名字命中就直接 clickRef
                // 精确点击——不截屏、不上传识图（省 1~2 轮 VLM），也不会点到相邻像素。
                // 仅当「扩展已连接 + 前台确像浏览器 + 非 canvas 页」才尝试；任何一步不满足
                // 都立刻回落到下面的识图管线，行为与改动前一致。对齐 Midscene「DOM 优先、
                // 视觉兜底」与 Stagehand 单次 act(prompt) 的原语设计。
                auto tryDomClickViaExtension = [&](const std::wstring& targetDesc,
                    bool doubleClick, bool rightButton, std::wstring& outWhy) -> std::wstring {
                    outWhy.clear();
                    std::wstring fgTitle;
                    if (HWND fg = GetForegroundWindow()) {
                        wchar_t buf[512]{};
                        GetWindowTextW(fg, buf, 512);
                        fgTitle = buf;
                    }
                    // 门禁集中判定（可自检）：判错会点到别的窗口，是准确度关键路径
                    DomFirstActionGateInput gate;
                    gate.extensionConnected =
                        windowmode::ExtBridgeServer::Instance().IsExtensionConnected();
                    gate.hasDomHooks = agentHooks.onObservePage && agentHooks.onClickRef;
                    gate.pageIsCanvas = AiPageKindIsCanvas();
                    gate.foregroundLooksBrowser = LooksLikeBrowserWindowTitle(fgTitle)
                        || AiForegroundIsBrowserWindow();
                    // 标题读不出来（模态对话框标题常为空）时不能放行 —— 看窗口类
                    gate.foregroundBrowserClass = ForegroundWindowIsBrowserClass();
                    gate.webSessionActive = AiWebBrowseSessionActive();
                    gate.foregroundTitleReadable = !fgTitle.empty();
                    gate.rightButton = rightButton;
                    if (!ShouldUseDomFirstAction(gate, &outWhy)) return {};
                    const std::wstring keyword = PageSnapshotTargetKeyword(targetDesc);
                    // offset=0：这条是「locateAndClick 前的树上命中尝试」，按需取第一段即可
                    //（命中项通常在可视区，分页对它的意义不大）。
                    const std::wstring tree = agentHooks.onObservePage(false, fgTitle, keyword, 0);
                    if (tree.rfind(L"[错误]", 0) == 0) {
                        outWhy = L"observePage 失败";
                        return {};
                    }
                    const PageSnapshot snap = AiLastPageSnapshot();
                    const PageSnapshotPick pick = PickPageSnapshotRefForText(snap, targetDesc);
                    if (pick.ref.empty()) {
                        outWhy = L"树上名字无可信命中";
                        return {};
                    }
                    if (pick.ambiguous) {
                        outWhy = L"树上命中歧义(候选 "
                            + std::to_wstring(pick.candidates) + L" 项)";
                        return {};
                    }
                    AppendAiDebugLog(L"  [诊断] DOM 优先：树上命中「" + pick.name + L"」["
                        + pick.role + L" " + pick.ref + L"] score="
                        + std::to_wstring(pick.score)
                        + L"（同名前 " + std::to_wstring(pick.sameNameCount)
                        + L" 项）→ clickRef，省整屏截图+识图");
                    const std::wstring clicked = agentHooks.onClickRef(pick.ref, doubleClick);
                    if (clicked.rfind(L"[错误]", 0) == 0) {
                        outWhy = L"clickRef 失败";
                        return {};
                    }
                    const PageSnapshot after = AiLastPageSnapshot();
                    std::wstring brief;
                    int shown = 0;
                    for (const auto& n : after.nodes) {
                        if (n.ref.empty() || Trim(n.name).empty()) continue;
                        if (!brief.empty()) brief += L"; ";
                        brief += n.role.empty() ? L"generic" : n.role;
                        brief += L" \"" + Trim(n.name) + L"\" [" + n.ref + L"]";
                        if (++shown >= 6) break;
                    }
                    std::wstring head = L"locateAndClick 已按控件树点击「" + Trim(pick.name)
                        + L"」(" + pick.ref + L"，DOM 精确点击，未截屏/未识图)";
                    // ★★ 重复点同一目标 ⇒ **如实报一句事实**（不拒绝、不替模型决定）。
                    //   依据：开关类控件（点赞/关注/收藏/开关）再点一次通常是**取消**，
                    //   而这类控件的状态常常只有画面能确认（见 DOM 模式的带图判据）。
                    {
                        const std::wstring key = Trim(pick.name);
                        int prior = 0;
                        for (const auto& k : aiDomClickedKeys) {
                            if (!key.empty() && k == key) ++prior;
                        }
                        if (prior > 0) {
                            head += L"\n[事实] 本次动作里你已经点过「" + key + L"」"
                                + std::to_wstring(prior) + L" 次（同一个目标）。"
                                L"**开关类**控件（点赞/关注/收藏/开关）再点一次通常是**取消**；"
                                L"要确认当前状态请看本轮的截图/控件树里的状态字段，"
                                L"别凭「我点过了」下结论。";
                        }
                        if (!key.empty()) aiDomClickedKeys.push_back(key);
                    }
                    if (!after.url.empty() || !after.title.empty()) {
                        head += L"\n新页面：" + (after.title.empty() ? L"(无标题)" : after.title);
                        if (!after.url.empty()) head += L" | " + after.url;
                    }
                    if (!brief.empty()) head += L"\n树上前几项：" + brief;
                    return head;
                };
                // ── 桌面 UIA 优先点击（第三基底；不烧 vision token）─────────────
                // 对齐微软 UFO：枚举前台窗口的可交互控件 → 按名字选中 → 校验后
                // InvokePattern 触发（不可 Invoke 时才点矩形中心）。命中即省掉整屏截图
                // + 1~2 轮 VLM。仅左键单击、非窗口/后台窗口模式、前台不是浏览器/自己时尝试；
                // 任何一步不满足都回落识图，行为与改动前一致。
                auto tryUiaClickFirst = [&](const std::wstring& targetDesc,
                    std::wstring& outWhy) -> std::wstring {
                    outWhy.clear();
                    HWND fg = GetForegroundWindow();
                    if (!fg || !IsWindow(fg)) {
                        outWhy = L"无前台窗口";
                        return {};
                    }
                    const HWND fgRoot = GetAncestor(fg, GA_ROOT);
                    const HWND own = UserFacingMainHwnd();
                    if (own && fgRoot && fgRoot == GetAncestor(own, GA_ROOT)) {
                        outWhy = L"前台是本软件窗口";
                        return {};
                    }
                    wchar_t titleBuf[512]{};
                    GetWindowTextW(fg, titleBuf, 512);
                    // 注意：**不要**因为「前台是浏览器」就跳过 UIA。浏览器的扩展可访问性树
                    // 只覆盖网页内容，覆盖不到浏览器外壳（地址栏、标签、「…」菜单、菜单项）；
                    // 而这些恰恰在 UIA 里有准确名字。DOM 路径已经先试过并失败了，这里必须继续试。
                    const auto items = windowmode::ListInteractiveUiControls(fg, 60);
                    if (items.empty()) {
                        outWhy = L"UIA 未枚举到可交互控件";
                        return {};
                    }
                    bool ambiguous = false;
                    const int pickIdx = windowmode::PickUiControlByName(items, targetDesc, &ambiguous);
                    // ★★ **目标描述的「控件种类」必须与命中的种类相符**（2026-09-30 实测事故）。
                    //
                    //   实测：目标写「顶部搜索框」⇒ UIA 命中一个叫「搜索」的**按钮**并
                    //   `InvokePattern` ⇒ 那一击打开了**豆包自己的搜索浮层**，把正在对话的
                    //   网页盖住 ⇒ 之后读回答全乱、用户看到"卡在那里半天没反应"。
                    //   判据只看**模型自己写的词**（输入框/搜索框/文本框/编辑框/地址栏 vs 按钮），
                    //   不看站点、不看应用 —— 换任何页面都成立。
                    //   ⚠ 拒选后**不点**，让调用方回落到元素索引/识图：那里照样能找到真正的输入框。
                    if (pickIdx >= 0) {
                        const int wantKind = [&] {
                            const std::wstring& t = targetDesc;
                            const bool inputish =
                                t.find(L"输入框") != std::wstring::npos
                                || t.find(L"搜索框") != std::wstring::npos
                                || t.find(L"搜索栏") != std::wstring::npos
                                || t.find(L"输入栏") != std::wstring::npos
                                || t.find(L"文本框") != std::wstring::npos
                                || t.find(L"编辑框") != std::wstring::npos
                                || t.find(L"地址栏") != std::wstring::npos
                                || t.find(L"填写") != std::wstring::npos;
                            if (inputish) return 1;
                            if (t.find(L"按钮") != std::wstring::npos
                                || t.find(L"摁钮") != std::wstring::npos) return 2;
                            return 0;
                        }();
                        if (wantKind != 0) {
                            const std::wstring& ct = items[static_cast<size_t>(pickIdx)].controlType;
                            const bool hitIsInput =
                                ct.find(L"输入") != std::wstring::npos
                                || ct.find(L"编辑") != std::wstring::npos
                                || ct.find(L"文本") != std::wstring::npos
                                || ct.find(L"文档") != std::wstring::npos
                                || ct.find(L"组合框") != std::wstring::npos;
                            const bool hitIsButton = ct.find(L"按钮") != std::wstring::npos;
                            if ((wantKind == 1 && hitIsButton) || (wantKind == 2 && hitIsInput)) {
                                outWhy = (wantKind == 1)
                                    ? L"UIA 只匹配到同名的按钮（目标要的是输入框）"
                                    : L"UIA 只匹配到同名的输入框（目标要的是按钮）";
                                AppendAiDebugLog(L"  [诊断] UIA 拒选：「"
                                    + items[static_cast<size_t>(pickIdx)].name + L"」是[" + ct
                                    + L"]，而目标「" + targetDesc + L"」要的是"
                                    + (wantKind == 1 ? L"**输入框**" : L"**按钮**")
                                    + L" ⇒ 不按名字触发（实测这样会打开另一个浮层/页面、"
                                      L"把正在用的界面盖住），回落元素索引/识图");
                                return {};
                            }
                        }
                    }
                    if (pickIdx < 0) {
                        // ★命中「只匹配到窗口自身标题栏按钮」时给可执行的解释，而不是笼统的
                        //   「无可信命中」——实测模型想关游戏内的卡牌面板，目标写「关闭」，
                        //   一按下去把整个游戏窗口关掉了。挑不中是有意为之（见
                        //   PickUiControlByName 的 titleBarControl 过滤），必须说清为什么。
                        const std::wstring wantLower = [&]() {
                            std::wstring w = Trim(targetDesc);
                            for (auto& c : w) {
                                if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
                            }
                            return w;
                        }();
                        for (const auto& it : items) {
                            if (!it.titleBarControl) continue;
                            std::wstring n = Trim(it.name);
                            for (auto& c : n) {
                                if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
                            }
                            if (n == wantLower) {
                                AppendAiDebugLog(L"  [诊断] UIA 拒绝窗口自身按钮「" + it.name
                                    + L"」（按名字点它会关掉整个窗口），回落识图/换描述");
                                return L"[错误] 「" + it.name
                                    + L"」是**窗口自身**的标题栏按钮（关/最小化/最大化），"
                                      L"不是应用里的按钮；按名字点它会把整个窗口关掉"
                                      L"（实测把游戏窗口关没了）。"
                                      L"请改用更具体的描述指向**应用内**的关闭控件"
                                      L"（如「卡牌面板右上角的 X」），或用 locateAndClick 看图定位。"
                                      L"确实要退出程序：closeProgram(targetPath=进程名) / keyClick(Alt+F4)。";
                            }
                        }
                        outWhy = L"UIA 控件名无可信命中";
                        return {};
                    }
                    if (ambiguous) {
                        outWhy = L"UIA 命中歧义（近似竞争项）";
                        return {};
                    }
                    const windowmode::UiControlInfo& hit =
                        items[static_cast<size_t>(pickIdx)];
                    if (!hit.enabled) {
                        // 原来要先识图定位、再 UIA 探测才发现是灰控件；现在提前一轮拦掉
                        if (const std::wstring memo = hit.name; !memo.empty())
                            AppendAiTaskMemoLine(L"blocked: disabled " + memo);
                        return L"[错误] 目标「" + targetDesc + L"」当前是灰色不可用状态"
                            L"（UIA 控件名：「" + hit.name + L"」），点击已被拦截。"
                            L"灰掉=前置条件没满足，不是位置点错：先修正输入/必填项再提交。";
                    }
                    std::wstring actualName;
                    std::wstring warn;
                    int actualId = 0;
                    RECT rc{};
                    bool invoked = false;
                    bool shellIconItem = false;
                    if (!windowmode::InvokeUiControlByName(hit.name, hit.id, actualName,
                            actualId, rc, invoked, warn, &shellIconItem)) {
                        outWhy = L"UIA 元素已失效（界面可能刚变）";
                        return {};
                    }
                    const int cx = (rc.left + rc.right) / 2;
                    const int cy = (rc.top + rc.bottom) / 2;
                    AppendAiDebugLog(L"  [诊断] UIA 优先：命中「" + actualName + L"」["
                        + hit.controlType + L" id=" + std::to_wstring(actualId) + L"]"
                        + (invoked ? L" → InvokePattern" : L" → 点矩形中心")
                        + (shellIconItem ? L"（桌面/资源管理器图标：补双击才等于「打开」）" : L"")
                        + L"（未整屏截图/未识图）"
                        + (warn.empty() ? L"" : (L"；" + warn)));
                    // ★★ 桌面/资源管理器里的图标：shell 的 InvokePattern **只做"选中"**，
                    //   用户语义里的"打开"必须**双击**（实测 2026-09-29：点桌面「Edge」/
                    //   列表项「Microsoft Edge」后回执说"已触发"，浏览器却没起来 ⇒ 模型瞎试）。
                    if (shellIconItem) {
                        notePointerClick(cx, cy);
                        parkCursorAwayFromUi();
                        const std::wstring dblJson =
                            BuildScreenClickActionsJson(cx, cy, false, L"left", 2);
                        (void)executeActionsJsonNow(dblJson);
                        std::wstring out2 = L"locateAndClick 已**双击**「" + actualName
                            + L"」（桌面/资源管理器图标：单击只选中，双击才是打开）"
                              L" 屏幕(" + std::to_wstring(cx) + L"," + std::to_wstring(cy) + L")"
                              L"（UIA 精确命中，未截屏/未识图）";
                        if (!warn.empty()) out2 += L"\n[警告] " + warn;
                        return out2;
                    }
                    if (!invoked) {
                        // 无 InvokePattern 的控件（列表项/树项）：仍是点中心，但按 UIA
                        // 矩形而不是像素猜测；同屏重复点击守卫照旧生效
                        notePointerClick(cx, cy);
                        parkCursorAwayFromUi();
                        const std::wstring clickJson =
                            BuildScreenClickActionsJson(cx, cy, false, L"left", 1);
                        (void)executeActionsJsonNow(clickJson);
                    }
                    std::wstring out = L"locateAndClick 已按 UIA 控件「" + actualName + L"」"
                        + (invoked ? L"触发（InvokePattern，未打像素）" : L"点击控件中心")
                        + L" 屏幕(" + std::to_wstring(cx) + L"," + std::to_wstring(cy) + L")"
                        + L"（UIA 精确命中，未截屏/未识图）";
                    if (!warn.empty()) out += L"\n[警告] " + warn;
                    return out;
                };
                agentHooks.onListUiControls = [&](int maxCount, const std::wstring& typeFilter,
                    const std::wstring& nameFilter) -> std::wstring {
                    HWND fg = GetForegroundWindow();
                    if (!fg || !IsWindow(fg)) return L"[错误] 没有前台窗口。";
                    wchar_t titleBuf[512]{};
                    GetWindowTextW(fg, titleBuf, 512);
                    int offscreenSkipped = 0;
                    auto items = windowmode::ListInteractiveUiControls(
                        fg, maxCount, &offscreenSkipped);

                    // ★ 过滤在**宿主侧**做完再回传：模型只要那几条列表项时，别把
                    //   几十个按钮塞进上下文（省 token，也省得它自己数）。
                    auto containsNoCase = [](const std::wstring& hay, const std::wstring& needle) {
                        if (needle.empty()) return true;
                        std::wstring h = hay, n = needle;
                        for (auto& c : h) c = static_cast<wchar_t>(towlower(c));
                        for (auto& c : n) c = static_cast<wchar_t>(towlower(c));
                        return h.find(n) != std::wstring::npos;
                    };
                    const int totalBefore = static_cast<int>(items.size());
                    if (!typeFilter.empty() || !nameFilter.empty()) {
                        items.erase(std::remove_if(items.begin(), items.end(),
                            [&](const windowmode::UiControlInfo& c) {
                                if (!containsNoCase(c.controlType, typeFilter)) return true;
                                return !containsNoCase(c.name, nameFilter);
                            }), items.end());
                    }

                    if (items.empty()) {
                        std::wstring err = L"[错误] 前台窗口「" + std::wstring(titleBuf)
                            + L"」UIA 枚举不到可交互控件（自绘界面/游戏/未实现 UIA）。"
                              L"网页内容请用 observePage/clickRef；"
                              L"自绘界面请改用 locateAndClick 识图点击。";
                        if (!typeFilter.empty() || !nameFilter.empty()) {
                            err = L"[错误] 前台窗口「" + std::wstring(titleBuf)
                                + L"」有 " + std::to_wstring(totalBefore)
                                + L" 个可交互控件，但**没有**匹配 typeFilter/nameFilter 的。"
                                  L"\n★ 去掉过滤再列一次看全部（类型名以回执里写的为准）。";
                        }
                        if (offscreenSkipped > 0) {
                            err += L"\n★ 但有 " + std::to_wstring(offscreenSkipped)
                                + L" 个控件在**视口外**（列表没滚到）—— 先用 scrollWheel 滚动再重列。";
                        }
                        return err;
                    }
                    AppendAiDebugLog(L"  [诊断] listUiControls: 前台「"
                        + std::wstring(titleBuf) + L"」枚举到 "
                        + std::to_wstring(items.size()) + L" 个可交互控件（未截屏）"
                        + ((!typeFilter.empty() || !nameFilter.empty())
                            ? L"，过滤后 " + std::to_wstring(items.size())
                                + L"/" + std::to_wstring(totalBefore)
                            : L"")
                        + (offscreenSkipped > 0
                            ? L"，另有 " + std::to_wstring(offscreenSkipped) + L" 个在视口外"
                            : L""));
                    std::wstring out = L"前台窗口：" + std::wstring(titleBuf) + L"\n";
                    if (!typeFilter.empty() || !nameFilter.empty()) {
                        out += L"（已过滤：共 " + std::to_wstring(totalBefore) + L" 条，匹配 "
                            + std::to_wstring(items.size()) + L" 条）\n";
                    }
                    // 预算从 2000 提到 3600：列表型界面（历史记录/书签/文件列表）
                    // 每行约 40–60 字符，2000 只够 40 条出头 —— 正是日志里「只拿到 47 条」的来源。
                    out += windowmode::FormatUiControlListForAgent(items, 3600);
                    if (offscreenSkipped > 0) {
                        // ★ 这条是给模型的**行动依据**：列表比看上去长，得先滚动。
                        out += L"★ 还有 " + std::to_wstring(offscreenSkipped)
                            + L" 个可交互控件在**视口外**没列出（滚动列表的常见情况）。"
                              L"要它们就先 scrollWheel 往下滚，再 listUiControls 一次；"
                              L"已知准确名字的话可以直接 invokeUiControl(name=…)，不必先列出。\n";
                    }
                    if (LooksLikeBrowserWindowTitle(titleBuf)) {
                        out += L"（浏览器：这里只有外壳控件——地址栏/标签/菜单；"
                               L"网页里的按钮请用 observePage/clickRef。"
                               L"⚠ 但 edge://history、edge://bookmarks 这类**内置页**扩展无权注入，"
                               L"observePage 必然失败 —— 这种页面只能走本工具的 UIA 路线。）\n";
                    }
                    out += L"用法：invokeUiControl(id=编号, name=\"名字\")；编号可能因界面变化"
                           L"错位，name 必须以这里列出的为准。";
                    return out;
                };
                agentHooks.onInvokeUiControl = [&](int id, const std::wstring& name,
                    bool observeAfter) -> std::wstring {
                    std::wstring actualName;
                    std::wstring warn;
                    int actualId = 0;
                    RECT rc{};
                    bool invoked = false;
                    bool shellIconItem = false;
                    if (!windowmode::InvokeUiControlByName(name, id, actualName, actualId, rc,
                            invoked, warn, &shellIconItem)) {
                        // 别只回「找不到」——那会逼模型再花一轮 listUiControls。
                        // 直接把最接近的候选名字带回去（模型常从对话框探测文本里抄名字，
                        // 例如把 Win32 按钮文本「保存(&S)」当成 UIA 名）。
                        const auto items = windowmode::ListInteractiveUiControls(
                            GetForegroundWindow(), 60);
                        std::wstring cands;
                        int shown = 0;
                        const wchar_t first = name.empty() ? 0 : name[0];
                        for (const auto& it : items) {
                            if (shown >= 6) break;
                            if (it.name.empty()) continue;
                            const bool sameStart = first && it.name[0] == first;
                            const bool contains = name.size() >= 2
                                && it.name.find(name.substr(0, 2)) != std::wstring::npos;
                            if (!sameStart && !contains) continue;
                            if (!cands.empty()) cands += L" / ";
                            cands += L"「" + it.name + L"」";
                            ++shown;
                        }
                        std::wstring out = L"[错误] 前台窗口里找不到名字像「" + name
                            + L"」的可交互控件（界面可能已变）。";
                        if (!cands.empty()) out += L"\n最接近的候选：" + cands + L"（按台账原样传名）";
                        else out += L"\n台账里没有相近名字：请 listUiControls 看准确名字。";
                        out += L"\n若你要的是输入框/标签文字（如「文件名:」）：台账只列按钮/菜单/输入类控件，"
                               L"填内容请直接在焦点框 quickInput，别用本工具。";
                        return out;
                    }
                    const int cx = (rc.left + rc.right) / 2;
                    const int cy = (rc.top + rc.bottom) / 2;
                    std::wstring how;
                    if (shellIconItem) {
                        // ★★ 桌面/资源管理器图标：shell 的 Invoke 只"选中" ⇒ **双击才是打开**
                        notePointerClick(cx, cy);
                        parkCursorAwayFromUi();
                        const std::wstring dblJson =
                            BuildScreenClickActionsJson(cx, cy, false, L"left", 2);
                        const std::wstring execMsg = executeActionsJsonNow(dblJson);
                        if (execMsg.rfind(L"[错误]", 0) == 0) return execMsg;
                        how = L"**双击**（桌面/资源管理器图标：单击只选中，双击才是打开）";
                    } else if (invoked) {
                        how = L"InvokePattern 触发（未打像素）";
                    } else {
                        notePointerClick(cx, cy);
                        parkCursorAwayFromUi();
                        const std::wstring clickJson =
                            BuildScreenClickActionsJson(cx, cy, false, L"left", 1);
                        const std::wstring execMsg = executeActionsJsonNow(clickJson);
                        how = L"点击控件中心";
                        if (execMsg.rfind(L"[错误]", 0) == 0) return execMsg;
                    }
                    AppendAiDebugLog(L"  [诊断] invokeUiControl:「" + actualName + L"」id="
                        + std::to_wstring(actualId) + L" → " + how
                        + (warn.empty() ? L"" : (L"（" + warn + L"）")));
                    (void)observeAfter;  // 标记由工具层按调用参数补（宿主只回纯文本）
                    std::wstring out = L"invokeUiControl 已触发「"
                        + actualName + L"」(id=" + std::to_wstring(actualId) + L"，" + how
                        + L") 屏幕(" + std::to_wstring(cx) + L"," + std::to_wstring(cy) + L")";
                    if (!warn.empty()) out += L"\n[警告] " + warn;
                    return out;
                };
                std::function<std::wstring(const std::wstring&, int, const std::wstring&, int, int)>
                    locateAndClickFn = nullptr;
                locateAndClickFn =
                    [&](const std::wstring& targetDescIn, int refineLevels,
                        const std::wstring& button, int clickCount,
                        int elementId) -> std::wstring {
                    LocateAndClickNestGuard locateGuard;
                    if (!locateGuard.entered()) {
                        return L"[错误] locateAndClick 不可嵌套（防无限外包定位）。";
                    }
                    // ★★按**编号**直查本帧元素索引（docs §32.4 缺的那「最后一公里」）。
                    //   起因（审计实测）：索引早就给模型编号与坐标了，但**没有任何工具吃得下编号**
                    //   —— `invokeUiControl` 的 id 只作交叉校验且只覆盖 UIA 条目，
                    //   于是模型「看得见编号、用不上编号」，只能退回写**名字**去匹配；
                    //   同屏多个同名条目（「确定」「600」）按名字会判歧义 ⇒ 直接回落整轮 VLM 识图。
                    //   现在：给编号就按编号拿坐标，确定命中，0 次识图。
                    std::wstring targetDesc = targetDescIn;
                    if (elementId > 0) {
                        const AiElementEntry* hit =
                            AiElementIndexById(aiElementIndexThisFrame, elementId);
                        if (!hit) {
                            // ⚠ 编号**只在本帧有效**，且窗口自身按钮/灰控件永不入选：
                            //   查不到就如实说清，绝不退化成「按名字猜」
                            //   （猜错 = 点到别处，比回落识图糟得多）。
                            return L"[错误] 元素索引里没有可用编号 "
                                + std::to_wstring(elementId) + L"（本帧索引 "
                                + std::to_wstring(aiElementIndexThisFrame.size())
                                + L" 条；编号只在该帧清单里有效，窗口自身按钮与灰控件不在表内）。"
                                  L"请按清单里最新的编号调用，或改用 target=按名字定位。";
                        }
                        // 编号命中即把 target 换成该条目名字：后续链路（保存对话拦截、
                        // 名字解析兜底、回执文案）都建立在「target 是屏幕上的短名字」之上。
                        if (Trim(targetDesc).empty()) targetDesc = hit->name;
                        AppendAiDebugLog(L"  [诊断] locateAndClick 按编号 ["
                            + std::to_wstring(elementId) + L"] 直查索引：「" + hit->name
                            + L"」（" + AiElementSourceName(hit->source) + L"）");
                    }
                    // ★记下「这一击想点谁」——落点标注（AiFrameClickMark）要靠它把
                    // 画面上的红叉和一句人话对上；引擎是唯一同时知道坐标和目标描述的地方。
                    SetAiActionClickIntent(targetDesc);
                    // 网页左键：先试 DOM（右键门禁在 ShouldUseDomFirstAction 里挡住）
                    // ⚠ 按编号时**跳过 DOM/UIA 两档**：模型已经指名了索引里的某一条，
                    //   改道按名字去别处找就等于不听它说的（索引就是「所见即所得」那张表）。
                    if (button != L"right" && elementId <= 0) {
                        std::wstring why;
                        const std::wstring domMsg = tryDomClickViaExtension(
                            targetDesc, clickCount >= 2, false, why);
                        if (!domMsg.empty()) return domMsg;
                        AppendAiDebugLog(L"  [诊断] DOM 优先未命中（" + why + L"），试 UIA");
                        // 桌面：DOM 不适用，再试 UIA（同样不烧 vision token）
                        if (clickCount <= 1 && !wmUsesTarget()) {
                            std::wstring uiaWhy;
                            const std::wstring uiaMsg = tryUiaClickFirst(targetDesc, uiaWhy);
                            if (!uiaMsg.empty()) return uiaMsg;
                            AppendAiDebugLog(L"  [诊断] UIA 优先未命中（" + uiaWhy
                                + L"），试元素索引");
                        }
                    }
                    // ★★「所见即所得」：先在本帧的**统一元素索引**里查坐标 —— 0 次 VLM。
                    // 索引 = UIA 控件 ∪ OCR 文字（见 BuildAiElementIndex），模型看的就是它，
                    // 所以这里的命中率就是「模型认得出来」的命中率。
                    // 与「文字直点」的区别：那条只看 OCR 且要求唯一命中；这条是**统一表**，
                    // 还能吃到 UIA 控件名（OCR 读不准的图标/小字按钮）。
                    // 命中后走的仍是同一条点击+校验链路（遮挡校验、灰化拦截、窗口按钮拦截、
                    // 落点标注、settle 验收），只是把「定位」这一步从识图换成了查表。
                    if (button != L"right" && clickCount <= 1 && !aiElementIndexThisFrame.empty()) {
                        AiIndexResolveResult ir;
                        // near：用最近一次落点当上下文（同屏多个同名时靠它区分）
                        const bool hasNear = clickMarkX >= 0 && clickMarkY >= 0;
                        if (AiElementIndexResolve(aiElementIndexThisFrame, targetDesc,
                                hasNear, clickMarkX, clickMarkY, &ir)) {
                            AppendAiDebugLog(L"  [诊断] 元素索引直点：[" + std::to_wstring(ir.id)
                                + L"] " + AiElementSourceName(ir.source) + L"「" + ir.name
                                + L"」→ 屏幕(" + std::to_wstring(ir.x) + L","
                                + std::to_wstring(ir.y) + L")（0 次识图）");
                            // ★★ **命中"画面底部"的文字条目 ⇒ 回执要说清**（2026-09-30 实测）。
                            //
                            //   实测：模型要空间页的**排序标签**「最新发布」，索引里那条却是
                            //   页面底部（y≈84%）的同名词 ⇒ 点下去是页脚/推荐位（用户看到
                            //   "点到广告那里了"的前一次就是这类），而模型以为点对了、连点 3 轮。
                            //   ⚠ 这里**只加事实**（不改行为、不替模型判断）：页脚与正文同名的
                            //     情况在网页里很常见，把"这条在底部"说出来，模型就能自己换描述。
                            //   ⚠ 判据只对 **OCR 文字**条目生效（UIA 控件在底部是真按钮，如
                            //     聊天框的"发送"），且用窗口高度的 10% 作带。
                            {
                                HWND idxFg = GetForegroundWindow();
                                RECT idxWr{};
                                if (ir.source == AiElementSource::OcrText && idxFg
                                    && GetWindowRect(idxFg, &idxWr)) {
                                    const int wh = idxWr.bottom - idxWr.top;
                                    if (wh > 200 && ir.y > idxWr.top + wh * 9 / 10) {
                                        AppendAiDebugLog(L"  [诊断] 提示：这条文字在**画面底部**"
                                            L"（页脚/推荐位常有同名词）——若你要的是列表上方的"
                                            L"同名标签，请给更具体的描述");
                                    }
                                }
                            }
                            // 与「文字直点」同一套守卫：不点窗口自身按钮、点前查灰化/遮挡。
                            const windowmode::UiElementState idxProbe =
                                windowmode::ProbeUiElementAtPoint(ir.x, ir.y);
                            if (idxProbe.probed && idxProbe.titleBarControl) {
                                AppendAiDebugLog(L"  [诊断] 元素索引直点被拦：命中窗口自身按钮「"
                                    + idxProbe.name + L"」");
                            } else {
                                const bool idxDupCheck =
                                    (clickMarkX != ir.x || clickMarkY != ir.y);
                                aiElementIndexThisBatch = true;
                                SetAiActionClickIntent(targetDesc);
                                const std::wstring clickJson = BuildScreenClickActionsJson(
                                    ir.x, ir.y, false, button, 1);
                                clickMarkX = ir.x;
                                clickMarkY = ir.y;
                                MarkLastAiClickScreenPoint(ir.x, ir.y);
                                const std::wstring execMsg = executeActionsJsonNow(clickJson);
                                std::wstring out = L"locateAndClick 已点击索引 ["
                                    + std::to_wstring(ir.id) + L"] " + AiElementSourceName(ir.source)
                                    + L"「" + ir.name + L"」" + DescribeClickPointForModel(ir.x, ir.y,
                                        liveMapValid ? &liveMap : nullptr)
                                    + L"（**0 次识图**：命中本帧元素索引，未烧 API）";
                                // ★文字条目**如实标注可点性未知**：索引里的 OCR 文字只是
                                //   「这一帧在屏幕上读到了这几个字」，模式标签/标题/计数器都会
                                //   出现在里面。实测有模型因此去点「我是僵尸」（那是个模式标签）
                                //   和面板标题，点了没反应、白费一轮。
                                //   引擎**不知道**它能不能点 —— 那就别装作知道（只报事实）。
                                if (ir.source == AiElementSource::OcrText) {
                                    out += L"；⚠ 这是**文字**条目：引擎只保证「屏幕上有这几个字」，"
                                        L"**不保证可点**（看本批回执里的界面变化来判断）";
                                }
                                out += L"；已移开指针防 hover";
                                AppendAiTaskMemoLine(L"done: locateAndClick(索引)");
                                if (!execMsg.empty()) out += L"；" + execMsg;
                                (void)idxDupCheck;
                                return out;
                            }
                        } else if (!ir.why.empty()) {
                            AppendAiDebugLog(L"  [诊断] 元素索引未命中：" + ir.why);
                        }
                    }
                    // 整段定位+点击期间藏壳/调试窗（含 Zoom 二次截屏）
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    // ── 识图/补点落点的**共同**拦截（docs §41.1）────────────────────
                    // 为什么必须抽成**一份实现**：§31.1 那条「窗口自身标题栏按钮」守卫
                    // 当年只加在主识图链路上，而「错点自纠」是后来加的（备用项）——
                    // 它的落点**没走那道守卫**，实测后果就是**补点把整个游戏窗口关掉**
                    // （用户：「咋把游戏关了」）。两条路只要各写各的，迟早再漏一次。
                    //
                    // ⚠⚠ 而且**不能只靠 UIA**：实测游戏窗口 `UIA 控件 0 条`
                    // （`ElementFromPoint` 拿不到元素）⇒ `titleBarControl` 那道守卫
                    // **根本不会触发**；`IsScreenPointOnForegroundWindow` 只回答
                    // 「是不是这个窗口的」⇒ 标题栏照样放行。所以这里加一条**纯 Win32 几何**
                    // 判据：落在「窗口矩形内、客户区矩形外」= 标题栏/边框 ⇒ 绝不通过。
                    // 结构事实比控件树可靠 —— 任何窗口都有客户区，游戏/自绘程序一样量得到。
                    auto blockVisionLanding = [&](int sx, int sy,
                                                  windowmode::UiElementState* outUi)
                        -> std::wstring {
                        const HWND probeHwnd = (wmExecPtr && wmUsesTarget())
                            ? wmExecPtr->TargetHwnd() : nullptr;
                        const windowmode::NonClientPointInfo nc =
                            windowmode::ProbeWindowNonClientAtPoint(probeHwnd, sx, sy);
                        if (outUi) *outUi = windowmode::ProbeUiElementAtPoint(sx, sy);
                        const std::wstring at = L"屏幕(" + std::to_wstring(sx) + L","
                            + std::to_wstring(sy) + L")";
                        if (nc.nonClient) {
                            AppendAiDebugLog(L"  [诊断] 拦截窗口非客户区落点：" + at
                                + L"（窗口 " + std::to_wstring(nc.windowRect.left) + L","
                                + std::to_wstring(nc.windowRect.top) + L"-"
                                + std::to_wstring(nc.windowRect.right) + L","
                                + std::to_wstring(nc.windowRect.bottom)
                                + L" / 客户区 " + std::to_wstring(nc.clientRect.left) + L","
                                + std::to_wstring(nc.clientRect.top) + L"-"
                                + std::to_wstring(nc.clientRect.right) + L","
                                + std::to_wstring(nc.clientRect.bottom) + L"）"
                                + (nc.hint.empty() ? L"" : L"，该处元素「" + nc.hint + L"」"));
                            return L"[错误] 定位点 " + at + L" 落在**窗口自己的标题栏/边框**上"
                                L"（非客户区），不在程序内容里，**点击会把整个窗口关掉/最小化**，"
                                L"已拦截。这多半是目标描述太笼统（如只说「右上角关闭」）："
                                L"请改成指向**应用内**的具体描述（如「卡牌面板右上角的 X」），"
                                L"或先 screenshot 看清目标位置再定位。"
                                L"确实要退出程序：用 closeProgram(targetPath=进程名) "
                                L"或 keyClick(Alt+F4)；要最小化/切走：activateWindow。";
                        }
                        if (outUi && outUi->probed && outUi->titleBarControl) {
                            AppendAiDebugLog(L"  [诊断] 拦截窗口自身按钮：" + at
                                + L" 命中「" + outUi->name + L"」");
                            return L"[错误] 定位点落在**窗口自身**的标题栏按钮「" + outUi->name
                                + L"」上（" + at + L"），点击会把整个窗口关掉，已拦截。"
                                  L"这多半是目标描述太笼统（如只说「关闭」）："
                                  L"请改成指向**应用内控件**的具体描述（如「卡牌面板右上角的 X」），"
                                  L"或先 screenshot 看清面板位置再定位。"
                                  L"确实要退出程序：用 closeProgram(targetPath=进程名) 或 "
                                  L"keyClick(Alt+F4)；要最小化/切走：activateWindow 切到别的窗口。";
                        }
                        return {};
                    };

                    // 视觉兜底：缓存命中时整段跳过（不截屏、不建识图客户端、不调 API）
                    AiLocateVerdict locateVerdict = AiLocateVerdict::Suspect;
                    std::wstring locateVerdictWhy;
                    // ── 「文字定位」共用探针：在 (px,py) 附近裁一小块**现场重新识别** ──
                    // 返回 true 时把目标文字的真实框中心写进 (outX,outY)。
                    // 两个用途：① 文字直点的候选点复核（索引可能已过期）；② 识图之后的坐标覆盖
                    // （本地 OCR 读的是文字像素，比 VLM 的粗框准 —— 实测粗框低了 63px 直接射失）。
                    auto ocrProbeText = [&](const std::wstring& wantText, int px, int py,
                                            int halfW, int halfH,
                                            int* outX, int* outY, std::wstring* outNote,
                                            std::wstring* outProbeText = nullptr) -> bool {
                        if (outX) *outX = px;
                        if (outY) *outY = py;
                        if (CheckOcrEnvironment(false).state != OcrEnvState::Ready) {
                            if (outNote) *outNote = L"未安装文字识别引擎";
                            return false;
                        }
                        const int pad = 10;
                        const int rx1 = px - halfW - pad;
                        const int ry1 = py - halfH - pad;
                        const int rx2 = px + halfW + pad;
                        const int ry2 = py + halfH + pad;
                        HBITMAP region = nullptr;
                        {
                            // ★★同一条规则（docs §70）：这块局部截图是用来**判定落点上是什么文字**的。
                            //   若本软件自己最顶层的窗口（调试信息输出窗口/悬浮球）正压在这个点上，
                            //   探针读到的会是**我们自己的日志文字**，还可能据此「复核通过」
                            //   ⇒ 促成一次点在自己窗口上的点击。给模型看/给判断用的截图，
                            //   一律先把自己的窗口藏掉（DWM cloak，不闪）。
                            //   ⚠ 作用域**只包住截图**：旧窗口藏 80ms 就够，别把它挂到整段 OCR 上。
                            qst::desktop_tools::ScopedHideOwnUiForCapture hideOwnForProbe(
                                UserFacingMainHwnd());
                            region = CaptureScreenRegion(rx1, ry1, rx2, ry2);
                        }
                        if (!region) {
                            if (outNote) *outNote = L"局部截图失败";
                            return false;
                        }
                        holdOcrSession();
                        const OcrEngineOutput probe = RunOcrOnBitmap(region, rx1, ry1, false);
                        DeleteBitmapHandle(region);
                        if (!probe.success || probe.lines.empty()) {
                            if (outNote) *outNote = L"局部没认到任何文字";
                            return false;
                        }
                        // nearest 档：`AiOcrPickNearestText` 只按「离探针点最近」取一条，
                        // 不做模糊匹配 —— 判「算不算同一个标签」的活交给调用方
                        // （索引档 vs 复核档的比较见 AiOcrProbeAgreesWithIndex）。
                        // `outProbeText` 可选：把复核**实际读到的文字**交出来。
                        const int w = rx2 - rx1, h = ry2 - ry1;
                        AiOcrDirectHit ph;
                        if (!AiOcrPickNearestText(probe.lines, wantText, px, py,
                                (std::max)(w, h), w, h, &ph)) {
                            if (outNote) *outNote = ph.why.empty()
                                ? (L"局部没读到「" + wantText + L"」") : ph.why;
                            return false;
                        }
                        if (outX) *outX = ph.screenX;
                        if (outY) *outY = ph.screenY;
                        if (outProbeText) *outProbeText = ph.hitText;
                        if (outNote) {
                            *outNote = L"局部复核确认「" + ph.hitText + L"」("
                                + std::to_wstring(ph.screenX) + L","
                                + std::to_wstring(ph.screenY) + L")";
                        }
                        return true;
                    };
                    // ★文字直点：这次要点的目标**就是屏幕上一段短文字**（按钮/菜单项/标签），
                    //   而观察帧的本地 OCR 已经给出它在哪 → 直接点，不截屏、不建识图客户端、不调 API。
                    //   带文字的按钮多是「点一次就完」的一次性操作，走 DOM→UIA→两轮 VLM
                    //   这条复用阶梯是纯浪费（实测一次 ≈ 20s，模型还得再花一轮确认）。
                    //   ★索引有效期放宽到 90s 但**用之前必须就地复核**：模型「看一眼 → 想 8~40s
                    //   → 才动手」是常态（实测日志 `OCR 索引已过期(7969ms) → 回落识图`），
                    //   6s 的 TTL 几乎永远命中不了；而时间长短本身说明不了画面有没有变，
                    //   裁一小块重新识别一次（几十毫秒）才是可靠的验证。
                    bool ocrDirectHit = false;
                    int ocrDirectX = 0, ocrDirectY = 0;
                    if (AiFastPathsEnabled()
                        && button == L"left" && clickCount == 1) {
                        const auto& ocrIdx = OcrScreenIndex();
                        if (!ocrIdx.lines.empty()) {
                            const long long age = static_cast<long long>(GetTickCount64())
                                - ocrIdx.stampMs;
                            std::wstring wantText;
                            if (age > kOcrScreenIndexFreshMs) {
                                AppendAiDebugLog(L"  [诊断] 文字直点：OCR 索引已过期("
                                    + std::to_wstring(age) + L"ms) → 回落识图");
                            } else if (ocrIdx.hwnd != GetForegroundWindow()) {
                                // 前台窗口换了：索引里的字坐标是别的窗口的 → 一律作废
                                AppendAiDebugLog(L"  [诊断] 文字直点：前台窗口已切换，"
                                    L"OCR 索引作废 → 回落识图");
                            } else if (!AiLocateExtractOcrTarget(targetDesc, &wantText)) {
                                // 图标/方位/序号/格子类目标：OCR 对不上号，正常走识图
                            } else {
                                AiOcrDirectHit hit;
                                const bool picked = AiOcrPickDirectClickTarget(ocrIdx.lines,
                                    wantText, ocrIdx.capX2 - ocrIdx.capX1,
                                    ocrIdx.capY2 - ocrIdx.capY1, &hit);
                                if (picked && hit.screenX >= ocrIdx.capX1
                                    && hit.screenX < ocrIdx.capX2
                                    && hit.screenY >= ocrIdx.capY1 && hit.screenY < ocrIdx.capY2) {
                                    int vx = hit.screenX, vy = hit.screenY;
                                    std::wstring vnote;
                                    // 复核窗口按命中框尺寸给（框大时裁小了会把文字切一半）
                                    const int phw = (std::max)(60, hit.boxW / 2 + 12);
                                    const int phh = (std::max)(28, hit.boxH / 2 + 10);
                                    // 复核**必须自己再读一遍**（确认画面没变），但采信规则
                                    // 不能比索引那次更严：小字标签二次识别常被裁切/重采样
                                    // 读得更差，要求「完全相等」等于让复核永远不通过。
                                    // 实测：索引 contains 命中「一键全选」→ 复核读成「键全选」
                                    // → 判「就地复核未通过」→ 白落回一整轮 VLM 识图
                                    // （一次识图 100KB+/15~45s，而本地 OCR 只要几十毫秒）。
                                    int px2 = 0, py2 = 0;
                                    std::wstring pnote2;
                                    std::wstring probeText;
                                    const bool probeRead = ocrProbeText(wantText,
                                        hit.screenX, hit.screenY, phw, phh,
                                        &px2, &py2, &pnote2, &probeText);
                                    const bool verified = probeRead
                                        && AiOcrProbeAgreesWithIndex(probeText, hit.hitText,
                                            wantText, px2, py2, hit.screenX, hit.screenY);
                                    if (verified) {
                                        vx = px2;
                                        vy = py2;
                                        vnote = pnote2.empty()
                                            ? L"局部复核通过" : pnote2;
                                    }
                                    if (verified) {
                                        ocrDirectHit = true;
                                        ocrDirectX = vx;
                                        ocrDirectY = vy;
                                        AppendAiDebugLog(L"  [诊断] 文字直点：本地 OCR 索引命中「"
                                            + hit.hitText + L"」(" + hit.matchKind + L") → 屏幕("
                                            + std::to_wstring(ocrDirectX) + L","
                                            + std::to_wstring(ocrDirectY)
                                            + L")，" + vnote + L"，未截屏未识图（省 1~2 轮 API）");
                                    } else {
                                        AppendAiDebugLog(L"  [诊断] 文字直点：候选「" + hit.hitText
                                            + L"」就地复核未通过（" + vnote + L"）→ 回落识图");
                                    }
                                } else if (!hit.why.empty()) {
                                    AppendAiDebugLog(L"  [诊断] 文字直点不可用：" + hit.why
                                        + L" → 回落识图");
                                }
                            }
                        }
                    }
                     auto runVisionLocate = [&]() -> ZoomRefineLocateResult {
                     ZoomRefineLocateResult zr;
                     // ★★子模型的截图**不得改写主模型的指针坐标空间**（docs §60.5）。
                     //   识图帧长边是 **960**，而模型看的观察帧是 **1024/768** ⇒
                     //   它们**不是同一个 upload 空间**；而 `mouseClick` 走的是 `liveMap`
                     //   ⇒ 识图跑完之后再用「从索引里抄来的坐标」点，会被按 960 那套缩放：
                     //   2560/960 而不是 2560/1024 ⇒ **右边缘偏 ≈170px ≈1.5 个卡槽**。
                     //   识图自己要用新映射（`RunZoomRefineLocate(..., liveMap, ...)`），
                     //   所以做法是**用完原样还回去**，而不是不写。
                     //   ⚠ 用 RAII：本 lambda 有 6 条以上提前 return，手工还原必漏。
                     struct LiveMapGuard {
                         AiCaptureMapping* live = nullptr;
                         bool* valid = nullptr;
                         AiCaptureMapping saved{};
                         bool savedValid = false;
                         ~LiveMapGuard() { *live = saved; *valid = savedValid; }
                     } liveMapGuard{ &liveMap, &liveMapValid, liveMap, liveMapValid };
                     // 识图链路分段计时（docs §27.2）：这条路是「本地执行」的大头
                     // （实测一次 ~8s），但光看总数分不出是**截屏编码**慢还是**模型往返**慢。
                     // 用**时间戳差值**打进两行日志（不跨 lambda 传变量，避免捕获顺序的坑）：
                     // 本行 → 「截屏+编码」行 = 截屏编码；再往后到「合计」行 = 模型往返。
                     ULONGLONG vLocStart = GetTickCount64();
                    // ★★ **视觉定位前也要确保"任务窗口"在前台**（2026-09-29 的真机死循环：
                    //   前台是 QQ/微信 ⇒ 截到的是 QQ ⇒ VLM 一直 NOT_FOUND、识图 API 连超时 3 次）。
                    //   ⚠ 现在这段逻辑**只有一份**：`EnsureTaskWindowForeground()`（文件内的共享 helper），
                    //     观察帧采集（`captureObservationNow`）与这里**共用**它 —— 不许再抄第二份。
                    {
                        std::wstring fgNote;
                        EnsureTaskWindowForeground(&fgNote);
                        if (!fgNote.empty()) AppendAiDebugLog(L"  [诊断] " + fgNote);
                    }
                    std::string b64;
                     int aw = 0, ah = 0;
                    // 定位长边 960 + 满分辨率（不被 aiImageScale=0.5 再压一半）：
                    // 小控件/输入框在整屏缩略后仍可辨，识图请求体也保持可控
                    if (!captureObservationNow(b64, aw, ah, 960, 1.0) || b64.empty()) {
                        zr.errorMessage = L"locateAndClick 截屏失败";
                        return zr;
                    }
                    AppendAiDebugLog(L"  [诊断] 识图-截屏+编码 "
                        + std::to_wstring(GetTickCount64() - vLocStart)
                        + L"ms（随后是模型往返）");
                    if (!liveMapValid) {
                        zr.errorMessage = L"locateAndClick 无有效截图映射";
                        return zr;
                    }
                    AppendAiDebugLog(L"  [诊断] locateAndClick freshCore targetLen="
                        + std::to_wstring(targetDesc.size()));
                    const int timeoutMs = ResolveAiLocateVisionTimeoutSec(
                        eff.aiTimeoutSec) * 1000;
                    // 视觉定位子任务：主模型（用户所选）非多模态时，
                    // 自动路由到 savedModels 中已保存的多模态模型。
                    std::wstring locateModel = effModel;
                    const std::wstring visionModel = ResolveVisionSubtaskModelName(
                        appSettings_.ai, effModel);
                    if (!visionModel.empty() && visionModel != effModel) {
                        locateModel = visionModel;
                        AppendAiDebugLog(L"  [诊断] 主模型「" + effModel
                            + L"」非多模态，识图定位改用「" + visionModel + L"」");
                    } else if (visionModel.empty() && !ModelSupportsVision(effModel)) {
                        zr.errorMessage = MissingVisionModelError(effModel);
                        return zr;
                    }
                    auto visionCore = CreateAiActionCore(
                        locateModel, appSettings_.ai.savedModels,
                        appSettings_.ai.apiUrl, appSettings_.ai.apiKey,
                        BuildAiActionVisionQuerySystemPrompt(aw, ah),
                        timeoutMs);
                    if (!visionCore) {
                        zr.errorMessage = L"无法创建识图客户端";
                        return zr;
                    }
                    ZoomRefineLocateOptions zopts;
                    zopts.maxLevels = std::clamp(refineLevels > 0 ? refineLevels : 1, 1, 2);
                    zopts.adaptiveRefineDepth = true;
                    if (zopts.maxLevels > 1) {
                        AppendAiDebugLog(L"  [诊断] locateAndClick refineLevels="
                            + std::to_wstring(zopts.maxLevels)
                            + L"（自适应：紧凑粗框可跳过二级）");
                    }
                    // UIA+视觉融合的锚点：窗口/后台窗口模式前台往往不是目标窗口，故只在非该模式取
                    std::vector<AiUiAnchor> uiAnchors;
                    if (!wmUsesTarget()) {
                        for (const auto& ctl : windowmode::ListInteractiveUiControls(
                                 GetForegroundWindow(), 60)) {
                            AiUiAnchor a;
                            a.x1 = ctl.rect.left;
                            a.y1 = ctl.rect.top;
                            a.x2 = ctl.rect.right;
                            a.y2 = ctl.rect.bottom;
                            a.name = ctl.name;
                            // 角色/能力/状态一并带上：融合判据只用 name+矩形，
                            // 但多带这几样不花钱，且让这条锚点与元素索引说的是同一件事。
                            a.role = ctl.controlType;
                            a.action = ctl.action;
                            a.state = ctl.state;
                            uiAnchors.push_back(std::move(a));
                        }
                        if (!uiAnchors.empty())
                            AppendAiDebugLog(L"  [诊断] 融合锚点：UIA 控件 "
                                + std::to_wstring(uiAnchors.size()) + L" 个（候选重合则用精确框）");
                    }
                    return ExecuteZoomRefineLocate(
                        visionCore.get(), targetDesc, b64, aw, ah, liveMap,
                        stopFlag_,
                        [this](const std::wstring& line) { AppendAiDebugLog(line); },
                        &aiHttpAbort_, zopts, uiAnchors.empty() ? nullptr : &uiAnchors,
                        &locateVerdict, &locateVerdictWhy);
                    };
                    ZoomRefineLocateResult zr;
                    if (ocrDirectHit) {
                        zr.ok = true;
                        zr.screenX = ocrDirectX;
                        zr.screenY = ocrDirectY;
                        zr.levelsUsed = 0;
                        zr.skippedRefine = true;
                        // 文字直点也是捷径：命中一条就登记，settle 判「无反应」时整表作废。
                        aiOcrDirectIndexThisBatch = true;
                        // OCR 是真读到这段文字才点的 → 直接记「可用」，别让模型看到
                        // 「可疑」又回头确认一轮（这正是用户抱怨的「总要反复确认」）。
                        zr.verdict = AiLocateVerdict::Accept;
                        locateVerdict = AiLocateVerdict::Accept;
                        locateVerdictWhy = L"本地 OCR 索引命中文字标签";
                    } else {
                        zr = runVisionLocate();
                        if (!zr.ok) {
                            // ⚠ `zr.errorMessage` 来自 `AgentCore::SendMessage`，它**自带**
                            //   `[错误] ` 前缀 ⇒ 这里再加一次就成了
                            //   `[错误] [错误] API 请求失败：服务器返回空响应。`（真机日志原文）。
                            //   前缀只该有一层：重复既难看，也会让「只看开头是不是 [错误]」
                            //   的下游判断在将来某次改动后失准。
                            std::wstring detail = zr.errorMessage.empty()
                                ? L"定位失败" : zr.errorMessage;
                            if (detail.rfind(L"[错误]", 0) == 0)
                                detail = Trim(detail.substr(4));
                            return L"[错误] " + detail
                                + L"。可换短标签再 locate，或看图换策略 / completeTask。";
                        }
                        // 识图链路分段计时（docs §27.2）：上面 lambda 已打「识图-截屏+编码
                        // Nms」——那一行与本行的差值就是**模型往返**。不跨 lambda 传变量
                        // （捕获顺序踩过坑），直接报合计。
                        AppendAiDebugLog(L"  [诊断] 识图链路到此结束（模型往返 = 本行时刻 - "
                            L"上面「截屏+编码」行；第 " + std::to_wstring(zr.levelsUsed)
                            + L" 级）");
                        if (zr.skippedRefine) {
                            AppendAiDebugLog(L"  [诊断] locateAndClick 自适应跳过二级 refine");
                        }
                        // ── OCR 文本核对（可选，最后一层独立证据）──────────────
                        // 只在「定位校验不是可用」且目标是纯短文本时才做：在定位点附近裁一块
                        // 交给文字识别，读到目标文字 → 升级为可用（省掉模型再确认一轮）；
                        // 读到别的文字 → 提示疑似未命中。用户没装识别引擎时整段静默跳过。
                        // ── 文字坐标覆盖（可选，装了识别引擎才走）──────────────────
                        // 目标是纯短文本时，**本地 OCR 读到的文字像素比 VLM 的粗框准**：
                        // 实测「自选僵尸卡牌」VLM 粗框中心 (226,169)，OCR 文字框中心 (245,106)
                        // —— 低了 63px，而那一轮因为「紧凑框」跳过了二级精炼，直接射失。
                        // 所以识图之后一律用 OCR 覆核一遍（不再只在「不是可用」时才做）：
                        //   ① 索引里离识图点最近的同名文字（≤300px）→ 就地复核；
                        //   ② 索引没有/太远 → 直接在识图点周边 ±120px 重新识别一次；
                        // 拿到文字框中心就改用它的坐标（差 >6px 才动），并记「可用」。
                        if (zr.ok && ocrVerifyBudget > 0) {
                            std::wstring wantText;
                            if (AiLocateExtractOcrTarget(targetDesc, &wantText)) {
                                int probeX = zr.screenX, probeY = zr.screenY;
                                int probeHalfW = 120, probeHalfH = 90;
                                std::wstring srcNote;
                                const auto& ocrIdx = OcrScreenIndex();
                                if (!ocrIdx.lines.empty()
                                    && ocrIdx.hwnd == GetForegroundWindow()
                                    && static_cast<long long>(GetTickCount64()) - ocrIdx.stampMs
                                        <= kOcrScreenIndexFreshMs) {
                                    AiOcrDirectHit nearHit;
                                    if (AiOcrPickNearestText(ocrIdx.lines, wantText,
                                            zr.screenX, zr.screenY, 300,
                                            ocrIdx.capX2 - ocrIdx.capX1,
                                            ocrIdx.capY2 - ocrIdx.capY1, &nearHit)) {
                                        probeX = nearHit.screenX;
                                        probeY = nearHit.screenY;
                                        probeHalfW = (std::max)(60, nearHit.boxW / 2 + 12);
                                        probeHalfH = (std::max)(28, nearHit.boxH / 2 + 10);
                                        srcNote = L"索引最近命中";
                                    }
                                }
                                int vx = probeX, vy = probeY;
                                std::wstring vnote;
                                if (ocrProbeText(wantText, probeX, probeY,
                                        probeHalfW, probeHalfH, &vx, &vy, &vnote)) {
                                    --ocrVerifyBudget;
                                    const int dx = vx - zr.screenX;
                                    const int dy = vy - zr.screenY;
                                    // ★覆盖识图点的前提：**索引给出了最近的命中**（srcNote 非空）。
                                    //   否则探针只是在识图点周围 120×90 里随便读到的第一段同名文字，
                                    //   拿去覆盖等于用一个没有独立依据的坐标替换掉识图结果。
                                    //   实测反面例子：目标「9999卡」时 srcNote 为空，识图(1535,1146)
                                    //   被覆盖成 OCR(1595,1178)——而 OCR 索引里"9999"根本没被登记
                                    //   （同一批日志里「文字直点不可用：OCR 索引里没有「9999卡」」），
                                    //   那个坐标是探针在别处读到的另一个 9999。
                                    const bool indexBacked = !srcNote.empty();
                                    if ((std::abs(dx) > 6 || std::abs(dy) > 6) && !indexBacked) {
                                        AppendAiDebugLog(
                                            L"  [诊断] 忽略 OCR 坐标覆盖（索引无依据，"
                                            L"只采信识图点）：识图("
                                            + std::to_wstring(zr.screenX) + L","
                                            + std::to_wstring(zr.screenY) + L") vs OCR("
                                            + std::to_wstring(vx) + L"," + std::to_wstring(vy)
                                            + L")（差 " + std::to_wstring(dx) + L","
                                            + std::to_wstring(dy) + L"px）");
                                    } else if (std::abs(dx) > 6 || std::abs(dy) > 6) {
                                        AppendAiDebugLog(L"  [诊断] 文字坐标覆盖识图点「"
                                            + wantText + L"」：" + srcNote + L"，识图("
                                            + std::to_wstring(zr.screenX) + L","
                                            + std::to_wstring(zr.screenY) + L") → OCR("
                                            + std::to_wstring(vx) + L"," + std::to_wstring(vy)
                                            + L")（差 " + std::to_wstring(dx) + L","
                                            + std::to_wstring(dy) + L"px）");
                                        zr.screenX = vx;
                                        zr.screenY = vy;
                                    } else {
                                        AppendAiDebugLog(L"  [诊断] 文字核对「" + wantText
                                            + L"」位置一致（" + vnote + L"）");
                                    }
                                    zr.verdict = AiLocateVerdict::Accept;
                                    locateVerdict = AiLocateVerdict::Accept;
                                    locateVerdictWhy = vnote;
                                } else if (zr.verdict != AiLocateVerdict::Accept) {
                                    // 原本就不可用 + OCR 也没读到 → 明说疑似未命中（不改坐标）
                                    locateVerdict = AiLocateVerdict::Refine;
                                    locateVerdictWhy = L"OCR 未在该点附近读到「" + wantText
                                        + L"」（" + vnote + L"），疑似未命中";
                                    AppendAiDebugLog(L"  [诊断] OCR 文本核对「" + wantText
                                        + L"」: " + vnote);
                                }
                            }
                        }
                    }
                    {
                        std::wstring t = targetDesc;
                        const bool ordinalFirst = t.find(L"第一个") != std::wstring::npos
                            || t.find(L"第1个") != std::wstring::npos
                            || t.find(L"第一张") != std::wstring::npos
                            || t.find(L"最上") != std::wstring::npos;
                        HWND fg = GetForegroundWindow();
                        RECT wr{};
                        if (ordinalFirst && fg && GetWindowRect(fg, &wr)) {
                            const int wh = wr.bottom - wr.top;
                            if (wh > 200 && zr.screenY > wr.top + wh * 85 / 100) {
                                return L"[错误] 「第1个」点在了窗口底部（那是推荐/页脚，不是列表开头）。"
                                    L"请点内容区重复卡片网格里最上最左的那张"
                                    L"（或 observePage 后 clickRef）。";
                            }
                        }
                    }
                    // 点击前查 UIA + **窗口非客户区**：两条都走 `blockVisionLanding`。
                    // ★★这道守卫是真实事故换来的，**请勿只改一处**：
                    //   ① 实测事故：模型要关游戏内的卡牌面板，识图/UIA 把点定到窗口右上角的
                    //      系统「关闭」按钮上，一击把整个游戏窗口关掉、进度丢失。
                    //      这类按钮名字（「关闭」）和应用内按钮字面完全相同，靠名字/靠灰化
                    //      都拦不住，只能靠「它属于窗口非客户区」这个**结构事实**。
                    //   ② 第二轮事故：同一个描述换成**错点自纠补点**又中了一次 ——
                    //      因为补点路径没走这道守卫。
                    //   ⚠ 那条「错点自纠」链路**批 A 已整体删除**（连判据带记账一起撤掉，
                    //     见 docs §45），所以「与补点共用同一份实现」的说法已过期：现在
                    //     `blockVisionLanding` 只有**这一个**调用者。本函数本身属
                    //     **破坏性保护族**，用户裁定推迟评估（`rollback_audit.ps1` 的
                    //     `keep: deferred guards`）—— 批 D **代码不动、只更正这句注释**（D7②）。
                    windowmode::UiElementState uiState;
                    if (const std::wstring blk =
                            blockVisionLanding(zr.screenX, zr.screenY, &uiState);
                        !blk.empty()) {
                        return blk;
                    }
                    if (uiState.probed && !uiState.enabled) {
                        AppendAiDebugLog(L"  [诊断] UIA：目标不可用（禁用），已拦截点击 name="
                            + uiState.name);
                        std::wstring why = L"[错误] 目标「" + targetDesc + L"」当前是灰色不可用状态";
                        if (!uiState.name.empty()) why += L"（控件名：" + uiState.name + L"）";
                        why += L"，点击已被拦截。灰掉=前置条件没满足，不是位置点错，"
                               L"重复点/换描述都没用。请回头检查本步之前的输入是否合法或有必填项没填："
                               L"例如文件名框里不能出现 \\ / : * ? \" < > |（要改目录得用目录选择器，"
                               L"不是把路径塞进文件名）、必选项没选、内容为空。"
                               L"先修正输入，再重新提交。";
                        if (const std::wstring memo = uiState.name; !memo.empty())
                            AppendAiTaskMemoLine(L"blocked: disabled " + memo);
                        return why;
                    }
                    // 点击前遮挡校验：定位点必须真的落在前台窗口（或其弹窗）上。
                    // 没有这道闸时，只要窗口中途被别的东西盖住/抢了前台，就会照着旧
                    // 截图把点击打到别的程序上——这是最典型的「点了但没反应/点错东西」来源。
                    // 窗口/后台窗口模式下目标窗口不一定是前台，故只在非该模式启用。
                    //
                    // ★★但拒绝之前必须先**尝试自愈**（实测代价：一次识图 6~12s 白烧）。
                    // 场景：本软件自己的「调试信息输出窗口」/浮窗抢了前台（把游戏挤到后面），
                    // 或者游戏只是丢焦。这时定位点仍然**确实属于那个目标程序**，
                    // 正确处置是「把它激活回前台再点」，而不是回一句「请先 activateWindow」
                    // ——模型照做要再花 2~3 轮（实测就是这么绕的）。
                    // 只对**该点所属的那个顶层窗口**做重激活：绝不激活别的程序（那是打错目标）。
                    if (!wmUsesTarget()
                        && !windowmode::IsScreenPointOnForegroundWindow(zr.screenX, zr.screenY)) {
                        HWND fgNow = GetForegroundWindow();
                        wchar_t nowTitle[256]{};
                        if (fgNow) GetWindowTextW(fgNow, nowTitle, 256);
                        // 该点属于谁？（GA_ROOT 拿到顶层窗口；进程名即我们要激活的目标）
                        POINT probePt{ zr.screenX, zr.screenY };
                        HWND ownerWnd = WindowFromPoint(probePt);
                        if (ownerWnd) ownerWnd = GetAncestor(ownerWnd, GA_ROOT);
                        std::wstring ownerProc;
                        if (ownerWnd) {
                            DWORD pid = 0;
                            GetWindowThreadProcessId(ownerWnd, &pid);
                            ownerProc = ProcessImageNameByPid(pid);
                        }
                        bool recovered = false;
                        if (!ownerProc.empty()) {
                            AppendAiDebugLog(L"  [诊断] 遮挡校验未过（前台是「"
                                + std::wstring(nowTitle) + L"」）→ 先尝试把点所属的「"
                                + ownerProc + L"」激活回前台再点");
                            const std::wstring act = activateByProcessFn(ownerProc);
                            if (act.rfind(L"[错误]", 0) != 0) {
                                Sleep(120);
                                recovered = windowmode::IsScreenPointOnForegroundWindow(
                                    zr.screenX, zr.screenY);
                            }
                            AppendAiDebugLog(recovered
                                ? L"  [诊断] 重激活成功，遮挡校验通过，继续点击"
                                : L"  [诊断] 重激活后遮挡校验仍不过，按原逻辑拦截");
                        }
                        if (!recovered) {
                            AppendAiDebugLog(
                                L"  [诊断] 遮挡校验失败：定位点不属于前台窗口，已拦截点击");
                            return L"[错误] 定位到了 ("
                                + std::to_wstring(zr.screenX) + L"," + std::to_wstring(zr.screenY)
                                + L")，但该点当前不属于前台窗口（当前前台：「" + nowTitle
                                + L"」）——窗口可能被覆盖或已切换，点击会打错目标，已拦截。"
                                  L"请 listWindows + activateWindow 确认目标窗口在前台后再操作；"
                                  L"网页请 observePage 后 clickRef。";
                        }
                    }
                    const bool isDouble = clickCount >= 2;
                    notePointerClick(zr.screenX, zr.screenY);
                    // ★引擎不写「这个目标上次在哪」这类跨帧世界状态：每次定位都真识图
                    // （宁可慢，不许拿旧坐标）。落点记账只保留「本批实际点过哪些点」。
                    int br[kClickColorGridN]{}, bg[kClickColorGridN]{}, bb[kClickColorGridN]{};
                    parkCursorAwayFromUi();
                    const bool hadBefore = SampleClickColorGrid(
                        zr.screenX, zr.screenY, br, bg, bb);
                    const std::wstring clickJson = BuildScreenClickActionsJson(
                        zr.screenX, zr.screenY, false, button, clickCount);
                    // 逻辑转化模板必须在点击前截取：点击会把按钮/菜单/页面切换到新状态，
                    // 点击后截到的模板是「后置状态」，下次运行时门闩 findImage 会找不到它，
                    // 快路径永远失效、每轮都烧 AI。先移开指针再截，模板=点击前界面=门闩要找的状态。
                    if (AiLogicConvertSessionActive()) {
                        // 藏起壳/宏调试窗再裁模板，避免裁进调试信息窗口
                        qst::desktop_tools::ScopedHideOwnUiForCapture hideForLogicTmpl(
                            UserFacingMainHwnd());
                        const std::wstring tmpl = CaptureAiLogicConvertTemplateAt(
                            zr.screenX, zr.screenY);
                        if (!tmpl.empty()) {
                            AiLogicConvertNoteLocate(targetDesc, zr.screenX, zr.screenY,
                                button, clickCount, tmpl);
                        }
                    }
                    const std::wstring execMsg = executeActionsJsonNow(clickJson);
                    // ★落点记账：视觉/缓存/OCR 三条路都汇到这里，所以在这里统一记
                    // 「刚刚真的点在了哪」，供下一帧观察图标红叉给模型验收。
                    // 走 MarkLastAiClickScreenPoint（而不是只写本地变量）是为了让
                    // 「这一击标过没有」的记账一起清掉 —— 用户对同一坐标再点一次时，
                    // 那就是新的一击，必须允许重新标注。
                    clickMarkX = zr.screenX;
                    clickMarkY = zr.screenY;
                    MarkLastAiClickScreenPoint(zr.screenX, zr.screenY);
                    // 坐标单位写清楚（见 DescribeClickPointForModel 的注释）
                    std::wstring out = L"locateAndClick 已"
                        + std::wstring(isDouble ? L"双击" : (button == L"right" ? L"右键点击" : L"点击"))
                        + DescribeClickPointForModel(zr.screenX, zr.screenY,
                            liveMapValid ? &liveMap : nullptr)
                        + L" 识图轮次=" + std::to_wstring(zr.levelsUsed);
                    // ★★「这一击到底有没有生效」原先只写在这里（docs §36）——
                    //   现在上移到**批次**那一层（`settleFact` 里的 `[结果]`），
                    //   理由见那里的注释：手算坐标的 `mouseClick` 也要拿到同一句话，
                    //   而且判据说一次就够了（说两次是噪音，说零次模型只能靠猜）。
                    if (zr.usedFindImageSnap) {
                        out += L"；找图精修"
                            + std::to_wstring(static_cast<int>(zr.findImageScore + 0.5)) + L"%";
                    }
                    // 本地校验定级：可疑时明确提示模型别盲信这一个点（不阻断点击，只提示）
                    if (zr.verdict != AiLocateVerdict::Accept) {
                        out += L"；定位校验=" + std::wstring(AiLocateVerdictName(zr.verdict));
                        if (!locateVerdictWhy.empty())
                            out += L"（" + locateVerdictWhy + L"）";
                        if (zr.verdict == AiLocateVerdict::Refine) {
                            out += L"。★该点可疑：先看截图确认再继续；"
                                   L"若没点中，换更具体的短标签或加 refineLevels=2，勿连点同坐标";
                        }
                    }
                    if (hadBefore) {
                        int ar[kClickColorGridN]{}, ag[kClickColorGridN]{}, ab[kClickColorGridN]{};
                        const bool hadAfter = SampleClickColorGrid(
                            zr.screenX, zr.screenY, ar, ag, ab);
                        bool compactRoiNear = false;
                        bool largeMotion = false;
                        // ★落点换算到 ROI 的坐标系（**位图局部**）再比 —— ROI 是位图局部坐标，
                        //   拿屏幕坐标直接比只有在「截图区域恰好 = 全屏」时才碰巧对（§39.4 同坑）。
                        const int rx = zr.screenX - lastUiRegionX1;
                        const int ry = zr.screenY - lastUiRegionY1;
                        for (const auto& r : lastUiChangeRois) {
                            const bool hits = rx >= r.x1 - 24 && rx <= r.x2 + 40
                                && ry >= r.y1 - 24 && ry <= r.y2 + 40;
                            // 「局部变化」的分界与 settle 的反应判据**共用同一把尺**
                            // （`AiRoiIsLocalMotion`，见 docs §40.1）—— 两处各写一份的话，
                            // 迟早会出现「settle 说没反应、回执说变了」这类互相拆台的话。
                            // ⚠ 也**必须**用相对画面那一版：绝对阈值会把 263×104 的
                            //   卡片高亮（占画面 0.74%）误判成「大面积运动」。
                            if (!AiRoiIsLocalMotion(r, lastUiRegionW, lastUiRegionH)) {
                                largeMotion = true;
                                continue;
                            }
                            if (hits) compactRoiNear = true;
                        }
                        const bool colorChanged = hadAfter
                            && ClickColorGridChanged(br, bg, bb, ar, ag, ab, 12, 2);
                        // ★有「点下去确实变了」的证据时，把前面那句「该点可疑」收回去 ——
                        // 否则模型会照它去再截一次图确认（实测白烧 1~2 轮）。
                        if (colorChanged || compactRoiNear) {
                            const size_t pos = out.find(L"。★该点可疑");
                            if (pos != std::wstring::npos) out.resize(pos);
                        }
                        const bool mixedPage = AiLastPageKind() == L"mixed";
                        // ⚠ 这一段的语气口径（批 C C5）：**只陈述本地观测到的事实与它的后果**，
                        //   不再用「禁止…」替模型下命令 —— 引擎既不拦也不判（批 A 已撤掉
                        //   近点重复/死点那两道闸），而「再点一下会取消」这种后果是**事实**，
                        //   照实说就够模型自己决定。
                        if (compactRoiNear) {
                            out += L"；点击处小范围已变（开关可能已切换）——"
                                L"对已切换的开关再点同一位置会取消。"
                                L"可用 observePage 看 pressed/checked；已是目标态则 completeTask";
                        } else if (colorChanged && !mixedPage && !largeMotion) {
                            out += L"；点击附近颜色已变（可能已切换；再点同一位置会取消）。"
                                L"可用 observePage 看 pressed/checked 后 completeTask";
                        } else if (largeMotion) {
                            out += L"；大范围画面在动（播放器），不能当成开关已切换。"
                                L"可用 observePage 看 pressed/checked";
                        } else {
                            // ★无任何变化证据 → 如实说明，把决定交回模型（引擎不猜、不补点）。
                            out += L"；点击附近外观接近（若开关仍是旧态则未完成）";
                        }
                    }
                    out += L"；已移开指针防 hover";
                    AppendAiTaskMemoLine(L"done: locateAndClick");
                    if (!execMsg.empty()) out += L"；" + execMsg;
                    return out;
                };
                agentHooks.onLocateAndClick = locateAndClickFn;
                // ★多目标：逐个定位并**立即点击**，中间不插观察/验收 ——
                // 「拿僵尸卡 → 放到草坪」这类两步操作以前要两轮主模型 + 两次 settle
                //（实测每步 1.5~2.6s），现在一次工具调用做完，失败即停。
                agentHooks.onLocateMulti = [locateAndClickFn](
                    const std::vector<std::wstring>& targets,
                    const std::wstring& button, int clickCount) -> std::wstring {
                    if (!locateAndClickFn) return L"[错误] 多目标定位未初始化。";
                    std::wstring out;
                    int okCount = 0;
                    for (size_t i = 0; i < targets.size(); ++i) {
                        const std::wstring r = locateAndClickFn(targets[i], 1, button, clickCount, 0);
                        if (!out.empty()) out += L"\n";
                        out += L"[" + std::to_wstring(i + 1) + L"/"
                            + std::to_wstring(targets.size()) + L"] " + r;
                        if (r.rfind(L"[错误]", 0) == 0) {
                            out += L"\n★第 " + std::to_wstring(i + 1)
                                + L" 个目标没定位到，后面的目标**没有点**（不做猜着点）。"
                                  L"换更短/更显眼的目标描述重试整批，或先 screenshot 看清再决定。";
                            break;
                        }
                        ++okCount;
                    }
                    if (okCount == static_cast<int>(targets.size())) {
                        out += L"\n★已连续完成 " + std::to_wstring(okCount)
                            + L" 个目标（一张截图内依次定位，未逐步验收）。"
                              L"下一步要么继续同一手法批量推进，要么 completeTask。";
                        AppendAiTaskMemoLine(L"done: locateAndClick 多目标 ×"
                            + std::to_wstring(okCount));
                    }
                    return out;
                };

                agentHooks.onSwitchWindow = [&](const std::wstring& paramsJson) -> std::wstring {
                    nlohmann::json params;
                    try {
                        params = nlohmann::json::parse(ToUtf8(paramsJson));
                    } catch (...) {
                        return L"[错误] switchWindow 参数 JSON 无效";
                    }
                    if (!params.is_object()) params = nlohmann::json::object();
                    std::string action;
                    if (params.contains("action") && params["action"].is_string())
                        action = params["action"].get<std::string>();
                    for (auto& c : action) {
                        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                    }

                    auto tapKey = [&](UINT vk) {
                        SendKeyboardKey(vk, true);
                        Sleep(25);
                        SendKeyboardKey(vk, false);
                    };

                    bool wantConfirm = false;
                    if (params.contains("confirm")) {
                        const auto& cf = params["confirm"];
                        if (cf.is_boolean()) wantConfirm = cf.get<bool>();
                        else if (cf.is_number_integer()) wantConfirm = cf.get<int>() != 0;
                    }
                    // 松开 Alt 才会真正跳到选中窗口；这里是唯一的「落地」动作
                    auto finishSwitch = [&]() {
                        MarkSimulatedInput();
                        SendKeyboardKey(VK_LMENU, false);
                        UnmarkSimulatedInput();
                        altTabAltHeld = false;
                        altTabHoldRounds = 0;
                        Sleep(120);
                        forceNextObserveUpload = true;
                        AppendAiDebugLog(L"  [诊断] switchWindow：已松开 Alt，切换落地");
                    };

                    if (action == "openpreview" || action == "open") {
                        if (altTabAltHeld) releaseAltTabIfHeld();
                        // 按住 Alt，轻点 Tab，保持 Alt → 出现窗口预览
                        MarkSimulatedInput();
                        SendKeyboardKey(VK_LMENU, true);
                        altTabAltHeld = true;
                        Sleep(40);
                        tapKey(VK_TAB);
                        UnmarkSimulatedInput();
                        Sleep(180); // 等切换器动画
                        altTabHoldRounds = 1;
                        forceNextObserveUpload = true;
                        AppendAiDebugLog(L"  [诊断] switchWindow openPreview：Alt 已按住，预览应已出现");
                        return L"[EXECUTED][OBSERVE]\nswitchWindow openPreview：已按住 Alt 并点 Tab。"
                            L"看蓝框选中项与目标窗口差几格，然后一步收尾："
                            L"move(steps=差值, direction, confirm=true) —— confirm=true 会在移动后立刻"
                            L"松开 Alt 落地，不用再单独调 confirm（Alt 悬着会挡屏幕）。"
                            L"优先仍可用 activateWindow(match=标题或进程名)。";
                    }

                    if (action == "move") {
                        if (!altTabAltHeld) {
                            return L"[错误] 尚未 openPreview（Alt 未按住）。请先 switchWindow(openPreview)。";
                        }
                        int steps = 1;
                        if (params.contains("steps") && params["steps"].is_number_integer())
                            steps = params["steps"].get<int>();
                        steps = std::clamp(steps, 1, 40);
                        std::string dir = "right";
                        if (params.contains("direction") && params["direction"].is_string())
                            dir = params["direction"].get<std::string>();
                        for (auto& c : dir) {
                            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
                        }
                        UINT vk = VK_RIGHT;
                        if (dir == "left") vk = VK_LEFT;
                        else if (dir == "tab") vk = VK_TAB;
                        MarkSimulatedInput();
                        for (int i = 0; i < steps && !StopRequested(); ++i) {
                            // Alt 保持按下，仅点方向键/Tab；间隔防吞键
                            tapKey(vk);
                            Sleep(45);
                        }
                        UnmarkSimulatedInput();
                        if (StopRequested()) {
                            releaseAltTabIfHeld();
                            return L"[错误] 用户取消";
                        }
                        if (wantConfirm) {
                            finishSwitch();
                            return L"[EXECUTED][OBSERVE]\nswitchWindow move×"
                                + std::to_wstring(steps)
                                + L" 后已松开 Alt，切换落地。请看图确认是否到了目标窗口；"
                                  L"不对就用 activateWindow(match=标题或进程名) 直接唤窗。";
                        }
                        ++altTabHoldRounds;
                        forceNextObserveUpload = true;
                        AppendAiDebugLog(L"  [诊断] switchWindow move×" + std::to_wstring(steps)
                            + L" (" + FromUtf8(dir) + L")，Alt 仍按住 第"
                            + std::to_wstring(altTabHoldRounds) + L" 轮");
                        // Alt 悬太多轮：预览一直盖着屏幕，强制落地免得卡死
                        if (altTabHoldRounds >= 4) {
                            finishSwitch();
                            return L"[EXECUTED][OBSERVE]\nswitchWindow move×"
                                + std::to_wstring(steps)
                                + L"：预览已开了太多轮，宿主已强制松开 Alt 落地。"
                                  L"请看图确认当前窗口；仍不对请改用 activateWindow(match=…)，"
                                  L"别再反复开预览。";
                        }
                        return L"[EXECUTED][OBSERVE]\nswitchWindow move×" + std::to_wstring(steps)
                            + L"：蓝框已在目标窗就 confirm 松开 Alt（不松开不会切过去）；"
                              L"否则继续 move 或 cancel。下次可直接 move(confirm=true) 一步完成。";
                    }

                    if (action == "confirm" || action == "ok") {
                        if (!altTabAltHeld) {
                            return L"[错误] 没有打开的 Alt+Tab 预览可确认。";
                        }
                        finishSwitch();
                        return L"[EXECUTED][OBSERVE]\nswitchWindow confirm：已松开 Alt，已切换到选中窗口。";
                    }

                    if (action == "cancel" || action == "close") {
                        MarkSimulatedInput();
                        if (altTabAltHeld) {
                            tapKey(VK_ESCAPE);
                            Sleep(30);
                            SendKeyboardKey(VK_LMENU, false);
                            altTabAltHeld = false;
                            altTabHoldRounds = 0;
                        }
                        UnmarkSimulatedInput();
                        forceNextObserveUpload = true;
                        return L"[EXECUTED][OBSERVE]\nswitchWindow cancel：已取消切换。"
                            L"可改 activateWindow(match=…)，或确认窗口是否未打开。";
                    }

                    // ★★ 模型把「切到某个窗口/标签」也写成 `switchWindow` 时，**必须给路由**
                    //   （2026-09-30 实测：它连试 4 轮，每轮都只得到"action 须为…"，
                    //    因为本工具是 **Alt+Tab 预览**，不是"切窗"工具）。
                    //   光说参数合法值不够 —— 它要的是另一件事，得把正确的工具名给它。
                    return L"[错误] switchWindow 是 **Alt+Tab 预览**工具"
                           L"（action 只能是 openPreview|move|confirm|cancel），"
                           L"**不能**用它直接切窗口。要切窗口请用："
                           L"① `activateWindow(match=\"标题或进程名的一段\")` —— 本地一步到位、不烧截图；"
                           L"② 要切**浏览器标签页**：`observePage` 看树上标签页条目后 "
                           L"`clickRef(eN)`，或直接对目标网址 `openWebpage(targetPath=…)`。"
                           L"\n（只有 activateWindow 连续失败时，才用 "
                           L"switchWindow(action=openPreview, force=true) 兜底。）";
                };

                // aiMaxSteps=-1：步骤与 Agent 轮次均不人为封死（轮次仅留安全上限防死循环）
                // aiMaxSteps>0：轮次约 2×步数，避免「搜到结果就断」
                const int agentRounds = (eff.aiMaxSteps < 0)
                    ? -1
                    : ((eff.aiMaxSteps > 0)
                        ? std::clamp(std::max(eff.aiMaxSteps * 2, 10), 4, 40)
                        : 10);

                auto handleAiActionApiResult = [&](const AiActionResult& ar) {
                    if (!wmUsesTarget()) {
                        auto& b = windowmode::ExtBridgeServer::Instance();
                        if (b.IsExtensionConnected() && !b.IsAborted()) {
                            std::string ign;
                            std::wstring e;
                            b.Request("detach", "", ign, e, 800);
                        }
                    }
                    // 用户热键终止：若 AI 已成功且有可固化轨迹，仍尝试写回再退出
                    if (StopRequested()) {
                        releaseAltTabIfHeld();
                        if (AiLogicConvertSessionActive()
                            && (AiLogicConvertPendingWriteback()
                                || AiLogicConvertShouldWriteback(ar.ok, ar.actionsAlreadyExecuted,
                                    ar.anyActionsExecuted, ar.completeReason, ar.textResult))) {
                            AiLogicConvertMarkPendingWriteback(true);
                            if (flushLogicConvertWriteback(L"热键终止"))
                                AiLogicConvertSessionEnd();
                        }
                        return;
                    }
                    if (!ar.ok) {
                        releaseAltTabIfHeld();
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：API 调用失败"
                            + (ar.errorMessage.empty() ? L"" : L" (" + ar.errorMessage + L")"));
                        return;
                    }
                    if (ar.visionQueryText) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]："
                            + AiActionRouteLabel(ar.routeKind) + L" → "
                            + Trim(ar.textResult));
                        if (!eff.aiOutputVarName.empty()) {
                            aiVars_[eff.aiOutputVarName] = ar.textResult;
                        }
                        return;
                    }
                    if (ar.actionsAlreadyExecuted) {
                        releaseAltTabIfHeld();
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：Agent 闭环结束 → "
                            + Trim(ar.textResult));
                        // aiActionExecute 的输出变量：与 AiTextAnalysis / AiImageAnalysis 对齐。
                        //
                        // 为什么必须有：宏里给「AI动作执行」配了输出变量却取不到值，等于白配；
                        // 更要紧的是 **AI 脚本助手的 runDesktopTask 靠它把 AI 的最终结论带回
                        // 聊天窗口**（引擎写进 aiVars_，助手侧用 EngineGetMacroVariable 读回来）。
                        // 没有这一行，助手就只能说「跑完了」而说不出跑成了什么 ——
                        // 那正是「模型自称完成」这一类假成功的温床。
                        if (!eff.aiOutputVarName.empty()) {
                            aiVars_[eff.aiOutputVarName] = ar.textResult;
                            AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：输出变量 "
                                + eff.aiOutputVarName + L" 已写入");
                        }
                        if (AiLogicConvertSessionActive()) {
                            if (!AiLogicConvertShouldWriteback(ar.ok, ar.actionsAlreadyExecuted,
                                    ar.anyActionsExecuted, ar.completeReason, ar.textResult)) {
                                AppendAiDebugLog(
                                    L"逻辑转化：任务未成功或无可固化轨迹，跳过写回");
                                AiLogicConvertSessionEnd();
                            } else {
                                AiLogicConvertMarkPendingWriteback(true);
                                if (flushLogicConvertWriteback(L"AI段结束")) {
                                    AiLogicConvertSessionEnd();
                                } else {
                                    // 路径暂缺：保留会话，等本 AI 步收尾 / 脚本结束再刷
                                    AppendAiDebugLog(
                                        L"逻辑转化：写回推迟到脚本结束或热键终止时重试");
                                }
                            }
                        }
                        return;
                    }
                    executeActionsJsonNow(ar.textResult);
                    releaseAltTabIfHeld();
                };

                if (effModel.empty() || !appSettings_.ai.enabled) {
                    AppendAiDebugLog(L"AI动作执行：未配置模型或 AI 未启用");
                    aiCurFrame = prevFrame;
                    return;
                }

                AiMacroLogFn logFn = [this](const std::wstring& line) { AppendAiDebugLog(line); };

                // 首轮截图策略：勾选「带截图」→ 必截；未勾选则仅当 prompt 明显要看屏（点击/定位）才按需截。
                // 纯变量分析+按键等不截；运行中 locateAndClick / observe 仍可按需截屏。勿嵌套 aiActionExecute。
                auto runWithOptionalAutoCapture = [&](bool preferCapture) {
                    // 「点击 X」类目标由宿主 onLocateAndClick 自己截屏（960 长边）；
                    // 这里再截一张 1280 长边整屏 + JPEG 编码纯属白烧（约 0.2–0.4s/次）。
                    // 路由分类依赖 withImage，故必须显式覆盖路由，保证仍走 CompositeClick
                    // 快路径（本地定位点击，不经规划轮）。
                    const AiActionRouteKind preRoute =
                        ClassifyAiActionRoute(resolvedPrompt, true);
                    const bool localLocateNoCapture = preferCapture && !eff.aiWithImage
                        && agentHooks.onLocateAndClick != nullptr
                        && preRoute == AiActionRouteKind::CompositeClick;
                    if (localLocateNoCapture) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel
                            + L"]：本地定位点击自带截屏 → 跳过首帧整屏截图（省一次截图+JPEG 编码）");
                    }
                    const bool wantCapture =
                        !localLocateNoCapture && (preferCapture || eff.aiWithImage);
                    int capX1 = 0, capY1 = 0, capX2 = 0, capY2 = 0;
                    std::string screenshotB64;
                    int apiW = 0, apiH = 0;
                    AiCaptureMapping capMap{};
                    bool haveImage = false;

                    if (wantCapture && resolveAiRegion(capX1, capY1, capX2, capY2)) {
                        // 首轮截图同样隐藏本软件窗口（主界面+调试窗），避免日志滚动进图
                        qst::desktop_tools::ScopedHideOwnUiForCapture hideOwnUi(
                            UserFacingMainHwnd());
                        HBITMAP screenBmp = nullptr;
                        if (wmUsesTarget()) {
                            screenBmp = wmExecPtr->CaptureScreenRegionFromWindow(
                                capX1, capY1, capX2, capY2,
                                lockedScreen_, lockedVirtX_, lockedVirtY_);
                        } else {
                            screenBmp = CaptureAiRegionComposed(capX1, capY1, capX2, capY2);
                        }
                        int sw = 0, sh = 0;
                        if (screenBmp) {
                            BITMAP bm{};
                            if (GetObject(screenBmp, sizeof(bm), &bm)) {
                                sw = bm.bmWidth;
                                sh = bm.bmHeight;
                            }
                        }
                        if (screenBmp && sw > 0 && sh > 0) {
                            if (!eff.aiWithImage) {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel
                                    + L"]：未勾选带截图，但任务需看屏，已按需截取观察区("
                                    + std::to_wstring(sw) + L"×" + std::to_wstring(sh)
                                    + L")");
                            } else {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：截屏完成("
                                    + std::to_wstring(sw) + L"×" + std::to_wstring(sh)
                                    + L")，发送中…");
                            }
                            const double scale = std::clamp(
                                eff.aiImageScale > 0.0 ? eff.aiImageScale : 0.5, 0.1, 1.0);
                            // 需看屏的首轮（本地定位点击）用更高长边，避免整屏缩略后小控件不可辨
                            const int firstLongEdge = preferCapture ? 1280 : 1024;
                            std::wstring imeStatus;
                            if (!wmUsesTarget()) {
                                imeStatus = QueryForegroundImeStatusText();
                                if (imeStatus.find(L"[输入法] 英文") != std::wstring::npos)
                                    imeStatus.clear();
                            }
                            const AiImageEncodeResult enc = EncodeBitmapForAiAnalysis(
                                screenBmp, scale, firstLongEdge,
                                imeStatus.empty() ? nullptr : &imeStatus);
                            CommitSavedImage(screenBmp, kAiObsImageVarName, imageVarRunId, imageVars_);
                            DeleteBitmapHandle(screenBmp);
                            if (!enc.base64.empty()) {
                                AppendAiDebugLog(L"  图片 " + std::to_wstring(enc.srcWidth) + L"×"
                                    + std::to_wstring(enc.srcHeight) + L" → 上传 "
                                    + std::to_wstring(enc.outWidth) + L"×" + std::to_wstring(enc.outHeight)
                                    + L"（" + std::to_wstring(enc.base64.size()) + L" 字节 base64）");
                                screenshotB64 = enc.base64;
                                apiW = enc.outWidth;
                                apiH = enc.outHeight;
                                capMap.capX1 = capX1;
                                capMap.capY1 = capY1;
                                capMap.capX2 = capX2;
                                capMap.capY2 = capY2;
                                capMap.srcWidth = enc.srcWidth;
                                capMap.srcHeight = enc.srcHeight;
                                capMap.apiWidth = apiW;
                                capMap.apiHeight = apiH;
                                liveMap = capMap;
                                liveMapValid = capMap.apiWidth > 0 && capMap.capX2 > capMap.capX1;
                                haveImage = true;
                            } else {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：图片编码失败");
                            }
                        } else {
                            AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：截屏失败"
                                + (eff.aiWithImage ? L"" : L"（将尝试无图工具路径）"));
                            if (screenBmp) DeleteBitmapHandle(screenBmp);
                        }
                    } else if (wantCapture) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：无法定位分析区域"
                            + (eff.aiWithImage ? L"" : L"（将尝试无图工具路径）"));
                    }

                    try {
                        if (!haveImage) {
                            AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：发送 prompt（无截图）…");
                            const AiActionRouteKind route = ClassifyAiActionRoute(resolvedPrompt, false);
                            const int timeoutMs = ResolveAiActionExecuteTimeoutSec(
                                eff.aiTimeoutSec, false) * 1000;
                            AgentCore* corePtr = nullptr;
                            std::unique_ptr<AgentCore> ownedCore = PrepareAiActionExecuteCore(
                                &aiSessions, prepAction, aiLoopDepth, route, 0, 0,
                                appSettings_, timeoutMs, corePtr);
                            AgentCore* core = corePtr ? corePtr : ownedCore.get();
                            if (!core) {
                                AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：无法创建 AI 客户端");
                                return;
                            }
                            // heal 回退会话保留 memo/计划门闩，避免每轮漂移再烧 updateTaskMemo
                            if (AiActionExecuteNestDepth() <= 1
                                && !AiLogicConvertSessionIsHeal())
                                ResetAiActionSessionState(imageVarRunId);
                            const size_t histBefore = core->GetHistory().size();
                            AiActionResult ar = ExecuteAiActionExecute(
                                core, resolvedPrompt, "", 0, 0, eff.aiContextMode,
                                stopFlag_, eff.aiTimeoutSec, logFn, &aiHttpAbort_, nullptr,
                                &agentHooks, agentRounds,
                                clipExtraJpeg.empty() ? nullptr : &clipExtraJpeg,
                                localLocateNoCapture ? &preRoute : nullptr);
                            if (ar.ok) {
                                propagateAiHistory(core, prepAction, histBefore,
                                    BuildAiActionExecuteTextSystemPrompt(),
                                    route == AiActionRouteKind::ToolExecute
                                        || route == AiActionRouteKind::MultiTurnTools,
                                    1024, timeoutMs);
                            }
                            handleAiActionApiResult(ar);
                            return;
                        }

                        const AiActionRouteKind route = ClassifyAiActionRoute(resolvedPrompt, true);
                        const int effectiveTimeoutSec = ResolveAiActionExecuteTimeoutSec(
                            eff.aiTimeoutSec, true);
                        const int timeoutMs = effectiveTimeoutSec * 1000;
                        // 文本主模型 + 列表识图模型：
                        // · 工具 Agent：规划仍用文本模型；locateAndClick 单独切识图子模型
                        // · Vision/Composite：整段须识图模型
                        // · 列表无识图模型：终止本步（勿仅警告后继续烧 API）
                        ScriptAction routedAction = prepAction;
                        std::wstring execModel = routedAction.aiModelName;
                        const std::wstring vm = ResolveVisionSubtaskModelName(
                            appSettings_.ai, execModel);
                        const bool primaryVision = ModelSupportsVision(execModel);
                        const bool toolAgent = (route == AiActionRouteKind::ToolExecute
                            || route == AiActionRouteKind::MultiTurnTools);
                        if (!primaryVision && vm.empty()) {
                            AiActionResult ar;
                            ar.ok = false;
                            ar.routeKind = route;
                            ar.errorMessage = MissingVisionModelError(execModel);
                            AppendAiDebugLog(L"  [错误] " + ar.errorMessage);
                            handleAiActionApiResult(ar);
                            return;
                        }
                        if (!primaryVision && !vm.empty()) {
                            if (toolAgent) {
                                AppendAiDebugLog(L"  [诊断] 主模型「" + execModel
                                    + L"」非多模态：规划轮用文本模型；"
                                      L"locateAndClick 识图将改用「" + vm + L"」");
                            } else {
                                AppendAiDebugLog(L"  [诊断] 主模型「" + execModel
                                    + L"」非多模态，带图执行改用「" + vm + L"」");
                                execModel = vm;
                            }
                        }
                        routedAction.aiModelName = execModel;
                        AgentCore* corePtr = nullptr;
                        std::unique_ptr<AgentCore> ownedCore = PrepareAiActionExecuteCore(
                            &aiSessions, routedAction, aiLoopDepth, route, apiW, apiH,
                            appSettings_, timeoutMs, corePtr);
                        AgentCore* core = corePtr ? corePtr : ownedCore.get();
                        if (!core) {
                            AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：无法创建 AI 客户端");
                            return;
                        }
                        if (AiActionExecuteNestDepth() <= 1
                            && !AiLogicConvertSessionIsHeal())
                            ResetAiActionSessionState(imageVarRunId);
                        const size_t histBefore = core->GetHistory().size();
                        AiActionResult ar = ExecuteAiActionExecute(
                            core, resolvedPrompt, screenshotB64,
                            apiW, apiH, eff.aiContextMode,
                            stopFlag_, eff.aiTimeoutSec, logFn, &aiHttpAbort_, &capMap,
                            &agentHooks, agentRounds,
                            clipExtraJpeg.empty() ? nullptr : &clipExtraJpeg);
                        if (ar.ok) {
                            std::wstring sysPrompt;
                            if (route == AiActionRouteKind::VisionQuery
                                || route == AiActionRouteKind::CompositeClick) {
                                sysPrompt = BuildAiActionVisionQuerySystemPrompt(apiW, apiH);
                            } else if (route == AiActionRouteKind::MultiTurnTools) {
                                sysPrompt = BuildAiActionHybridSystemPrompt(apiW, apiH);
                            } else {
                                sysPrompt = BuildAiActionExecuteSystemPrompt(apiW, apiH);
                            }
                            propagateAiHistory(core, routedAction, histBefore, sysPrompt,
                                route == AiActionRouteKind::ToolExecute
                                    || route == AiActionRouteKind::MultiTurnTools,
                                1024, timeoutMs);
                        }
                        handleAiActionApiResult(ar);
                    } catch (...) {
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：执行异常");
                    }
                };

                const bool needScreen = AiActionPromptLikelyNeedsScreenCapture(resolvedPrompt);
                if (!eff.aiWithImage && !needScreen) {
                    AppendAiDebugLog(L"AI动作执行 [" + effModel
                        + L"]：未勾选带截图且任务无需看屏 → 首轮无图"
                        L"（需定位时再调 locateAndClick，宿主会按需截屏）");
                }
                runWithOptionalAutoCapture(needScreen);
                releaseAltTabIfHeld();
                if (logicConvertTop) {
                    // AI 步收尾：路径已齐则写回；仍失败则保留 pending，等脚本结束/热键终止再刷
                    if (AiLogicConvertPendingWriteback()) {
                        if (flushLogicConvertWriteback(L"AI步收尾"))
                            AiLogicConvertSessionEnd();
                    } else {
                        AiLogicConvertSessionEnd();
                    }
                }
                aiCurFrame = prevFrame;
            };

            // 精密键鼠回放（对齐 LLIR/FLOW 思路）：
            // 1) 绝对时间轴  2) 关闭加速使 SendInput 贴近 Raw 录制值  3) 提高线程优先级  4) 忙等收尾
            struct InputTimelineState {
                bool enabled = false;
                PrecisionInputTimeline precision;
                void Reset(size_t waitHint = 0) {
                    precision.Reset(waitHint);
                }
            };
            InputTimelineState inputTimeline;
            inputTimeline.enabled = IsRecordingScriptPath(selfPath)
                || ScriptIsTimedInputSequence(actions);
            // 全局倍速：录制目录直接播放，或带 QPC timingUs 的录制轨迹（含另存到宏目录）。
            // 手写鼠标宏没有 timingUs，顶层仍为 1。嵌套 mousePlayback 另推动作字段。
            // 编辑器调试不走 timingUs 兜底（调试 selfPath 常是显示名），避免误套设置倍速。
            double playbackTimeScale = 1.0;
            bool applyRecordingSpeed = IsRecordingScriptPath(selfPath);
            if (!applyRecordingSpeed && !debugMode_.load(std::memory_order_relaxed)) {
                for (const auto& a : actions) {
                    if (a.timingUs > 0) {
                        applyRecordingSpeed = true;
                        break;
                    }
                }
            }
            if (applyRecordingSpeed) {
                playbackTimeScale = quickscript::RecordingPlaybackTimeScale(appSettings_);
                wmApplyTimeScale(playbackTimeScale);
            }

            // 精密轴期间：调试行先写入内存，本轮结束后再批量刷窗。
            // 这样既不影响时序，用户仍可复制完整日志（含 late 统计）供分析。
            struct DeferPlaybackDebugUiGuard {
                EngineHost* self = nullptr;
                bool armed = false;
                DeferPlaybackDebugUiGuard(EngineHost* s, bool enable, size_t actionCount) : self(s) {
                    if (!s || !enable) return;
                    s->PrepareDeferredPlaybackDebug(actionCount);
                    s->deferPlaybackDebugUi_.store(true, std::memory_order_relaxed);
                    if (s->appSettings_.playback.autoOutputKeyFunctionDebug
                        && qst::desktop_tools::MacroDebug().IsCreated()) {
                        qst::desktop_tools::MacroDebug().AppendLog(
                            L"精密时间轴回放：调试逐步日志延后刷出，且有上限（避免长录制多轮把时间轴拖变形）");
                    }
                    armed = true;
                }
                ~DeferPlaybackDebugUiGuard() {
                    if (!armed || !self) return;
                    self->DisarmDeferredPlaybackDebugUi();
                    armed = false;
                }
                void DisarmNow() {
                    if (!armed || !self) return;
                    self->DisarmDeferredPlaybackDebugUi();
                    armed = false;
                }
            } deferDbgGuard(this, inputTimeline.enabled, actions.size());

            // 绝对时间轴开启时不再额外垫 SendInput 间隔：迟到限速会与追赶打架，加重跑次抖动。
            MouseInputRouter::Instance().SetCatchUpGapUs(0);
            // SPI/优先级/绑核必须在通知 UI 结束前回恢复：否则下一轮会和析构抢加速设置。
            {
            struct CatchUpGapResetGuard {
                ~CatchUpGapResetGuard() {
                    MouseInputRouter::Instance().SetCatchUpGapUs(0);
                }
            } catchUpGapResetGuard;
            // ── 低性能模式（设置 → 宏回放设置）：只保留必要的时间轴，
            //    不做提优先级/绑核/抬全局定时器分辨率 —— 这三样都会让整机更热
            //    （抬定时器分辨率会让全系统无法进深度 C-state，长时间挂机尤其明显）。
            const bool lowPerf = LowPerformanceMode();
            MouseBallisticsGuard ballisticsGuard(inputTimeline.enabled
                || (wmExecPtr && wmExecPtr->PreferHardwareInput()));
            // 加速已关：保持 Raw 原包大小；未关才拆包防加倍（拆包会改变报告次数）。
            const bool splitLargeMoves =
                inputTimeline.enabled && !ballisticsGuard.FlatVerified();
            MouseInputRouter::Instance().SetSplitLargeMoves(splitLargeMoves);
            PlaybackProcessPriorityGuard processPriGuard(inputTimeline.enabled && !lowPerf);
            PlaybackThreadAffinityGuard affinityGuard(inputTimeline.enabled && !lowPerf);
            MultimediaTimerGuard timerPeriodGuard(inputTimeline.enabled, !lowPerf);
            struct ThreadPriorityGuard {
                HANDLE thread = nullptr;
                int prev = THREAD_PRIORITY_NORMAL;
                bool active = false;
                explicit ThreadPriorityGuard(bool enable) {
                    if (!enable) return;
                    thread = GetCurrentThread();
                    prev = GetThreadPriority(thread);
                    active = true;
                    // 勿用 TIME_CRITICAL：会饿死 UI/LL 钩子，表现为停止热键失灵、键鼠假死
                    SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
                }
                ~ThreadPriorityGuard() {
                    if (active) SetThreadPriority(thread, prev);
                }
            } threadPriGuard(inputTimeline.enabled && !lowPerf);

            // 文字直点用的 OCR 行表同理：上一轮的字坐标一律作废
            ResetOcrScreenIndex();

            bool timelineInterrupted = false;
            bool wmTargetLostLogged = false;
            auto wmAbortIfTargetLost = [this, wmExecPtr, &wmTargetLostLogged]() -> bool {
                if (!wmExecPtr || !wmExecPtr->IsActive()) return false;
                if (wmExecPtr->TargetStillAlive()) return false;
                if (!wmTargetLostLogged) {
                    wmTargetLostLogged = true;
                    // ★ 把**"跑了多久 + 最后派发的动作"**一起报出来（2026-09-27 真机）：
                    //   原先只有句柄/退出码，分不清「绑定后几十毫秒就没」与「跑了几秒某步把它搞崩」，
                    //   而这两者指向完全不同的原因（注入/绑定 vs 某个具体动作）。
                    //   ⚠ `exit=0x00000000` 是 `STILL_ACTIVE` 之外里最容易被读反的一个：
                    //     **0 表示进程自己正常退出了**（不是崩溃）；崩溃通常是 0xC0000005 之类。
                    std::wstring lost = std::wstring(L"[窗口/后台窗口模式] 目标窗口已消失（进程退出/闪退），停止脚本 ")
                        + wmExecPtr->TargetAliveDebug();
                    if (playbackRunStartTick_ != 0) {
                        lost += L" 本轮已跑="
                            + std::to_wstring(GetTickCount64() - playbackRunStartTick_) + L"ms";
                    }
                    lost += L" 最后动作=";
                    lost += playbackLastActionText_.empty()
                        ? std::wstring(L"(未记录；调试窗未开)") : playbackLastActionText_;
                    windowmode::WindowModeLog(lost);
                    AppendDebugLog(L"窗口/后台窗口模式：目标已闪退或关闭，已停止（不会对失效窗口继续记步）");
                }
                stopFlag_.store(true, std::memory_order_relaxed);
                return true;
            };
            auto waitAbsoluteTimeline = [this, &inputTimeline, &timelineInterrupted, &wmAbortIfTargetLost, &playbackTimeScale](
                double waitSec, uint64_t timingUs = 0) {
                waitSec = quickscript::ScalePlaybackTimeSeconds(waitSec, playbackTimeScale);
                timingUs = quickscript::ScalePlaybackTimeUs(timingUs, playbackTimeScale);
                if (timingUs == 0 && waitSec <= 0.0) {
                    MouseInputRouter::Instance().NoteWaitLatenessUs(0);
                    return true;
                }
                const auto cancelled = [this, &wmAbortIfTargetLost] {
                    return StopRequested() || BreakoutTriggered() || wmAbortIfTargetLost();
                };
                // 绝对轴：与录制 QPC 戳对齐。间隙等待会叠 SendInput 开销，整体偏慢且更抖。
                const bool ok = (timingUs > 0)
                    ? inputTimeline.precision.WaitDeltaUs(timingUs, cancelled)
                    : inputTimeline.precision.WaitDeltaSeconds(waitSec, cancelled);
                MouseInputRouter::Instance().NoteWaitLatenessUs(
                    inputTimeline.precision.LastLatenessUs());
                if (!ok) timelineInterrupted = true;
                return ok;
            };

            int scheduledYieldDepth = 0;
            bool scheduledYieldLocalStop = false;
            int imageWatchDepth = 0;
            std::vector<std::chrono::steady_clock::time_point> watchPollAt;
            const std::vector<ScriptAction>* watchPollBound = nullptr;
            auto matchScriptImageOnce = [&](const ScriptAction& src) -> ImageMatchResult {
                ScriptAction findAct = src;
                findAct.imagePath = resolveTemplatePath(src.imageUseVar, src.imagePath);
                if (findAct.imagePath.empty()) return {};
                const TemplateScale findTmplScale = currentTmplScale();
                if (wmUsesTarget()) {
                    ImageMatchOutput output = wmExecPtr->FindImageClient(
                        findAct, lockedScreen_, lockedVirtX_, lockedVirtY_);
                    if (output.matches.empty()) return {};
                    return output.matches.front();
                }
                if (src.windowRelative) return {};
                int x1 = src.searchX1, y1 = src.searchY1, x2 = src.searchX2, y2 = src.searchY2;
                if (src.searchFullScreen) {
                    int sx = 0, sy = 0, sw = 0, sh = 0;
                    GetVirtualScreenRect(sx, sy, sw, sh);
                    x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                } else {
                    int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
                    GetVirtualScreenRect(vsX, vsY, vsW, vsH);
                    if ((x2 <= x1 || y2 <= y1)
                        || (x1 <= vsX + 2 && y1 <= vsY + 2
                            && x2 >= vsX + vsW - 2 && y2 >= vsY + vsH - 2)) {
                        x1 = vsX; y1 = vsY; x2 = vsX + vsW; y2 = vsY + vsH;
                    }
                }
                const PreparedFindImageMatch prep = PrepareFindImageMatch(findAct, findTmplScale);
                if (!prep.bitmap) return {};
                ImageMatchOptions opt = prep.options;
                RestrictFindImageToSingleAnchor(opt);
                opt.maxOverlap = 0.5;
                ImageMatchOutput output;
                if (lockedScreen_) {
                    output = FindTemplateInFrozenScreenMulti(
                        lockedScreen_, lockedVirtX_, lockedVirtY_, x1, y1, x2, y2, prep.bitmap, opt);
                } else {
                    output = FindTemplateOnScreenMulti(x1, y1, x2, y2, prep.bitmap, opt);
                }
                if (prep.bitmap) DeleteBitmapHandle(prep.bitmap);
                if (output.matches.empty()) return {};
                return output.matches.front();
            };
            auto matchScriptImageAll = [&](const ScriptAction& src, int maxMatches) -> ImageMatchOutput {
                ScriptAction findAct = src;
                findAct.imagePath = resolveTemplatePath(src.imageUseVar, src.imagePath);
                ImageMatchOutput empty{};
                if (findAct.imagePath.empty()) return empty;
                const TemplateScale findTmplScale = currentTmplScale();
                const int keep = std::clamp(maxMatches, 1, kMultiMatchMaxHits);
                if (wmUsesTarget()) {
                    ImageMatchOutput output = wmExecPtr->FindImageClient(
                        findAct, lockedScreen_, lockedVirtX_, lockedVirtY_);
                    if (static_cast<int>(output.matches.size()) > keep) {
                        output.matches.resize(static_cast<size_t>(keep));
                    }
                    return output;
                }
                if (src.windowRelative) return empty;
                int x1 = src.searchX1, y1 = src.searchY1, x2 = src.searchX2, y2 = src.searchY2;
                if (src.searchFullScreen) {
                    int sx = 0, sy = 0, sw = 0, sh = 0;
                    GetVirtualScreenRect(sx, sy, sw, sh);
                    x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                } else {
                    int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
                    GetVirtualScreenRect(vsX, vsY, vsW, vsH);
                    if ((x2 <= x1 || y2 <= y1)
                        || (x1 <= vsX + 2 && y1 <= vsY + 2
                            && x2 >= vsX + vsW - 2 && y2 >= vsY + vsH - 2)) {
                        x1 = vsX; y1 = vsY; x2 = vsX + vsW; y2 = vsY + vsH;
                    }
                }
                const PreparedFindImageMatch prep = PrepareFindImageMatch(findAct, findTmplScale);
                if (!prep.bitmap) return empty;
                ImageMatchOptions opt = prep.options;
                opt.maxMatches = keep;
                opt.maxOverlap = 0.5;
                ImageMatchOutput output;
                if (lockedScreen_) {
                    output = FindTemplateInFrozenScreenMulti(
                        lockedScreen_, lockedVirtX_, lockedVirtY_, x1, y1, x2, y2, prep.bitmap, opt);
                } else {
                    output = FindTemplateOnScreenMulti(x1, y1, x2, y2, prep.bitmap, opt);
                }
                if (prep.bitmap) DeleteBitmapHandle(prep.bitmap);
                if (static_cast<int>(output.matches.size()) > keep) {
                    output.matches.resize(static_cast<size_t>(keep));
                }
                return output;
            };
            auto ensureWatchPollClock = [&]() {
                if (watchPollBound != activeActions) {
                    watchPollBound = activeActions;
                    const size_t n = activeActions ? activeActions->size() : 0;
                    // 时间监视第一次立即搜一次，之后按间隔轮询
                    const auto dueNow = std::chrono::steady_clock::now() - std::chrono::hours(24);
                    watchPollAt.assign(n, dueNow);
                }
            };
            auto watchPollIntervalSec = [](const ScriptAction& w) -> double {
                double s = w.watchPollSeconds;
                if (!(s > 0.0)) s = 1.0;
                // 低性能模式：轮询下限 50ms → 200ms（占用与轮询频率成正比；
                // 监视是内部轮询，不像 loop 的 duration 属于用户语义，抬高更安全）
                const double floorSec = LowPerformanceMode() ? 0.2 : 0.05;
                if (s < floorSec) s = floorSec;
                if (s > 3600.0) s = 3600.0;
                return s;
            };
            auto fireImageWatches = [&](bool timedOnly) -> bool {
                if (imageWatchDepth > 0 || !activeActions) return false;
                ensureWatchPollClock();
                const auto now = std::chrono::steady_clock::now();
                int fired = -1;
                size_t seen = 0;
                for (size_t i = 0; i < activeActions->size(); ++i) {
                    const auto& w = (*activeActions)[i];
                    if (w.type != ActionType::WatchImage || w.indent != 0) continue;
                    if (++seen > 8) break;
                    const bool timeMode = w.watchMode != 0;
                    if (timedOnly != timeMode) continue;
                    if (timeMode) {
                        if (i >= watchPollAt.size()) continue;
                        const double interval = watchPollIntervalSec(w);
                        const double elapsed = std::chrono::duration<double>(now - watchPollAt[i]).count();
                        if (elapsed < interval) continue;
                        watchPollAt[i] = now;
                    }
                    const ImageMatchResult raw = matchScriptImageOnce(w);
                    const ImageMatchResult match = NormalizeMatchVarResult(
                        raw, w.matchThreshold, w.perfectMatch);
                    if (match.found) {
                        fired = static_cast<int>(i);
                        break;
                    }
                }
                if (fired < 0) return false;
                ++imageWatchDepth;
                const size_t bodyEnd = containerBodyEnd(static_cast<size_t>(fired));
                if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                    AppendDebugLog(L"找图监视命中：第 "
                        + std::to_wstring((*activeActions)[static_cast<size_t>(fired)].originalNo > 0
                            ? (*activeActions)[static_cast<size_t>(fired)].originalNo
                            : fired + 1)
                        + L" 步");
                }
                const RunRangeResult r = runRange(static_cast<size_t>(fired) + 1, bodyEnd);
                --imageWatchDepth;
                if (r == RunRangeResult::BreakLoop) pendingBreakLoop = true;
                if (pendingGoto || pendingBreakLoop) return true;
                if ((*activeActions)[static_cast<size_t>(fired)].resumeAfterWatch) return true;
                pendingGoto = bodyEnd;
                return true;
            };
            auto sleepWithTimeWatches = [&](double seconds) {
                if (seconds <= 0.0 || StopRequested()) return;
                using clock = std::chrono::steady_clock;
                auto end = clock::now() + std::chrono::duration_cast<clock::duration>(
                    std::chrono::duration<double>(seconds));
                while (!StopRequested() && !BreakoutTriggered()) {
                    if (fireImageWatches(true)) {
                        if (pendingGoto || pendingBreakLoop || StopRequested()) return;
                    }
                    const auto now = clock::now();
                    if (now >= end) break;
                    const double rem = std::chrono::duration<double>(end - now).count();
                    double slice = rem;
                    ensureWatchPollClock();
                    if (activeActions) {
                        bool anyTime = false;
                        double soon = rem;
                        size_t seen = 0;
                        const auto tnow = clock::now();
                        for (size_t i = 0; i < activeActions->size(); ++i) {
                            const auto& w = (*activeActions)[i];
                            if (w.type != ActionType::WatchImage || w.indent != 0) continue;
                            if (++seen > 8) break;
                            if (w.watchMode == 0) continue;
                            anyTime = true;
                            if (i >= watchPollAt.size()) continue;
                            const double interval = watchPollIntervalSec(w);
                            const double elapsed = std::chrono::duration<double>(tnow - watchPollAt[i]).count();
                            const double due = (std::max)(0.0, interval - elapsed);
                            if (due < soon) soon = due;
                        }
                        if (anyTime) slice = (std::min)(rem, (std::max)(0.02, soon));
                    }
                    SleepInterruptible(slice);
                }
            };
            auto sleepRepeatInterval = [&](const ScriptAction& act, int repeatIndex) -> bool {
                if (!ShouldWaitAfterRepeat(act, repeatIndex)
                    || StopRequested() || scheduledYieldLocalStop) {
                    return pendingGoto || pendingBreakLoop;
                }
                sleepWithTimeWatches(quickscript::ScalePlaybackTimeSeconds(
                    act.duration + RandomDelay(act.randomDuration), playbackTimeScale));
                return pendingGoto || pendingBreakLoop || StopRequested();
            };
            std::function<bool(const ScriptAction&, ImageMatchResult&, int&, int&)> findLocateAnchor;
            findLocateAnchor = [this, &executeOne, &resolveTemplatePath, &makeVarCtx,
                &pendingGoto, &pendingBreakLoop](
                const ScriptAction& src, ImageMatchResult& matchOut, int& tplW, int& tplH) -> bool {
                matchVars_.erase(L"_qstLocateAnchor");
                ScriptAction findAct = src;
                findAct.type = ActionType::FindImage;
                findAct.findImageFollowUp = 2;
                findAct.matchVarName = L"_qstLocateAnchor";
                findAct.offsetX = 0;
                findAct.offsetY = 0;
                findAct.nOffsetX = 0;
                findAct.nOffsetY = 0;
                findAct.duration = 0;
                findAct.randomDuration = 0;
                findAct.timingUs = 0;
                findAct.clickCount = 1;
                findAct.imageLocate = false;
                const std::wstring tplPath = ResolveImagePath(
                    resolveTemplatePath(src.imageUseVar, src.imagePath));
                tplW = 0;
                tplH = 0;
                HBITMAP tplBmp = LoadBitmapFromFile(tplPath);
                if (tplBmp) {
                    BITMAP bm{};
                    GetObjectW(tplBmp, sizeof(bm), &bm);
                    tplW = bm.bmWidth;
                    tplH = bm.bmHeight;
                    DeleteBitmapHandle(tplBmp);
                }
                findAct.imagePath = tplPath.empty() ? src.imagePath : tplPath;
                findAct.findTimeExpr = L"0";
                const double findTimeSec = ResolveFindImageTimeSec(src.findTimeExpr, makeVarCtx());
                const bool loopUntilFound = findTimeSec < 0.0;
                const auto findStart = std::chrono::steady_clock::now();
                for (;;) {
                    if (StopRequested() || pendingGoto || pendingBreakLoop) return false;
                    executeOne(findAct);
                    if (StopRequested() || pendingGoto || pendingBreakLoop) return false;
                    auto it = matchVars_.find(L"_qstLocateAnchor");
                    if (it != matchVars_.end() && it->second.found) {
                        matchOut = it->second;
                        if (tplW <= 0)
                            tplW = (std::max)(1, matchOut.bottomRightX - matchOut.topLeftX);
                        if (tplH <= 0)
                            tplH = (std::max)(1, matchOut.bottomRightY - matchOut.topLeftY);
                        return true;
                    }
                    if (!loopUntilFound) {
                        if (findTimeSec <= 0.0) return false;
                        const double elapsed = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - findStart).count();
                        if (elapsed >= findTimeSec) return false;
                    }
                    SleepInterruptible(0.05);
                }
            };

            auto applyWorkerBreakout = [this](double seconds, bool wmActive) {
                const double t = (!wmActive && seconds > 0.0) ? seconds : 0.0;
                workerBreakoutTime_ = t;
                if (ForegroundInputRouter::Instance().IsHidActive()) {
                    synthetic_input::SetBreakoutTracking(t > 0.0);
                }
            };
            auto publishRunningWm = [this](const windowmode::WindowModeScriptConfig& cfg) {
                std::lock_guard<std::mutex> lock(extScriptStateMu_);
                runningWindowMode_ = cfg;
            };
            auto beginWmCfg = [this, &wmExec](
                const windowmode::WindowModeScriptConfig& cfg,
                const std::wstring& nestedPath, std::wstring& err) -> bool {
                windowmode::BeginRunOptions opts;
                opts.launchTarget = true;
                opts.cancelFlag = &stopFlag_;
                const auto slash = nestedPath.find_last_of(L"\\/");
                opts.launchSearchDir = slash == std::wstring::npos ? L"" : nestedPath.substr(0, slash);
                return wmExec.BeginRun(cfg, err, opts);
            };
            auto captureFgIntoCfg = [this](windowmode::WindowModeScriptConfig& cfg) -> bool {
                HWND fg = GetForegroundWindow();
                DWORD pid = 0;
                if (fg) GetWindowThreadProcessId(fg, &pid);
                if (!fg || !IsWindow(fg) || pid == 0 || pid == GetCurrentProcessId()) return false;
                RECT rc{};
                if (!GetWindowRect(fg, &rc)) return false;
                const auto info = GetWindowInfoFromPoint(
                    (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
                if (info.windowClassName.empty() && info.windowTitle.empty()
                    && info.processPath.empty()) {
                    return false;
                }
                ApplyWindowInfoToConfig(cfg, info);
                return true;
            };
            auto restoreModeFrame = [&](const NestedModeFrame& frame) {
                if (wmExec.IsActive()) wmExec.EndRun();
                activeWmCfg = frame.cfg;
                if (frame.wasActive && frame.cfg.enabled) {
                    std::wstring err;
                    const std::wstring restorePath =
                        frame.launchPath.empty() ? selfPath : frame.launchPath;
                    if (!beginWmCfg(frame.cfg, restorePath, err)) {
                        AppendDebugLog(L"嵌套运行结束：恢复主宏窗口/后台窗口模式失败：" + err);
                        activeWmCfg = windowmode::DefaultWindowModeConfig();
                        applyWorkerBreakout(frame.breakoutTime, false);
                        publishRunningWm(activeWmCfg);
                        wmExec.SetCoordMeta(activeCoordMeta);
                        return;
                    }
                    wmExec.SetCoordMeta(activeCoordMeta);
                }
                applyWorkerBreakout(frame.breakoutTime, wmExec.IsActive());
                publishRunningWm(activeWmCfg);
            };
            auto pushNestedUseMode = [&](const ScriptAction& a, const ScriptFileData& nestedData,
                const std::wstring& nestedPath) -> bool {
                const int mode = NormalizeNestedUseMode(a.useMode);
                if (mode == kNestedUseModeInherit) return true;
                NestedModeFrame frame{
                    activeWmCfg, wmExec.IsActive(), workerBreakoutTime_, runningScriptPath};
                if (mode == kNestedUseModeDefault) {
                    if (wmExec.IsActive()) wmExec.EndRun();
                    activeWmCfg = windowmode::DefaultWindowModeConfig();
                    applyWorkerBreakout(a.breakoutTimeSeconds, false);
                    publishRunningWm(activeWmCfg);
                    nestedModeStack.push_back(frame);
                    return true;
                }

                windowmode::WindowModeScriptConfig cfg = a.nestedWindowMode;
                cfg.enabled = true;
                cfg.executionKind = (mode == kNestedUseModeBackground)
                    ? windowmode::WindowModeExecutionKind::BackgroundWindow
                    : windowmode::WindowModeExecutionKind::HiddenDesktop;
                MergeNestedWindowRelative(cfg, nestedData);
                cfg.selectMethod = windowmode::NormalizeSelectMethod(cfg.selectMethod);
                using SM = windowmode::WindowSelectMethod;
                if (cfg.selectMethod == SM::SelectOnStartup) {
                    if (!captureFgIntoCfg(cfg)) {
                        AppendDebugLog(L"嵌套运行失败：启动时使用当前所在窗口，但未能获取前台窗口");
                        return false;
                    }
                    cfg.autoLaunchTarget = false;
                } else {
                    cfg.autoLaunchTarget = windowmode::ShouldAutoLaunchTarget(cfg);
                    if (cfg.selectMethod == SM::UseEditorWindowClass
                        && !NestedWindowModeHasIdentity(cfg)) {
                        AppendDebugLog(L"嵌套运行失败：请先配置目标窗口类或改用其他选择窗口方式");
                        return false;
                    }
                    if (cfg.selectMethod == SM::NoSelect && cfg.targetExePath.empty()) {
                        AppendDebugLog(L"嵌套运行失败：不选择窗口时需要目标程序路径");
                        return false;
                    }
                }
                if (wmExec.IsActive()) wmExec.EndRun();
                // ★★ 必须**在 BeginRun 之前**就把运行态配置发布出去（2026-10-03 修）。
                //   宏调试窗的 sink 用 `runningWindowMode_.enabled` 过滤（免得扩展桥心跳灌进来），
                //   而嵌套模式的 `publishRunningWm` 原来在 BeginRun **成功之后**才调 ⇒
                //   **注入期**（绑窗/注入/失败）的所有窗口模式日志全被静默丢弃 ——
                //   而那正是唯一能定位「假焦点注入失败」的一段。现场后果：用户导出的
                //   诊断里只有 BeginRun/EndRun，`假焦点注入失败: <原因>` 一个字都看不到。
                //   失败时下面的 restoreModeFrame 会把 runningWindowMode_ 还原回 frame.cfg。
                publishRunningWm(cfg);
                std::wstring wmErr;
                if (!beginWmCfg(cfg, nestedPath, wmErr)) {
                    AppendDebugLog(L"嵌套运行失败：窗口/后台窗口模式启动失败 " + wmErr);
                    restoreModeFrame(frame);
                    return false;
                }
                activeWmCfg = cfg;
                wmExec.SetCoordMeta(activeCoordMeta);
                applyWorkerBreakout(0, true);
                publishRunningWm(activeWmCfg);
                nestedModeStack.push_back(frame);
                return true;
            };
            auto popNestedUseMode = [&]() {
                if (nestedModeStack.empty()) return;
                NestedModeFrame frame = nestedModeStack.back();
                nestedModeStack.pop_back();
                restoreModeFrame(frame);
            };
            auto runNestedLibrary = [&](const ScriptAction& a, bool isPlayback) {
                const int useMode = NormalizeNestedUseMode(a.useMode);
                bool switched = false;
                for (int i = 0; i < a.clickCount && !StopRequested() && !scheduledYieldLocalStop; ++i) {
                    std::wstring path = ResolveNestedLibraryTarget(a.targetPath, a.blockName);
                    if (path.empty() || (!isPlayback && _wcsicmp(path.c_str(), runningScriptPath.c_str()) == 0)) {
                        if (path.empty()) {
                            AppendDebugLog(isPlayback
                                ? L"运行录制回放失败：找不到目标录制（可能已拖入专业模式文件夹）"
                                : L"运行宏失败：找不到目标脚本（可能已拖入专业模式文件夹）");
                        }
                        break;
                    }
                    const ScriptFileData nestedData = LoadScriptFileData(path, false);
                    if (nestedData.actions.empty()) break;
                    if (i == 0 && useMode != kNestedUseModeInherit) {
                        if (!pushNestedUseMode(a, nestedData, path)) break;
                        switched = true;
                    } else if (switched && useMode != kNestedUseModeDefault) {
                        MergeNestedWindowRelative(activeWmCfg, nestedData);
                    }
                    CoordMeta nestedMeta = ScriptCoordMetaForExecution(nestedData.coordMeta);
                    std::vector<ScriptAction> nested =
                        PrepareScriptActionsForExecution(nestedData.actions, nestedMeta);
                    if (IsRecordingScriptPath(path) || ScriptIsTimedInputSequence(nested)
                        || nestedData.inputTimingVersion > 0) {
                        PreparePlaybackTimeline(nested, appSettings_.playback.spreadRelativeMovePackets);
                    }
                    if (!usesOcr && ScriptUsesTextRecognition(nested)) {
                        usesOcr = true;
                        workerUsesOcrVars_ = true;
                    }
                    if (usesOcr) holdOcrSession();
                    const std::vector<ScriptAction>* prevActions = activeActions;
                    const std::wstring prevPath = runningScriptPath;
                    const CoordMeta prevCoordMeta = activeCoordMeta;
                    activeCoordMeta = nestedMeta;
                    if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                    activeActions = &nested;
                    runningScriptPath = path;
                    if (inputTimeline.enabled
                        && (IsRecordingScriptPath(path)
                            || ScriptIsTimedInputSequence(nested)
                            || nestedData.inputTimingVersion > 0)) {
                        inputTimeline.Reset();
                    }
                    const double prevScale = playbackTimeScale;
                    if (isPlayback) {
                        playbackTimeScale = quickscript::PlaybackTimeScaleAlways(a.playbackSpeed);
                        // 嵌套回放也同步目标窗口时钟：否则嵌套段会「脚本变速、游戏原速」。
                        wmApplyTimeScale(playbackTimeScale);
                    }
                    runRange(0, nested.size());
                    if (isPlayback) {
                        playbackTimeScale = prevScale;
                        wmApplyTimeScale(playbackTimeScale);
                    }
                    activeActions = prevActions;
                    runningScriptPath = prevPath;
                    activeCoordMeta = prevCoordMeta;
                    if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                    if (sleepRepeatInterval(a, i)) {
                        if (switched) popNestedUseMode();
                        return;
                    }
                }
                if (switched) popNestedUseMode();
            };

            // 找图/OCR「移动/点击」成功后，后续鼠标点击应落在该点。
            // 编辑器「鼠标点击」不展示坐标，改动作类型时却常带上旧 x/y，会从找图落点飞走。
            // 远程桌面/真人鼠标会在等待间隙把系统光标拖走，必须记住落点并在每次点击前重新落到该点，
            // 不能只点 GetCursorPos「当前位置」。
            bool keepCursorAtFind = false;
            int lastFindX = 0;
            int lastFindY = 0;
            bool loggedRdpFindPin_ = false;
            auto applyFindCursor = [&](int tx, int ty, bool click, MouseButtonType btn,
                const ScriptAction& act) -> bool {
                keepCursorAtFind = true;
                lastFindX = tx;
                lastFindY = ty;
                if (!click) {
                    wmSetLivePos(tx, ty, 0, 0);
                    return false;
                }
                if (!wmUsesTarget()) activateDesktopAt(tx, ty);
                const int n = std::max(1, act.clickCount);
                for (int i = 0; i < n && !StopRequested(); ++i) {
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    if (wmUsesTarget()) {
                        wmSetLivePos(tx, ty, 0, 0);
                        wmExecPtr->PostMouseClickAtClient(tx, ty, btn, false);
                    } else {
                        SendMouseClickAtScreen(tx, ty, btn);
                    }
                    if (markSim) UnmarkSimulatedInput();
                    if (i == 0 && appSettings_.playback.autoOutputKeyFunctionDebug && !wmUsesTarget()) {
                        POINT cur{};
                        GetCursorPos(&cur);
                        HWND hit = WindowFromPoint(POINT{tx, ty});
                        HWND root = hit ? GetAncestor(hit, GA_ROOT) : nullptr;
                        wchar_t cls[160]{};
                        if (root) GetClassNameW(root, cls, 160);
                        wchar_t buf[384]{};
                        swprintf_s(buf,
                            L"找图点击 目标=(%d,%d) 光标=(%d,%d) 窗口=%s",
                            tx, ty, cur.x, cur.y, cls[0] ? cls : L"(无)");
                        AppendDebugLog(buf);
                        if (cur.x != tx || cur.y != ty) {
                            AppendDebugLog(
                                L"光标未能落到找图点（游戏锁定/裁剪光标，或远程桌面把鼠标拖走了）");
                        }
                    }
                    if (sleepRepeatInterval(act, i)) return true;
                }
                return false;
            };

            executeOne = [this, &usesOcr, &holdOcrSession, &heldKeyVk, &heldKeys, &runRange, &runningScriptPath, &activeActions, &lockedScreen_, &lockedVirtX_, &lockedVirtY_, &clearLockedScreen, &makeVarCtx, &resolveTemplatePath, &executeOne, &runAiActionExecute, &aiSessions, &aiLoopDepth, &pendingBreakLoop, wmExecPtr, &wmSetPos, &wmSetLivePos, &wmSendKey, &wmSendHeldModifiers, &wmMouseButton, &wmMouseClick, &activateDesktopAt, &wmSendShortcut, &isImeToggleShortcut, &wmUsesTarget, &wmUsesBackground, &activeCoordMeta, &currentTmplScale, execTargetW, execTargetH, &inputTimeline, &waitAbsoluteTimeline, &wmAbortIfTargetLost, &playbackTimeScale, wmApplyTimeScale, imageVarRunId, &scheduledYieldDepth, &scheduledYieldLocalStop, &fireImageWatches, &sleepWithTimeWatches, &sleepRepeatInterval, &pendingGoto, &findLocateAnchor, &matchScriptImageAll, &runNestedLibrary, &keepCursorAtFind, &lastFindX, &lastFindY, &loggedRdpFindPin_, &applyFindCursor, &reqRelDx, &reqRelDy, &reqRelPackets](const ScriptAction& a) {
                // 变速下发后延迟打印一次 DLL 侧诊断（只在首个动作后触发一次，开销可忽略）。
                if (wmExecPtr) wmExecPtr->MaybeLogTimeScaleDiag();
                if (StopRequested() || scheduledYieldLocalStop || wmAbortIfTargetLost()) return;
                if (fireImageWatches(true)) {
                    if (pendingGoto || pendingBreakLoop) return;
                }
                executedSteps_.fetch_add(1, std::memory_order_relaxed);
                // 兼容未 Normalize 的旧内存对象：瞬时类仍可能带前延迟
                if (inputTimeline.enabled && a.randomDuration <= 1e-12) {
                    const bool durationIsInterval =
                        a.type == ActionType::Wait
                        || a.type == ActionType::MouseDrag
                        || ActionUsesInterRepeatInterval(a.type);
                    if (!durationIsInterval && (a.timingUs > 0 || a.duration > 1e-9)) {
                        waitAbsoluteTimeline(a.duration, a.timingUs);
                        if (StopRequested() || wmAbortIfTargetLost()) return;
                    }
                }

                if (KeyFunctionDebugActive() && !DeferredDetailDebugDropped()
                    && a.type != ActionType::MoveMouse
                    && a.type != ActionType::MoveMouseRelative
                    && a.type != ActionType::Wait
                    && a.type != ActionType::KeyDown
                    && a.type != ActionType::KeyUp
                    && a.type != ActionType::FindImage
                    && a.type != ActionType::TextRecognition) {
                    if (keepCursorAtFind && !a.moveFromVar
                        && (a.type == ActionType::MouseClick
                            || a.type == ActionType::MouseDown
                            || a.type == ActionType::MouseUp)) {
                        wchar_t pinBuf[256]{};
                        if (wmUsesTarget()) {
                            int ccx = 0, ccy = 0;
                            if (wmExecPtr && wmExecPtr->GetCursorClientPos(ccx, ccy)) {
                                swprintf_s(pinBuf,
                                    L"（沿用找图/OCR落点，软光标=(%d,%d)，重新落到(%d,%d)）",
                                    ccx, ccy, lastFindX, lastFindY);
                            } else {
                                swprintf_s(pinBuf, L"（沿用找图/OCR落点，重新落到(%d,%d)）",
                                    lastFindX, lastFindY);
                            }
                        } else {
                            POINT cur{};
                            GetCursorPos(&cur);
                            swprintf_s(pinBuf,
                                L"（沿用找图/OCR落点，当前光标=(%d,%d)，重新落到(%d,%d)）",
                                cur.x, cur.y, lastFindX, lastFindY);
                        }
                        AppendDebugLog(FormatGenericActionDebug(a) + pinBuf);
                        if (!loggedRdpFindPin_ && GetSystemMetrics(SM_REMOTESESSION)) {
                            loggedRdpFindPin_ = true;
                            AppendDebugLog(
                                L"当前是远程桌面会话：找图后的点击会重新落到找图点；"
                                L"遥控时请勿在对端画面上移动/点击鼠标（会把系统光标拖走）");
                        }
                    } else {
                        AppendDebugLog(FormatGenericActionDebug(a));
                    }
                }
                if (a.type == ActionType::MoveMouse) {
                    keepCursorAtFind = false;
                    int x = a.x;
                    int y = a.y;
                    if (a.moveFromVar) {
                        MacroVariableContext ctx = makeVarCtx();
                        if (!TryResolveIntOperand(a.moveVarExprX, ctx, x)) x = 0;
                        if (!TryResolveIntOperand(a.moveVarExprY, ctx, y)) y = 0;
                    }
                    if (KeyFunctionDebugActive()) {
                        AppendDeferredMoveAbsDebug(a, x, y);
                    }
                    // 精密轴只对「录制回放」禁用随机抖动（录制坐标本就是精确采样，
                    // 且录制时 randomX/randomY 已置 0）；手写宏（含时间轴输入序列脚本）
                    // 的 ±随机必须生效，否则每次移动都落在同一点。
                    const bool recordingReplay = IsRecordingScriptPath(runningScriptPath);
                    const int rx = recordingReplay ? 0 : a.randomX;
                    const int ry = recordingReplay ? 0 : a.randomY;
                    wmSetPos(x, y, rx, ry);
                }
                else if (a.type == ActionType::MoveMouseRelative) {
                    keepCursorAtFind = false;
                    const bool recordingReplay = IsRecordingScriptPath(runningScriptPath);
                    const int dx = a.x + (recordingReplay ? 0 : RandomInt(a.randomX));
                    const int dy = a.y + (recordingReplay ? 0 : RandomInt(a.randomY));
                    reqRelDx += dx;
                    reqRelDy += dy;
                    ++reqRelPackets;
                    if (KeyFunctionDebugActive()) {
                        AppendDeferredMoveRelDebug(a, dx, dy);
                    }
                    if (wmUsesTarget()) {
                        const bool hw = wmExecPtr->PreferHardwareInput();
                        if (hw) MarkSimulatedInput();
                        // 相对移动的落点/卡顿排查**只看得到这条**：绝对移动在
                        // `MoveMouseClient` 里有「移动 → 客户区(…)」日志，相对移动此前
                        // **一行都没有** —— 导致无法判断它到底走了软输入还是硬件路径，
                        // 只能靠 `SendInput ok=` 间接猜。这里补上与绝对移动同级的证据。
                        if (KeyFunctionDebugActive()) {
                            wchar_t line[224]{};
                            swprintf_s(line, L"[窗口/后台窗口模式] 相对移动 → (%d,%d) 路径=%s",
                                dx, dy, wmExecPtr->RelativeMoveRouteName());
                            AppendDebugLog(line);
                        }
                        wmExecPtr->MoveMouseRelativeClient(dx, dy);
                        if (hw) UnmarkSimulatedInput();
                    } else {
                        MarkSimulatedInput();
                        SendMouseMoveRelative(dx, dy);
                        UnmarkSimulatedInput();
                    }
                }
                else if (a.type == ActionType::Wait) {
                    if (a.randomDuration > 1e-12 || !inputTimeline.enabled) {
                        const double waitSec = quickscript::ScalePlaybackTimeSeconds(
                            a.duration + RandomDelay(a.randomDuration), playbackTimeScale);
                        if (waitSec > 0.0) {
                            sleepWithTimeWatches(waitSec);
                            if (inputTimeline.enabled) inputTimeline.Reset();
                        }
                    } else if (a.timingUs > 0) {
                        waitAbsoluteTimeline(0.0, a.timingUs);
                    } else if (a.duration > 0.0) {
                        waitAbsoluteTimeline(a.duration, 0);
                    }
                    if (pendingGoto || pendingBreakLoop) return;
                    if (KeyFunctionDebugActive()) {
                        AppendDeferredWaitDebug(a,
                            inputTimeline.enabled
                                ? inputTimeline.precision.LastLatenessUs() : 0,
                            playbackTimeScale);
                    }
                }
                else if (a.type == ActionType::MouseDown) {
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    wmSendHeldModifiers(a, true);
                    if (keepCursorAtFind && !a.moveFromVar) {
                        if (wmUsesTarget()) {
                            wmSetLivePos(lastFindX, lastFindY, 0, 0);
                            wmExecPtr->PostMouseButtonAtClient(lastFindX, lastFindY, a.button, true, false);
                        } else {
                            activateDesktopAt(lastFindX, lastFindY);
                            SendMouseMoveAbsoluteScreen(lastFindX, lastFindY);
                            wmMouseButton(lastFindX, lastFindY, a.button, true);
                        }
                    } else {
                        wmMouseButton(a.x, a.y, a.button, true);
                    }
                    if (markSim) UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::MouseUp) {
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    if (keepCursorAtFind && !a.moveFromVar) {
                        if (wmUsesTarget()) {
                            wmSetLivePos(lastFindX, lastFindY, 0, 0);
                            wmExecPtr->PostMouseButtonAtClient(lastFindX, lastFindY, a.button, false, false);
                        } else {
                            activateDesktopAt(lastFindX, lastFindY);
                            SendMouseMoveAbsoluteScreen(lastFindX, lastFindY);
                            wmMouseButton(lastFindX, lastFindY, a.button, false);
                        }
                    } else {
                        wmMouseButton(a.x, a.y, a.button, false);
                    }
                    wmSendHeldModifiers(a, false);
                    if (markSim) UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::MouseClick) for (int i = 0; i < a.clickCount && !StopRequested(); ++i) {
                    int cx = a.x;
                    int cy = a.y;
                    bool clickAtFind = false;
                    if (a.moveFromVar) {
                        keepCursorAtFind = false;
                        MacroVariableContext ctx = makeVarCtx();
                        if (!TryResolveIntOperand(a.moveVarExprX, ctx, cx)) cx = 0;
                        if (!TryResolveIntOperand(a.moveVarExprY, ctx, cy)) cy = 0;
                        wmSetPos(cx, cy, a.randomX, a.randomY);
                        cx = cx + RandomInt(a.randomX);
                        cy = cy + RandomInt(a.randomY);
                    } else if (keepCursorAtFind) {
                        // 找图/OCR 落点；每次点击前重钉，避免远程桌面/真人鼠标把光标拖走。
                        cx = lastFindX;
                        cy = lastFindY;
                        clickAtFind = true;
                    } else if (a.x != 0 || a.y != 0) {
                        wmSetPos(a.x, a.y, a.randomX, a.randomY);
                        cx = a.x + RandomInt(a.randomX);
                        cy = a.y + RandomInt(a.randomY);
                    }
                    // 窗口/后台窗口模式 CDP/扩展键鼠不走本机 SendInput，勿 Mark（否则脱离检测会误判忙碌）。
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    wmSendHeldModifiers(a, true);
                    if (clickAtFind && wmUsesTarget())
                        wmExecPtr->PostMouseClickAtClient(cx, cy, a.button, false);
                    else
                        wmMouseClick(cx, cy, a.button);
                    wmSendHeldModifiers(a, false);
                    if (markSim) UnmarkSimulatedInput();
                    // duration=两次重复之间的间隔；执行 1 次时不等待，首前/末后也不插等待
                    if (sleepRepeatInterval(a, i)) return;
                }
                else if (a.type == ActionType::MouseDrag) {
                    keepCursorAtFind = false;
                    int sx = a.x + RandomInt(a.randomX);
                    int sy = a.y + RandomInt(a.randomY);
                    int ex = a.endX + RandomInt(a.randomEndX);
                    int ey = a.endY + RandomInt(a.randomEndY);
                    bool skipDrag = false;
                    if (a.imageLocate) {
                        ImageMatchResult loc{};
                        int tplW = 0, tplH = 0;
                        if (!findLocateAnchor(a, loc, tplW, tplH)) {
                            skipDrag = true;
                        } else {
                            const double nsx = a.x / static_cast<double>(tplW);
                            const double nsy = a.y / static_cast<double>(tplH);
                            const double nex = a.endX / static_cast<double>(tplW);
                            const double ney = a.endY / static_cast<double>(tplH);
                            const TemplateScale dragScale = currentTmplScale();
                            ResolveFindImageClickPoint(loc, tplW, tplH, nsx, nsy,
                                dragScale, false, sx, sy);
                            ResolveFindImageClickPoint(loc, tplW, tplH, nex, ney,
                                dragScale, false, ex, ey);
                            sx += RandomInt(a.randomX);
                            sy += RandomInt(a.randomY);
                            ex += RandomInt(a.randomEndX);
                            ey += RandomInt(a.randomEndY);
                        }
                    }
                    if (!skipDrag) {
                        const bool markSim = !wmUsesTarget();
                        auto lift = [&]() {
                            if (markSim) MarkSimulatedInput();
                            wmMouseButton(ex, ey, a.button, false);
                            wmSendHeldModifiers(a, false);
                            if (markSim) UnmarkSimulatedInput();
                        };
                        if (markSim) MarkSimulatedInput();
                        wmSendHeldModifiers(a, true);
                        wmSetLivePos(sx, sy, 0, 0);
                        wmMouseButton(sx, sy, a.button, true);
                        if (markSim) UnmarkSimulatedInput();
                        const double dragSec = std::max(0.0, quickscript::ScalePlaybackTimeSeconds(
                            a.duration + RandomDelay(a.randomDuration), playbackTimeScale));
                        if (dragSec > 1e-4) {
                            // 按「按下→松开」墙钟插值。旧实现每步 Sleep(时长/步数)，
                            // SendInput/投递耗时叠在间隔外，实际总比设定慢一截。
                            using clock = std::chrono::steady_clock;
                            const auto t0 = clock::now();
                            const auto tEnd = t0 + std::chrono::duration_cast<clock::duration>(
                                std::chrono::duration<double>(dragSec));
                            int lastPx = sx;
                            int lastPy = sy;
                            while (!StopRequested() && !wmAbortIfTargetLost()) {
                                const auto now = clock::now();
                                double u = 1.0;
                                if (now < tEnd && dragSec > 1e-12) {
                                    u = std::chrono::duration<double>(now - t0).count() / dragSec;
                                    if (u < 0.0) u = 0.0;
                                    if (u > 1.0) u = 1.0;
                                }
                                const int px = sx + static_cast<int>(std::lround((ex - sx) * u));
                                const int py = sy + static_cast<int>(std::lround((ey - sy) * u));
                                if (px != lastPx || py != lastPy) {
                                    wmSetLivePos(px, py, 0, 0);
                                    lastPx = px;
                                    lastPy = py;
                                }
                                if (u >= 1.0 || now >= tEnd) break;
                                const double remSec = std::chrono::duration<double>(
                                    tEnd - clock::now()).count();
                                if (remSec <= 0.0) break;
                                SleepInterruptible((std::min)(remSec, 1.0 / 120.0));
                            }
                            if ((lastPx != ex || lastPy != ey)
                                && !StopRequested() && !wmAbortIfTargetLost()) {
                                wmSetLivePos(ex, ey, 0, 0);
                            }
                        } else {
                            wmSetLivePos(ex, ey, 0, 0);
                        }
                        lift();
                    }
                }
                else if (a.type == ActionType::KeyDown) {
                    const UINT playVk = NormalizeScriptKeyVk(a.keyVk, a.keyText);
                    // 已按住再 KeyDown = 录制自动重复；精密回放跳过注入，避免向游戏灌重复 KEYDOWN
                    if (inputTimeline.enabled && heldKeys.count(playVk)) {
                        if (KeyFunctionDebugActive() && !DeferredDetailDebugDropped()) {
                            AppendDebugLog(FormatGenericActionDebug(a) + L" (已按住，跳过)");
                        }
                    } else {
                        if (KeyFunctionDebugActive() && !DeferredDetailDebugDropped()) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        const bool markSim = !wmUsesTarget();
                        if (markSim) MarkSimulatedInput();
                        wmSendHeldModifiers(a, true);
                        wmSendKey(playVk, true);
                        heldKeyVk = playVk;
                        heldKeys.insert(playVk);
                        if (markSim) UnmarkSimulatedInput();
                    }
                }
                else if (a.type == ActionType::KeyUp) {
                    const UINT playVk = NormalizeScriptKeyVk(a.keyVk, a.keyText);
                    if (KeyFunctionDebugActive() && !DeferredDetailDebugDropped()) {
                        AppendDebugLog(FormatGenericActionDebug(a));
                    }
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    wmSendKey(playVk, false);
                    if (heldKeyVk == playVk) heldKeyVk = 0;
                    heldKeys.erase(playVk);
                    wmSendHeldModifiers(a, false);
                    if (markSim) UnmarkSimulatedInput();
                }
                else if (a.type == ActionType::KeyClick) for (int i = 0; i < a.clickCount && !StopRequested(); ++i) {
                    const UINT playVk = NormalizeScriptKeyVk(a.keyVk, a.keyText);
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    // 表格导航键/ASCII 键前先切英文 IME：中文组合窗会把 Enter/Tab/Home 吃掉
                    // 软投递后台窗口不经系统 IME，勿改用户前台输入法。
                    if (!wmUsesTarget() || (wmExecPtr && wmExecPtr->PreferHardwareInput())) {
                        if (!isImeToggleShortcut(a)) ForceEnglishImeBeforeAsciiKey();
                    }
                    // 按键点击 = 真实物理按键：必须发 KEYDOWN/KEYUP（scan code），
                    // 不能用 KEYEVENTF_UNICODE——游戏/DirectInput/轮询键盘状态的应用
                    // 收不到 Unicode 事件，导致「按键点击失效」（鼠标正常、按键无反应）。
                    // 文字输入请用 quickInput（走 Unicode，天然绕开中文 IME）。
                    wmSendHeldModifiers(a, true);
                    wmSendKey(playVk, true);
                    wmSendKey(playVk, false);
                    wmSendHeldModifiers(a, false);
                    if (markSim) UnmarkSimulatedInput();
                    if (sleepRepeatInterval(a, i)) return;
                }
                else if (a.type == ActionType::HotkeyShortcut) for (int i = 0; i < a.clickCount && !StopRequested(); ++i) {
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    if (!wmUsesTarget() || (wmExecPtr && wmExecPtr->PreferHardwareInput())) {
                        if (!isImeToggleShortcut(a)) ForceEnglishImeBeforeAsciiKey();
                    }
                    wmSendShortcut(a);
                    if (markSim) UnmarkSimulatedInput();
                    if (sleepRepeatInterval(a, i)) return;
                }
                else if (a.type == ActionType::QuickInput) for (int i = 0; i < a.clickCount && !StopRequested(); ++i) {
                    // 不在整段输入期间持有 MarkSimulatedInput：否则热键停止会被 ShouldIgnoreHotkeyStop 挡住。
                    // 注入键由 LL 钩子按 ExtraInfo 标签过滤（远控 INJECTED 仍可停）；字间轮询 stopFlag_
                    // （紧急停会经 ghWorkerCancelFlag 一并置位）以便立即终止。
                    const std::wstring text = ResolveQuickInputText(a.inputText, a.parseEscapes);
                    if (text.empty() && !Trim(a.inputText).empty()) {
                        AppendDebugLog(L"[警告] quickInput 文本解析后为空（原文本："
                            + a.inputText
        + L"）。变量请使用 {var} / {var.属性} / {time:格式} / {Now}。");
                    }
                    if (wmExecPtr && wmExecPtr->IsActive()) {
                        // 窗口/后台窗口模式 soft/CDP 输入直接投递给目标窗口，不经系统 IME，无需准备
                        wmExecPtr->SendQuickInputToTarget(text,
                            quickscript::ScalePlaybackTimeSeconds(a.charInterval, playbackTimeScale));
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            wchar_t buf[160]{};
                            swprintf_s(buf, L"快捷输入→目标窗口 hwnd=0x%p%s",
                                wmExecPtr->TargetHwnd(),
                                wmUsesBackground() ? L" [后台窗口模式]" : L" [独立桌面模式]");
                            AppendDebugLog(buf);
                        }
                    } else {
                        // 桌面模式：先清挂起组字（TSF 输入法 ImmSetConversionStatus 常不生效，
                        // 残留拼音会把数字/字母拼进组字串，如「1」→「h1」），含 ASCII 再切英文
                        PrepareImeForTextInput(text);
                        SendQuickInputText(text,
                            quickscript::ScalePlaybackTimeSeconds(a.charInterval, playbackTimeScale),
                            &stopFlag_);
                    }
                    if (sleepRepeatInterval(a, i)) return;
                }
                else if (a.type == ActionType::ScrollWheel) for (int i = 0; i < a.clickCount && !StopRequested(); ++i) {
                    MarkSimulatedInput();
                    const bool positive = a.scrollDirection == 0;
                    if (wmUsesTarget()) {
                        if (a.scrollVertical) {
                            wmExecPtr->PostScrollWheelAtClient(a.x, a.y, a.scrollSteps, true, positive);
                        }
                        if (a.scrollHorizontal) {
                            wmExecPtr->PostScrollWheelAtClient(a.x, a.y, a.scrollSteps, false, positive);
                        }
                    } else {
                        const int delta = (positive ? 1 : -1) * WHEEL_DELTA;
                        for (int step = 0; step < a.scrollSteps; ++step) {
                            if (a.scrollVertical)
                                MouseInputRouter::Instance().Wheel(delta, false);
                            if (a.scrollHorizontal)
                                MouseInputRouter::Instance().Wheel(delta, true);
                        }
                    }
                    UnmarkSimulatedInput();
                    if (sleepRepeatInterval(a, i)) return;
                }
                else if (a.type == ActionType::FindImage) {
                    // 找图/保存图片时隐藏本软件窗口（主界面+调试输出窗）：
                    // 否则日志滚动会被当成界面变化、整屏截进图片变量（误匹配 + 烧 API）。
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwnUi(
                        UserFacingMainHwnd());
                    const TemplateScale findTmplScale = currentTmplScale();
                    ScriptAction findAct = a;
                    findAct.imagePath = resolveTemplatePath(a.imageUseVar, a.imagePath);
                    const bool hasTemplate = a.imageUseVar
                        ? !a.imagePath.empty()
                        : !a.imagePath.empty();

                    // ── 后续操作：保存图片 ──
                    if (a.findImageFollowUp == 3) {
                      const double saveImgFindTimeSec = hasTemplate
                          ? ResolveFindImageTimeSec(a.findTimeExpr, makeVarCtx())
                          : 0.0;
                      const bool saveImgLoopUntilFound = hasTemplate && saveImgFindTimeSec < 0.0;
                      auto saveImgFindStart = std::chrono::steady_clock::now();
                      for (;;) {
                        const std::wstring saveTarget = a.matchVarName.empty() ? L"image" : a.matchVarName;
                        int capX1 = 0, capY1 = 0, capX2 = 0, capY2 = 0;
                        bool regionOk = false;
                        ImageMatchResult lastRawMatch{};

                        if (!hasTemplate) {
                            if (wmUsesTarget()) {
                                int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                                ScriptAction probe = a;
                                if (wmExecPtr->ResolveClientSearchRect(probe, cx1, cy1, cx2, cy2)
                                    && wmExecPtr->MapClientRect(cx1, cy1, cx2, cy2,
                                        capX1, capY1, capX2, capY2)) {
                                    regionOk = true;
                                }
                            } else if (a.searchFullScreen) {
                                int sx = 0, sy = 0, sw = 0, sh = 0;
                                GetVirtualScreenRect(sx, sy, sw, sh);
                                capX1 = sx; capY1 = sy; capX2 = sx + sw; capY2 = sy + sh;
                                regionOk = (capX2 > capX1 && capY2 > capY1);
                            } else {
                                capX1 = a.searchX1; capY1 = a.searchY1;
                                capX2 = a.searchX2; capY2 = a.searchY2;
                                // 空/倒置区域按整屏处理（与编辑器测试一致），避免静默截 0 面积
                                if (capX2 <= capX1 || capY2 <= capY1) {
                                    int sx = 0, sy = 0, sw = 0, sh = 0;
                                    GetVirtualScreenRect(sx, sy, sw, sh);
                                    capX1 = sx; capY1 = sy; capX2 = sx + sw; capY2 = sy + sh;
                                }
                                regionOk = (capX2 > capX1 && capY2 > capY1);
                            }
                        } else {
                            // 有模板：找锚图 → ApplyImageRegion → 截图区
                            if (findAct.imagePath.empty()) {
                                regionOk = false;
                            } else if (wmUsesTarget()) {
                                ImageMatchOutput output = wmExecPtr->FindImageClient(
                                    findAct, lockedScreen_, lockedVirtX_, lockedVirtY_);
                                if (!output.matches.empty()) {
                                    lastRawMatch = output.matches.front();
                                    const ImageMatchResult& match = output.matches.front();
                                    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                                    if (ApplyImageRegionToMatch(a,
                                            match.topLeftX, match.topLeftY,
                                            match.bottomRightX, match.bottomRightY,
                                            cx1, cy1, cx2, cy2)
                                        && wmExecPtr->MapClientRect(cx1, cy1, cx2, cy2,
                                            capX1, capY1, capX2, capY2)) {
                                        regionOk = true;
                                    }
                                }
                            } else {
                                int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                                if (a.searchFullScreen) {
                                    int sx = 0, sy = 0, sw = 0, sh = 0;
                                    GetVirtualScreenRect(sx, sy, sw, sh);
                                    x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                                }
                                HBITMAP tmpl = LoadBitmapFromFile(findAct.imagePath);
                                if (tmpl) {
                                    ImageMatchOptions opt = BuildExecutionFindImageOptions(findAct, findTmplScale);
                                    RestrictFindImageToSingleAnchor(opt);
                                    opt.maxOverlap = 0.5;
                                    ImageMatchOutput output;
                                    if (lockedScreen_) {
                                        output = FindTemplateInFrozenScreenMulti(
                                            lockedScreen_, lockedVirtX_, lockedVirtY_,
                                            x1, y1, x2, y2, tmpl, opt);
                                    } else {
                                        output = FindTemplateOnScreenMulti(x1, y1, x2, y2, tmpl, opt);
                                    }
                                    DeleteBitmapHandle(tmpl);
                                    if (!output.matches.empty()) {
                                        lastRawMatch = output.matches.front();
                                        const ImageMatchResult& match =
                                            NormalizeMatchVarResult(output.matches.front(), a.matchThreshold,
                                                                    a.perfectMatch);
                                        if (match.found) {
                                            regionOk = ApplyImageRegionToMatch(a,
                                                match.topLeftX, match.topLeftY,
                                                match.bottomRightX, match.bottomRightY,
                                                capX1, capY1, capX2, capY2);
                                        }
                                    }
                                }
                            }
                        }

                        if (regionOk) {
                            HBITMAP bmp = nullptr;
                            if (wmUsesTarget()) {
                                bmp = wmExecPtr->CaptureScreenRegionFromWindow(
                                    capX1, capY1, capX2, capY2,
                                    lockedScreen_, lockedVirtX_, lockedVirtY_);
                            } else {
                                bmp = CaptureScreenOrFrozenRegion(
                                    capX1, capY1, capX2, capY2,
                                    lockedScreen_, lockedVirtX_, lockedVirtY_);
                            }
                            if (bmp) {
                                CommitSavedImage(bmp, saveTarget, imageVarRunId, imageVars_);
                                DeleteBitmapHandle(bmp);
                            }
                        }
                        if (fireImageWatches(false) || fireImageWatches(true)) {
                            if (pendingGoto || pendingBreakLoop) break;
                            if (hasTemplate) {
                                saveImgFindStart = std::chrono::steady_clock::now();
                            }
                            continue;
                        }
                        bool saveImgDone = regionOk || !hasTemplate;
                        if (!saveImgDone && !saveImgLoopUntilFound) {
                            if (saveImgFindTimeSec <= 0.0) {
                                saveImgDone = true;
                            } else {
                                const double elapsed = std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - saveImgFindStart).count();
                                if (elapsed >= saveImgFindTimeSec) saveImgDone = true;
                            }
                        }
                        if (saveImgDone || StopRequested() || BreakoutTriggered()) {
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                AppendDebugLog(FormatFindImageDebug(a, lastRawMatch, regionOk, 0, 0));
                            }
                            break;
                        }
                        SleepInterruptible(0.05);
                      }
                    } else {
                    // 窗口/后台模式也必须 Prepare：偏移 nOffset 依赖 templateW/H。
                    // 若这里留空，ResolveFindImageClickPoint 会把偏移算成 0（落在中心）。
                    const PreparedFindImageMatch findPrep = PrepareFindImageMatch(findAct, findTmplScale);
                    int lastFindMs = 0;
                    bool findUsedAnamorphic = false;
                    auto runFind = [&]() -> ImageMatchResult {
                        findUsedAnamorphic = false;
                        if (wmUsesTarget()) {
                            ImageMatchOutput output = wmExecPtr->FindImageClient(
                                findAct, lockedScreen_, lockedVirtX_, lockedVirtY_);
                            lastFindMs = output.elapsedMs;
                            if (appSettings_.playback.autoOutputKeyFunctionDebug
                                && output.matches.empty()) {
                                wchar_t buf[320]{};
                                swprintf_s(buf,
                                    L"找图诊断(窗口/后台窗口模式) 无匹配 %dms bestNcc=%.1f%% pixelAgree=%.1f%% "
                                    L"（应走扩展/客户区；若见全屏找图调试则窗口/后台窗口模式未激活）",
                                    output.elapsedMs, output.debugBestNccPercent,
                                    output.debugBestPixelAgreePercent);
                                AppendDebugLog(buf);
                            }
                            if (output.matches.empty()) return {};
                            return output.matches.front();
                        }
                        if (a.windowRelative) {
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                AppendDebugLog(
                                    L"找图跳过：动作为窗口相对，但窗口/后台窗口模式未绑定；"
                                    L"全屏桌面 GDI 对游戏会得到假 0%");
                            }
                            return {};
                        }
                        int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                        if (a.searchFullScreen) {
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                        } else {
                            int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
                            GetVirtualScreenRect(vsX, vsY, vsW, vsH);
                            // 默认模式：搜索区域为空/倒置（含全 0）时回退整屏，
                            // 否则运行期会在 0 面积区域上找图，rawCandidates=0 永远匹配不到。
                            // 窗口/后台窗口模式不走这里，由 FindImageClient / ResolveClientSearchRect 用全客户区。
                            if ((x2 <= x1 || y2 <= y1)
                                || (x1 <= vsX + 2 && y1 <= vsY + 2
                                    && x2 >= vsX + vsW - 2 && y2 >= vsY + vsH - 2)) {
                                x1 = vsX; y1 = vsY; x2 = vsX + vsW; y2 = vsY + vsH;
                            }
                        }
                        HBITMAP tmpl = findPrep.bitmap;
                        if (!tmpl) {
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                AppendDebugLog(L"找图失败: 无法加载模板 " + findAct.imagePath);
                            }
                            return {};
                        }
                        ImageMatchOptions opt = findPrep.options;
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            wchar_t buf[960]{};
                            swprintf_s(buf,
                                L"找图调试 ref=%dx%d cap=%dx%d cur=%dx%d@%ddpi scale=%.3fx%.3f "
                                L"preScale=%d matchScale=%.3f~%.3f tpl=%dx%d search=(%d,%d)-(%d,%d) thr=%.0f locked=%d",
                                activeCoordMeta.refWidth, activeCoordMeta.refHeight,
                                activeCoordMeta.captureWidth > 0 ? activeCoordMeta.captureWidth
                                    : activeCoordMeta.refWidth,
                                activeCoordMeta.captureHeight > 0 ? activeCoordMeta.captureHeight
                                    : activeCoordMeta.refHeight,
                                execTargetW, execTargetH, GetDpiForSystem(),
                                findTmplScale.sx, findTmplScale.sy,
                                findPrep.templatePreScaled ? 1 : 0,
                                opt.scaleMin, opt.scaleMax,
                                findPrep.templateW, findPrep.templateH, x1, y1, x2, y2, opt.thresholdPercent,
                                lockedScreen_ ? 1 : 0);
                            AppendDebugLog(buf);
                        }
                        RestrictFindImageToSingleAnchor(opt);
                        opt.maxOverlap = 0.5;
                        auto doMatch = [&](HBITMAP bmp, const ImageMatchOptions& matchOpt) -> ImageMatchOutput {
                            if (lockedScreen_) {
                                return FindTemplateInFrozenScreenMulti(
                                    lockedScreen_, lockedVirtX_, lockedVirtY_, x1, y1, x2, y2, bmp, matchOpt);
                            }
                            return FindTemplateOnScreenMulti(x1, y1, x2, y2, bmp, matchOpt);
                        };
                        ImageMatchOutput output = doMatch(tmpl, opt);
                        lastFindMs = output.elapsedMs;
                        // 宽高比变化时：仅当 NCC 还有希望时再试非等比拉伸
                        const bool aspectChanged =
                            std::abs(findTmplScale.sx - findTmplScale.sy) > 0.02;
                        if (output.matches.empty() && aspectChanged
                            && output.debugBestNccPercent >= opt.thresholdPercent * 0.40) {
                            HBITMAP stretched = LoadScaledTemplateBitmap(
                                findAct.imagePath, findTmplScale.sx, findTmplScale.sy);
                            if (stretched) {
                                ImageMatchOptions stretchOpt = opt;
                                stretchOpt.scaleMin = 1.0;
                                stretchOpt.scaleMax = 1.0;
                                stretchOpt.scaleStep = 1.0;
                                stretchOpt.disablePyramid = true;
                                stretchOpt.crossResolutionMatch = true;
                                ImageMatchOutput stretchOut = doMatch(stretched, stretchOpt);
                                lastFindMs += stretchOut.elapsedMs;
                                if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                    wchar_t buf[320]{};
                                    swprintf_s(buf,
                                        L"找图兜底 非等比拉伸 bestNcc=%.1f%% matches=%d",
                                        stretchOut.debugBestNccPercent,
                                        static_cast<int>(stretchOut.matches.size()));
                                    AppendDebugLog(buf);
                                }
                                if (!stretchOut.matches.empty()) {
                                    findUsedAnamorphic = true;
                                    output = std::move(stretchOut);
                                }
                                DeleteBitmapHandle(stretched);
                            }
                        }

                        if (appSettings_.playback.autoOutputKeyFunctionDebug && output.matches.empty()) {
                            wchar_t buf[320]{};
                                swprintf_s(buf,
                                    L"找图诊断 无共识匹配 %dms rawCandidates=%d bestNcc=%.1f%% pixelAgree=%.1f%%",
                                    output.elapsedMs, output.debugRawCandidates, output.debugBestNccPercent,
                                    output.debugBestPixelAgreePercent);
                            AppendDebugLog(buf);
                            // 模板过大提示：整屏/大区域模板对光标、时钟、消息等任何微小变化都敏感，
                            // 阈值越高越容易「无共识」→ 循环里反复触发后续动作烧 API。
                            const long long searchArea =
                                static_cast<long long>(std::max(1, x2 - x1))
                                * std::max(1, y2 - y1);
                            const long long tplArea =
                                static_cast<long long>(findPrep.templateW)
                                * findPrep.templateH;
                            if (searchArea > 0 && tplArea * 100 >= searchArea * 70) {
                                wchar_t warn[320]{};
                                swprintf_s(warn,
                                    L"找图提示 模板过大（约占搜索区 %d%%）：整屏/大区域模板对任何微小变化都敏感，"
                                    L"建议改用小目标区域、降低阈值，或在循环里加变化确认，避免反复触发后续步骤。",
                                    static_cast<int>(tplArea * 100 / searchArea));
                                AppendDebugLog(warn);
                            }
                        }
                        if (output.matches.empty()) {
                            return {};
                        }
                        return output.matches.front();
                    };
                    ImageMatchResult lastRawMatch{};
                    int lastTargetX = 0, lastTargetY = 0;
                    bool lastHadTarget = false;
                    // 找图时限按脚本原值（含正数），不随回放倍速缩放
                    const double findTimeSec = ResolveFindImageTimeSec(a.findTimeExpr, makeVarCtx());
                    const bool loopUntilFound = findTimeSec < 0.0;
                    auto findStart = std::chrono::steady_clock::now();
                    do {
                        const ImageMatchResult rawMatch = runFind();
                        lastRawMatch = rawMatch;
                        if (fireImageWatches(false) || fireImageWatches(true)) {
                            if (pendingGoto || pendingBreakLoop) break;
                            findStart = std::chrono::steady_clock::now();
                            continue;
                        }
                        const ImageMatchResult match = NormalizeMatchVarResult(
                            rawMatch, a.matchThreshold, a.perfectMatch);
                        if (a.findImageFollowUp == 2) {
                            const std::wstring varName = a.matchVarName.empty() ? L"matchRet" : a.matchVarName;
                            matchVars_[varName] = match;
                            if (match.found) break;
                            else if (!loopUntilFound) {
                                if (findTimeSec <= 0.0) break;
                                const double elapsed = std::chrono::duration<double>(
                                    std::chrono::steady_clock::now() - findStart).count();
                                if (elapsed >= findTimeSec) break;
                            }
                        } else if (match.found) {
                            int tx = 0, ty = 0;
                            ResolveFindImageClickPoint(match, findPrep.templateW, findPrep.templateH,
                                a.nOffsetX, a.nOffsetY, findTmplScale,
                                findPrep.templatePreScaled || findUsedAnamorphic, tx, ty);
                            lastTargetX = tx;
                            lastTargetY = ty;
                            lastHadTarget = true;
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                int cx = 0, cy = 0;
                                FindImageMatchCenter(match, cx, cy);
                                int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
                                GetVirtualScreenBounds(vsX, vsY, vsW, vsH);
                                wchar_t buf[640]{};
                                swprintf_s(buf,
                                    L"找图落点 %dms tl=(%d,%d) box=%dx%d scale=%.3f "
                                    L"center=(%d,%d) norm=(%.4f,%.4f) "
                                    L"offNorm=(%.4f,%.4f) offScaled=(%d,%d) target=(%d,%d)",
                                    lastFindMs,
                                    match.topLeftX, match.topLeftY,
                                    match.bottomRightX - match.topLeftX,
                                    match.bottomRightY - match.topLeftY,
                                    match.scale,
                                    cx, cy,
                                    vsW > 0 ? (cx - vsX) / static_cast<double>(vsW) : 0.0,
                                    vsH > 0 ? (cy - vsY) / static_cast<double>(vsH) : 0.0,
                                    a.nOffsetX, a.nOffsetY, tx - cx, ty - cy, tx, ty);
                                AppendDebugLog(buf);
                            }
                            const std::wstring varName = a.matchVarName.empty() ? L"matchRet" : a.matchVarName;
                            matchVars_[varName] = match;
                            if (a.findImageFollowUp == 0) {
                                applyFindCursor(tx, ty, true, a.button, a);
                            } else if (a.findImageFollowUp == 1) {
                                applyFindCursor(tx, ty, false, a.button, a);
                                if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                    wchar_t buf[160]{};
                                    if (wmUsesTarget()) {
                                        int ccx = 0, ccy = 0;
                                        if (wmExecPtr->GetCursorClientPos(ccx, ccy)) {
                                            swprintf_s(buf, L"找图软光标客户区=(%d,%d) 目标=(%d,%d)",
                                                ccx, ccy, tx, ty);
                                        } else {
                                            swprintf_s(buf, L"找图软光标未知 目标=(%d,%d)", tx, ty);
                                        }
                                    } else {
                                        POINT cur{};
                                        GetCursorPos(&cur);
                                        swprintf_s(buf, L"找图光标实际位置=(%d,%d)", cur.x, cur.y);
                                    }
                                    AppendDebugLog(buf);
                                }
                            }
                            break;
                        } else if (!loopUntilFound) {
                            if (findTimeSec <= 0.0) break;
                            const double elapsed = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - findStart).count();
                            if (elapsed >= findTimeSec) break;
                        }
                        // 可中断等待；窗口/后台窗口模式单次找图常 1~2s，重试间隔宜短以便热键立刻停
                        SleepInterruptible(0.05);
                    } while (!StopRequested() && !BreakoutTriggered());
                    if ((a.findImageFollowUp == 0 || a.findImageFollowUp == 1) && !lastHadTarget
                        && appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(L"找图未匹配，跳过点击/移动");
                    }
                    if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(FormatFindImageDebug(a, lastRawMatch, lastHadTarget, lastTargetX, lastTargetY));
                    }
                    if (findPrep.bitmap) {
                        DeleteBitmapHandle(findPrep.bitmap);
                    }
                    } // end else (non-saveImage followUp)
                }
                else if (a.type == ActionType::MultiMatch) {
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwnMm(
                        UserFacingMainHwnd());
                    ScriptAction mmAct = a;
                    NormalizeMultiMatchFields(mmAct);
                    if (mmAct.imagePaths.empty()) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(L"多图匹配跳过: 没有模板图");
                        }
                    } else {
                    const TemplateScale mmTmplScale = currentTmplScale();
                    struct MultiHit {
                        ImageMatchResult match;
                        int templateIndex = 0;
                        std::wstring templatePath;
                        int templateW = 0;
                        int templateH = 0;
                        bool templatePreScaled = false;
                    };
                    auto fileNameOf = [](const std::wstring& path) -> std::wstring {
                        const auto slash = path.find_last_of(L"\\/");
                        return slash == std::wstring::npos ? path : path.substr(slash + 1);
                    };
                    auto sortHits = [&](std::vector<ImageMatchResult>& matches) {
                        if (mmAct.multiMatchSort == 1) {
                            std::sort(matches.begin(), matches.end(),
                                [](const ImageMatchResult& l, const ImageMatchResult& r) {
                                    return l.score > r.score;
                                });
                        } else {
                            std::sort(matches.begin(), matches.end(),
                                [](const ImageMatchResult& l, const ImageMatchResult& r) {
                                    if (l.topLeftX != r.topLeftX) return l.topLeftX < r.topLeftX;
                                    return l.topLeftY < r.topLeftY;
                                });
                        }
                    };
                    auto prepTemplateSize = [&](const std::wstring& path, bool useVar, int& w, int& h, bool& preScaled) {
                        ScriptAction probe = mmAct;
                        probe.imagePath = useVar ? path : resolveTemplatePath(false, path);
                        probe.imageUseVar = useVar;
                        if (useVar) probe.imagePath = resolveTemplatePath(true, path);
                        const PreparedFindImageMatch prep = PrepareFindImageMatch(probe, mmTmplScale);
                        w = prep.templateW;
                        h = prep.templateH;
                        preScaled = prep.templatePreScaled;
                        if (prep.bitmap) DeleteBitmapHandle(prep.bitmap);
                    };
                    auto collectHits = [&]() -> std::vector<MultiHit> {
                        std::vector<MultiHit> hits;
                        if (mmAct.multiMatchMode == 1) {
                            ScriptAction probe = mmAct;
                            probe.imagePath = mmAct.imagePaths.front();
                            probe.imageUseVar = MultiMatchSlotUseVar(mmAct, 0);
                            ImageMatchOutput output = matchScriptImageAll(probe, mmAct.multiMatchMax);
                            sortHits(output.matches);
                            int tw = 0, th = 0;
                            bool pre = false;
                            prepTemplateSize(probe.imagePath, probe.imageUseVar, tw, th, pre);
                            for (const auto& raw : output.matches) {
                                const ImageMatchResult match = NormalizeMatchVarResult(
                                    raw, mmAct.matchThreshold, mmAct.perfectMatch);
                                if (!match.found) continue;
                                MultiHit hit;
                                hit.match = match;
                                hit.templateIndex = 0;
                                hit.templatePath = probe.imagePath;
                                hit.templateW = tw;
                                hit.templateH = th;
                                hit.templatePreScaled = pre;
                                hits.push_back(std::move(hit));
                                if (static_cast<int>(hits.size()) >= mmAct.multiMatchMax) break;
                            }
                            return hits;
                        }
                        for (int i = 0; i < static_cast<int>(mmAct.imagePaths.size()); ++i) {
                            ScriptAction probe = mmAct;
                            probe.imagePath = mmAct.imagePaths[static_cast<size_t>(i)];
                            probe.imageUseVar = MultiMatchSlotUseVar(mmAct, i);
                            ImageMatchOutput output = matchScriptImageAll(probe, 1);
                            if (output.matches.empty()) continue;
                            const ImageMatchResult match = NormalizeMatchVarResult(
                                output.matches.front(), mmAct.matchThreshold, mmAct.perfectMatch);
                            if (!match.found) continue;
                            MultiHit hit;
                            hit.match = match;
                            hit.templateIndex = i;
                            hit.templatePath = probe.imagePath;
                            prepTemplateSize(probe.imagePath, probe.imageUseVar, hit.templateW, hit.templateH,
                                hit.templatePreScaled);
                            hits.push_back(std::move(hit));
                            break;
                        }
                        return hits;
                    };
                    std::vector<MultiHit> lastHits;
                    const double findTimeSec = ResolveFindImageTimeSec(mmAct.findTimeExpr, makeVarCtx());
                    const bool loopUntilFound = findTimeSec < 0.0;
                    auto findStart = std::chrono::steady_clock::now();
                    do {
                        lastHits = collectHits();
                        if (fireImageWatches(false) || fireImageWatches(true)) {
                            if (pendingGoto || pendingBreakLoop) break;
                            findStart = std::chrono::steady_clock::now();
                            continue;
                        }
                        if (!lastHits.empty()) break;
                        if (!loopUntilFound) {
                            if (findTimeSec <= 0.0) break;
                            const double elapsed = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - findStart).count();
                            if (elapsed >= findTimeSec) break;
                        }
                        SleepInterruptible(0.05);
                    } while (!StopRequested() && !BreakoutTriggered());

                    const std::wstring varName =
                        mmAct.matchVarName.empty() ? L"matchRet" : mmAct.matchVarName;
                    ImageMatchListVar list;
                    list.hits.reserve(lastHits.size());
                    for (const auto& hit : lastHits) {
                        ImageMatchListHit stored;
                        stored.match = hit.match;
                        stored.templateIndex = hit.templateIndex;
                        stored.templateName = fileNameOf(hit.templatePath);
                        list.hits.push_back(std::move(stored));
                    }
                    matchListVars_[varName] = std::move(list);
                    if (lastHits.empty()) {
                        ImageMatchResult miss{};
                        matchVars_[varName] = miss;
                    } else {
                        matchVars_[varName] = lastHits.front().match;
                    }

                    const int follow = mmAct.findImageFollowUp;
                    if (lastHits.empty() && (follow == 0 || follow == 1)
                        && appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(L"多图匹配未命中，跳过点击/移动");
                    }
                    if (!lastHits.empty() && follow != 2) {
                        auto clickOrMove = [&](const MultiHit& hit) -> bool {
                            int tx = 0, ty = 0;
                            ResolveFindImageClickPoint(hit.match, hit.templateW, hit.templateH,
                                mmAct.nOffsetX, mmAct.nOffsetY, mmTmplScale,
                                hit.templatePreScaled, tx, ty);
                            return applyFindCursor(tx, ty, follow == 0, mmAct.button, mmAct);
                        };
                        if (follow == 1 || mmAct.multiMatchMode != 1) {
                            if (clickOrMove(lastHits.front())) return;
                        } else {
                            for (size_t i = 0; i < lastHits.size(); ++i) {
                                if (StopRequested() || BreakoutTriggered()) break;
                                if (clickOrMove(lastHits[i])) return;
                                if (i + 1 < lastHits.size()
                                    && (mmAct.duration > 0.0 || mmAct.randomDuration > 0.0)
                                    && mmAct.clickCount <= 1) {
                                    SleepInterruptible(quickscript::ScalePlaybackTimeSeconds(
                                        mmAct.duration + RandomDelay(mmAct.randomDuration),
                                        playbackTimeScale));
                                }
                            }
                        }
                    }
                    if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(L"多图匹配 "
                            + std::wstring(mmAct.multiMatchMode == 1 ? L"一图多处" : L"多图择一")
                            + L" 命中=" + std::to_wstring(static_cast<int>(lastHits.size()))
                            + L" 变量=" + varName);
                    }
                    }
                }
                else if (a.type == ActionType::TextRecognition) {
                    // OCR 按图定位/识别同样隐藏本软件窗口，避免日志窗进入识别区域
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwnOcr(
                        UserFacingMainHwnd());
                    usesOcr = true;
                    workerUsesOcrVars_ = true;
                    holdOcrSession();
                    const std::wstring varName = a.matchVarName.empty() ? L"a" : a.matchVarName;
                    auto resolveOcrRegion = [&](int& x1, int& y1, int& x2, int& y2) -> bool {
                        if (wmUsesTarget()) {
                            if (a.ocrRegionByImage) {
                                ScriptAction probe = a;
                                probe.imagePath = resolveTemplatePath(a.imageUseVar, a.imagePath);
                                // search* 为绝对找图范围；相对偏移在 imageRegion*
                                ImageMatchOutput output = wmExecPtr->FindImageClient(
                                    probe, lockedScreen_, lockedVirtX_, lockedVirtY_);
                                if (output.matches.empty()) return false;
                                const ImageMatchResult& match = output.matches.front();
                                int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                                if (!ApplyImageRegionToMatch(a,
                                        match.topLeftX, match.topLeftY,
                                        match.bottomRightX, match.bottomRightY,
                                        cx1, cy1, cx2, cy2)) {
                                    return false;
                                }
                                return wmExecPtr->MapClientRect(cx1, cy1, cx2, cy2, x1, y1, x2, y2);
                            }
                            if (!wmExecPtr->ResolveClientSearchRect(a, x1, y1, x2, y2)) return false;
                            return wmExecPtr->MapClientRect(x1, y1, x2, y2, x1, y1, x2, y2);
                        }
                        if (a.ocrRegionByImage) {
                            // 「按图取区域」要先找图 → 必须有 OpenCV。
                            // 没有就老实失败（OCR 得到空结果），**不要**硬着头皮往下走：
                            // 下面 LoadBitmapFromFile 会引用 OpenCV 符号，delay-load 桩
                            // 在缺 DLL 时会抛 0xC06D007E 把整个进程带走。
                            if (!OpenCvAvailable()) return false;
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            int findX1 = sx, findY1 = sy, findX2 = sx + sw, findY2 = sy + sh;
                            if (!a.searchFullScreen && a.searchX2 > a.searchX1 && a.searchY2 > a.searchY1) {
                                findX1 = a.searchX1;
                                findY1 = a.searchY1;
                                findX2 = a.searchX2;
                                findY2 = a.searchY2;
                            }
                            const TemplateScale tmplScale = currentTmplScale();
                            const std::wstring tmplPath = resolveTemplatePath(a.imageUseVar, a.imagePath);
                            HBITMAP tmpl = LoadBitmapFromFile(tmplPath);
                            if (!tmpl) return false;
                            ImageMatchOptions opt = BuildExecutionFindImageOptions(a, tmplScale);
                            RestrictFindImageToSingleAnchor(opt);
                            opt.maxOverlap = 0.5;
                            ImageMatchOutput output;
                            if (lockedScreen_) {
                                output = FindTemplateInFrozenScreenMulti(
                                    lockedScreen_, lockedVirtX_, lockedVirtY_,
                                    findX1, findY1, findX2, findY2, tmpl, opt);
                            } else {
                                output = FindTemplateOnScreenMulti(
                                    findX1, findY1, findX2, findY2, tmpl, opt);
                            }
                            DeleteBitmapHandle(tmpl);
                            if (output.matches.empty()) return false;
                            const ImageMatchResult& match = output.matches.front();
                            return ApplyImageRegionToMatch(a,
                                match.topLeftX, match.topLeftY,
                                match.bottomRightX, match.bottomRightY,
                                x1, y1, x2, y2);
                        }
                        if (a.searchFullScreen) {
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                        } else if (x2 <= x1 || y2 <= y1) {
                            // 空/倒置区域按整屏处理（与编辑器测试一致），避免 OCR 静默跑 0 面积区域
                            int sx = 0, sy = 0, sw = 0, sh = 0;
                            GetVirtualScreenRect(sx, sy, sw, sh);
                            x1 = sx; y1 = sy; x2 = sx + sw; y2 = sy + sh;
                        }
                        return true;
                    };
                    auto runOcrAction = [&]() -> OcrVarResult {
                        OcrEngineOutput output;
                        if (wmUsesTarget()) {
                            output = wmExecPtr->RunOcrOnClientRegion(
                                a, lockedScreen_, lockedVirtX_, lockedVirtY_);
                        } else {
                            int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                            if (!resolveOcrRegion(x1, y1, x2, y2)) {
                                return a.ocrResultMode == 1
                                    ? MakeOcrSearchMissingVarResult()
                                    : MakeOcrTextVarResult(L"");
                            }
                            output = RunOcrOnScreenRegion(
                                x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_, a.ocrDigitsOnly);
                        }
                        if (!output.success) {
                            return a.ocrResultMode == 1
                                ? MakeOcrSearchMissingVarResult()
                                : MakeOcrTextVarResult(L"");
                        }
                        if (a.ocrResultMode == 0) {
                            return MakeOcrTextVarResult(ConcatOcrLines(output));
                        }
                        MacroVariableContext ctx = makeVarCtx();
                        const std::wstring target = ResolveMacroVariables(a.ocrSearchText, ctx);
                        // 匹配度随命中行一起取出（文字查找存的就是它，别再自己算一遍）
                        const auto found = FindTextInOcrLinesScored(output, target);
                        if (found.has_value()) {
                            return MakeOcrSearchVarResult(found->line, found->matchData);
                        }
                        return MakeOcrSearchMissingVarResult();
                    };
                    auto applyFollowUpAt = [&](int centerX, int centerY) {
                        if (a.ocrFollowUp == 2) return;
                        int tx = centerX + a.offsetX;
                        int ty = centerY + a.offsetY;
                        if (wmExecPtr && wmExecPtr->IsActive()) {
                            windowmode::ScreenToClientPoint(
                                wmExecPtr->TargetHwnd(), tx, ty, tx, ty);
                        }
                        if (a.ocrFollowUp == 0) {
                            applyFindCursor(tx, ty, true, MouseButtonType::Left, a);
                        } else if (a.ocrFollowUp == 1) {
                            applyFindCursor(tx, ty, false, MouseButtonType::Left, a);
                        }
                    };
                    auto emitOcrDebug = [&](const std::wstring& textContent, bool searchFound,
                                            int matchData) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatOcrDebug(
                                a, textContent, searchFound, matchData, makeVarCtx()));
                        }
                    };
                    if (a.ocrResultMode == 0 && a.ocrFollowUp != 2) {
                        OcrEngineOutput output;
                        if (wmUsesTarget()) {
                            output = wmExecPtr->RunOcrOnClientRegion(
                                a, lockedScreen_, lockedVirtX_, lockedVirtY_);
                        } else {
                            int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                            if (!resolveOcrRegion(x1, y1, x2, y2)) {
                                ocrVars_[varName] = MakeOcrTextVarResult(L"");
                                emitOcrDebug(L"", false, 0);
                                return;
                            }
                            output = RunOcrOnScreenRegion(
                                x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_, a.ocrDigitsOnly);
                        }
                        const std::wstring text = output.success ? ConcatOcrLines(output) : L"";
                        ocrVars_[varName] = MakeOcrTextVarResult(text);
                        emitOcrDebug(text, false, 0);
                        if (output.success && !output.lines.empty()) {
                            const OcrTextLine* best = &output.lines.front();
                            for (const auto& line : output.lines) {
                                if (line.confidence > best->confidence) best = &line;
                            }
                            applyFollowUpAt((best->x1 + best->x2) / 2, (best->y1 + best->y2) / 2);
                        }
                    } else if (a.ocrResultMode == 0) {
                        OcrVarResult result = runOcrAction();
                        // 逻辑转化动态列表：错屏 OCR 时按备注 listActivate 再试一次
                        if (a.remark.find(L"文字识别：动态列表") != std::wstring::npos
                            && !LooksLikeListOcrText(result.text)) {
                            std::wstring listAct;
                            const size_t p = a.remark.find(L"|listActivate:");
                            if (p != std::wstring::npos)
                                listAct = Trim(a.remark.substr(p + 14));
                            if (!listAct.empty() && !wmUsesTarget()) {
                                AppendDebugLog(L"逻辑转化 OCR 不像列表，重试 activate+OCR："
                                    + listAct);
                                const auto all = windowmode::ListSwitchableWindows();
                                const auto hits = windowmode::MatchWindows(all, listAct);
                                if (hits.size() == 1) {
                                    std::wstring err;
                                    if (windowmode::ActivateWindow(hits.front().hwnd, err)) {
                                        std::this_thread::sleep_for(std::chrono::milliseconds(250));
                                        result = runOcrAction();
                                    }
                                }
                            }
                            if (!LooksLikeListOcrText(result.text)) {
                                AppendDebugLog(L"逻辑转化 OCR 仍不像列表，保留原文供动态填写 AI 自检");
                            }
                        }
                        ocrVars_[varName] = result;
                        emitOcrDebug(result.text, false, 0);
                    } else {
                        OcrVarResult lastResult{};
                        do {
                            const OcrVarResult result = runOcrAction();
                            lastResult = result;
                            ocrVars_[varName] = result;
                            if (result.found && a.ocrFollowUp != 2) {
                                const int centerX = (result.topLeftX + result.bottomRightX) / 2;
                                const int centerY = (result.topLeftY + result.bottomRightY) / 2;
                                applyFollowUpAt(centerX, centerY);
                                break;
                            }
                            if (result.found || !a.findUntilFound) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        } while (!StopRequested() && !BreakoutTriggered());
                        emitOcrDebug(lastResult.text, lastResult.found != 0, lastResult.matchData);
                    }
                }
                else if (a.type == ActionType::RunMacro) {
                    runNestedLibrary(a, false);
                }
                else if (a.type == ActionType::LockScreenshot) {
                    clearLockedScreen();
                    if (wmUsesTarget()) {
                        if (!wmExecPtr->LockWindowCapture(lockedScreen_, lockedVirtX_, lockedVirtY_)) {
                            AppendDebugLog(std::wstring(wmUsesBackground() ? L"后台窗口模式" : L"窗口模式")
                                + L"：锁定窗口截图失败，后续找图将使用实时截图");
                        }
                    } else {
                        lockedScreen_ = CaptureVirtualScreen(lockedVirtX_, lockedVirtY_);
                    }
                }
                else if (a.type == ActionType::UnlockScreenshot) {
                    clearLockedScreen();
                }
                else if (a.type == ActionType::StopMacro) {
                    // 运行宏调用栈：stopMacro 置全局停止，结束调用方。
                    // 定时插入：原脚本不是父脚本，只结束这次定时，随后从暂停处继续。
                    if (StopMacroShouldEndEntireRun(scheduledYieldDepth)) {
                        stopFlag_ = true;
                    } else {
                        scheduledYieldLocalStop = true;
                    }
                }
                else if (a.type == ActionType::EndLoop) {
                    if (aiLoopDepth > 0) pendingBreakLoop = true;
                }
                else if (a.type == ActionType::RunProgram) {
                    const std::wstring path = ResolveRunProgramPath(
                        a.shortcutPreset, ExpandEnvironmentVars(a.targetPath));
                    if (path.empty()) {
                        AppendDebugLog(L"运行程序失败：目标路径为空");
                    } else if (!LaunchProgram(path, a.inputText)) {
                        AppendDebugLog(L"运行程序失败：无法启动「" + path + L"」");
                        AppendAiDebugLog(L"  [诊断] runProgram 启动失败：「" + path
                            + L"」。若是浏览器请先 listWindows→activateWindow 复用已开窗口；"
                              L"裸名 edge 会被解析为 msedge.exe 真实路径；"
                              L"仍打不开请 openAppViaSearch(query=应用显示名) 走开始菜单搜索。");
                    }
                }
                else if (a.type == ActionType::CloseProgram) {
                    if (!a.targetPath.empty())
                        CloseProgramsByTarget(ExpandEnvironmentVars(a.targetPath),
                            a.matchFileNameOnly);
                }
                else if (a.type == ActionType::OpenWebpage) {
                    if (!a.targetPath.empty()) {
                        const std::wstring webTarget = ExpandEnvironmentVars(a.targetPath);
                        const auto schemeEnd = webTarget.find(L"://");
                        bool allow = false;
                        if (schemeEnd != std::wstring::npos) {
                            std::wstring scheme = webTarget.substr(0, schemeEnd);
                            for (auto& ch : scheme) {
                                if (ch >= L'A' && ch <= L'Z')
                                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                            }
                            allow = (scheme == L"http" || scheme == L"https");
                        } else {
                            // 无 scheme：当作 https 补全由系统处理前仍拒绝 file/UNC 裸路径冒充网页
                            allow = false;
                            AppendDebugLog(L"打开网页失败：仅允许 http/https URL");
                        }
                        if (allow && !LaunchProgram(webTarget, L"")) {
                            AppendDebugLog(L"打开网页失败：无法打开「" + webTarget + L"」");
                        } else if (!allow && schemeEnd != std::wstring::npos) {
                            AppendDebugLog(L"打开网页失败：仅允许 http/https URL");
                        }
                    }
                }
                else if (a.type == ActionType::OpenFile) {
                    if (!a.targetPath.empty())
                        LaunchProgram(ExpandEnvironmentVars(a.targetPath), L"");
                }
                else if (a.type == ActionType::ActivateWindow) {
                    const std::wstring query = Trim(ExpandEnvironmentVars(a.targetPath));
                    if (query.empty()) {
                        AppendDebugLog(L"激活窗口失败：match/targetPath 为空");
                    } else {
                        qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                        const auto all = windowmode::ListSwitchableWindows();
                        const auto hits = windowmode::MatchWindows(all, query);
                        if (hits.empty()) {
                            AppendDebugLog(L"激活窗口失败：无匹配「" + query + L"」");
                        } else if (hits.size() > 1) {
                            AppendDebugLog(L"激活窗口失败：「" + query + L"」匹配到 "
                                + std::to_wstring(hits.size()) + L" 个窗口，拒绝自动选择");
                        } else {
                            std::wstring err;
                            if (!windowmode::ActivateWindow(hits.front().hwnd, err)) {
                                AppendDebugLog(L"激活窗口失败：" + err);
                            } else {
                                AppendDebugLog(L"已激活窗口：" + hits.front().title);
                            }
                        }
                    }
                }
                else if (a.type == ActionType::TimerRecordTime) {
                    if (!a.loopVarName.empty()) {
                        timerStarts_[a.loopVarName] = std::chrono::steady_clock::now();
                    }
                }
                else if (a.type == ActionType::MousePlayback) {
                    runNestedLibrary(a, true);
                }
                else if (a.type == ActionType::GetCursorPos) {
                    int cx = 0, cy = 0;
                    bool gotPos = false;
                    if (wmExecPtr && wmExecPtr->IsActive()) {
                        gotPos = wmExecPtr->GetCursorClientPos(cx, cy);
                    } else {
                        POINT pt{};
                        gotPos = GetCursorPos(&pt) == TRUE;
                        cx = pt.x;
                        cy = pt.y;
                    }
                    const std::wstring varName = a.matchVarName.empty() ? L"a" : a.matchVarName;
                    if (gotPos) {
                        ImageMatchResult match{};
                        match.found = true;
                        match.topLeftX = cx;
                        match.topLeftY = cy;
                        match.bottomRightX = cx;
                        match.bottomRightY = cy;
                        match.x = cx;
                        match.y = cy;
                        match.score = 100.0;
                        matchVars_[varName] = match;
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(L"获取当前光标位置→[" + varName + L"] "
                                + std::to_wstring(cx) + L"," + std::to_wstring(cy));
                        }
                    } else {
                        // 取不到光标 ⇒ 必须清零（.x/.y 读到 0 才叫「失败」，
                        // 留着上一轮的坐标会让后续判定/移动跑到旧位置去）
                        matchVars_[varName] = {};
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(L"获取当前光标位置失败 → [" + varName + L"] 已清零");
                        }
                    }
                }
                else if (a.type == ActionType::VarCompute) {
                    MacroVariableContext ctx = makeVarCtx();
                    const VarComputeResult vr = RunVarCompute(a.computeCode, ctx, &stopFlag_);
                    // 警告先出：失败时用户能看到「哪个变量是空的」，成功时也能发现写错的名字
                    for (const auto& w : vr.warnings) AppendDebugLog(w);
                    if (!vr.ok) {
                        AppendDebugLog(L"变量运算失败：" + vr.error);
                    } else {
                        for (const auto& kv : vr.exported) {
                            userVars_[kv.first] = kv.second;
                        }
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            std::wstring line = L"变量运算导出 ";
                            if (vr.exported.empty()) line += L"（无 return）";
                            else {
                                bool first = true;
                                for (const auto& kv : vr.exported) {
                                    if (!first) line += L", ";
                                    first = false;
                                    line += kv.first + L"=" + kv.second;
                                }
                            }
                            AppendDebugLog(line);
                        }
                    }
                }
                else if (a.type == ActionType::GetColor) {
                    MacroVariableContext ctx = makeVarCtx();
                    int px = a.x, py = a.y;
                    if (a.imageLocate) {
                        ImageMatchResult loc{};
                        int tplW = 0, tplH = 0;
                        if (!findLocateAnchor(a, loc, tplW, tplH)) return;
                        const double nsx = tplW > 0 ? a.x / static_cast<double>(tplW) : 0.0;
                        const double nsy = tplH > 0 ? a.y / static_cast<double>(tplH) : 0.0;
                        ResolveFindImageClickPoint(loc, tplW, tplH, nsx, nsy,
                            currentTmplScale(), false, px, py);
                    } else if (a.moveFromVar) {
                        TryResolveIntOperand(a.moveVarExprX, ctx, px);
                        TryResolveIntOperand(a.moveVarExprY, ctx, py);
                    }
                    int r = 0, g = 0, b = 0;
                    HBITMAP fr = lockedScreen_;
                    const bool ok = GetScreenPixelRgb(px, py, r, g, b,
                        fr, lockedVirtX_, lockedVirtY_);
                    const std::wstring varName = a.matchVarName.empty() ? L"colorRet" : a.matchVarName;
                    if (ok) {
                        aiVars_[varName] = FormatColorHex(r, g, b);
                        ImageMatchResult match{};
                        match.found = true;
                        match.x = px;
                        match.y = py;
                        match.topLeftX = px;
                        match.topLeftY = py;
                        match.bottomRightX = px;
                        match.bottomRightY = py;
                        match.score = 100.0;
                        matchVars_[varName] = match;
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(L"获取颜色@" + std::to_wstring(px) + L","
                                + std::to_wstring(py) + L" → " + aiVars_[varName]);
                        }
                    } else {
                        // ★失败必须**写**变量（先例：OCR 取字失败写空串）：
                        //  不写就等于把上一轮的颜色留在变量里 ⇒ `if({colorRet} == "#FF0000")`
                        //  在取色失败时照样为真（循环第 2 圈最容易踩）。
                        aiVars_[varName] = L"";
                        matchVars_[varName] = {};
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(L"获取颜色失败@" + std::to_wstring(px) + L","
                                + std::to_wstring(py) + L" → [" + varName + L"] 已清零");
                        }
                    }
                }
                else if (a.type == ActionType::FindColor) {
                    int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                    HBITMAP colorBmp = lockedScreen_;
                    int colorVx = lockedVirtX_, colorVy = lockedVirtY_;
                    HBITMAP colorTmp = nullptr;
                    const std::wstring varName = a.matchVarName.empty() ? L"colorRet" : a.matchVarName;
                    // ★失败/未命中一律**写**变量（不是「不动」）：不写就等于把上一轮命中的颜色
                    //   留在变量里，`if({colorRet} == "#FF0000")` 在未命中时照样为真。
                    //   同理 `.matchData` 必须落到 0，否则读到的还是上一轮的质量分。
                    auto markColorMiss = [&](const std::wstring& reason) {
                        matchVars_[varName] = {};
                        aiVars_[varName] = L"";
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(L"找色未命中（" + reason + L"）[" + varName + L"] 已清零");
                        }
                    };
                    if (a.imageLocate) {
                        ImageMatchResult loc{};
                        int tplW = 0, tplH = 0;
                        if (!findLocateAnchor(a, loc, tplW, tplH)) {
                            markColorMiss(L"未找到定位图");
                            return;
                        }
                        if (!ApplyImageRegionToMatch(a,
                                loc.topLeftX, loc.topLeftY, loc.bottomRightX, loc.bottomRightY,
                                x1, y1, x2, y2)) {
                            x1 = loc.topLeftX;
                            y1 = loc.topLeftY;
                            x2 = loc.bottomRightX;
                            y2 = loc.bottomRightY;
                        }
                        if (x2 <= x1) x2 = x1 + 1;
                        if (y2 <= y1) y2 = y1 + 1;
                    } else if (wmUsesTarget()) {
                        if (!wmExecPtr->ResolveClientSearchRect(a, x1, y1, x2, y2)) {
                            markColorMiss(L"无法解析目标窗口客户区");
                            return;
                        }
                    } else if (a.searchFullScreen) {
                        int vx = 0, vy = 0, vw = 0, vh = 0;
                        GetVirtualScreenRect(vx, vy, vw, vh);
                        x1 = vx; y1 = vy; x2 = vx + vw; y2 = vy + vh;
                    }
                    if (wmUsesTarget() && !colorBmp) {
                        int ox = 0, oy = 0;
                        if (wmExecPtr->LockWindowCapture(colorTmp, ox, oy)) {
                            colorBmp = colorTmp;
                            colorVx = 0;
                            colorVy = 0;
                        } else {
                            markColorMiss(L"无法截取目标窗口");
                            return;
                        }
                    }
                    const ColorMatchHit hit = FindColorInScreenRegion(
                        x1, y1, x2, y2, a.colorR, a.colorG, a.colorB, a.colorTolerance,
                        colorBmp, colorVx, colorVy, 2, &stopFlag_);
                    if (colorTmp) DeleteBitmapHandle(colorTmp);
                    if (StopRequested()) return;
                    ImageMatchResult match{};
                    if (hit.found) {
                        match.found = true;
                        match.x = hit.x;
                        match.y = hit.y;
                        match.topLeftX = hit.x;
                        match.topLeftY = hit.y;
                        match.bottomRightX = hit.x;
                        match.bottomRightY = hit.y;
                        // 匹配度统一 0~100（色差是单通道最大差 0~255，直接 100-色差会变负）
                        match.score = ColorMatchScorePercent(hit.distance);
                        aiVars_[varName] = FormatColorHex(hit.r, hit.g, hit.b);
                        const int tx = hit.x + a.offsetX;
                        const int ty = hit.y + a.offsetY;
                        if (a.findImageFollowUp == 0) {
                            applyFindCursor(tx, ty, true, a.button, a);
                        } else if (a.findImageFollowUp == 1) {
                            applyFindCursor(tx, ty, false, a.button, a);
                        }
                    } else {
                        aiVars_[varName] = L"";  // 未命中：不留上一轮的颜色
                    }
                    matchVars_[varName] = match;
                    if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(hit.found
                            ? (L"找色命中 " + FormatColorHex(a.colorR, a.colorG, a.colorB)
                                + L" @ " + std::to_wstring(hit.x) + L"," + std::to_wstring(hit.y)
                                + L" 匹配度" + std::to_wstring(ColorMatchScorePercent(hit.distance)))
                            : (L"找色未命中 " + FormatColorHex(a.colorR, a.colorG, a.colorB)
                                + L" [" + varName + L"] 已清零"));
                    }
                }
                else if (a.type == ActionType::ColorMatch) {
                    MacroVariableContext ctx = makeVarCtx();
                    int px = a.x, py = a.y;
                    const std::wstring varName = a.matchVarName.empty() ? L"colorRet" : a.matchVarName;
                    if (a.imageLocate) {
                        ImageMatchResult loc{};
                        int tplW = 0, tplH = 0;
                        if (!findLocateAnchor(a, loc, tplW, tplH)) {
                            matchVars_[varName] = {};
                            aiVars_[varName] = L"";  // 失败必须写，别留上一轮的颜色
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                AppendDebugLog(L"颜色匹配失败（未找到定位图）[" + varName + L"] 已清零");
                            }
                            return;
                        }
                        const double nsx = tplW > 0 ? a.x / static_cast<double>(tplW) : 0.0;
                        const double nsy = tplH > 0 ? a.y / static_cast<double>(tplH) : 0.0;
                        ResolveFindImageClickPoint(loc, tplW, tplH, nsx, nsy,
                            currentTmplScale(), false, px, py);
                    } else if (a.moveFromVar) {
                        TryResolveIntOperand(a.moveVarExprX, ctx, px);
                        TryResolveIntOperand(a.moveVarExprY, ctx, py);
                    }
                    int r = 0, g = 0, b = 0, dist = 0;
                    bool readOk = false;
                    const bool matched = MatchColorAtScreenPoint(px, py,
                        a.colorR, a.colorG, a.colorB, a.colorTolerance,
                        &r, &g, &b, &dist, lockedScreen_, lockedVirtX_, lockedVirtY_, &readOk);
                    if (!readOk) {
                        // 取点失败 ≠ 颜色不匹配：这时 r/g/b 是**没被写过**的 0,0,0，
                        // 写进变量就是「黑色」这个假事实 ⇒ 两个变量都清零。
                        matchVars_[varName] = {};
                        aiVars_[varName] = L"";
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(L"颜色匹配失败（取点失败）@" + std::to_wstring(px) + L","
                                + std::to_wstring(py) + L" [" + varName + L"] 已清零");
                        }
                        return;
                    }
                    ImageMatchResult match{};
                    match.found = matched;
                    match.x = px;
                    match.y = py;
                    match.topLeftX = px;
                    match.topLeftY = py;
                    match.bottomRightX = px;
                    match.bottomRightY = py;
                    // 匹配度统一 0~100（不匹配记 0；匹配时 = 100 - 色差，钳到 0~100）
                    match.score = matched ? static_cast<double>(ColorMatchScorePercent(dist)) : 0.0;
                    matchVars_[varName] = match;
                    // 取点成功 ⇒ 实际颜色是**真事实**：不匹配时也照写（{变量} 的语义就是「该点颜色」）
                    aiVars_[varName] = FormatColorHex(r, g, b);
                    if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(std::wstring(matched ? L"颜色匹配成功" : L"颜色匹配失败")
                            + L" @" + std::to_wstring(px) + L"," + std::to_wstring(py)
                            + L" 实际" + FormatColorHex(r, g, b));
                    }
                }
                else if (a.type == ActionType::AiTextAnalysis) {
                    if (StopRequested()) return;
                    MacroVariableContext ctx = makeVarCtx();
                    const std::wstring resolvedPrompt = ResolveMacroVariables(a.aiPrompt, ctx);
                    const std::wstring outputVarName = a.aiOutputVarName.empty() ? L"aiResult" : a.aiOutputVarName;
                    const std::wstring fallback = a.aiFallbackValue.empty()
                        ? (a.aiOutputType == 1 ? L"0" : L"") : a.aiFallbackValue;
                    const std::wstring modelLabel = EffectiveAiModelName(a);
                    AppendAiDebugLog(L"AI文字分析 [" + modelLabel + L"]：发送 prompt…");
                    try {
                        const AiActionResult ar = RunAiTextAnalysisForAction(
                            a, resolvedPrompt, &aiSessions, aiLoopDepth);
                        if (ar.ok) {
                            StoreAiOutputVar(outputVarName, a.aiOutputType, ar.textResult, fallback);
                            AppendAiDebugLog(L"AI文字分析 [" + modelLabel + L"]：完成 → "
                                + outputVarName + L" = " + aiVars_[outputVarName]);
                        } else {
                            StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                            AppendAiDebugLog(L"AI文字分析 [" + modelLabel + L"]：失败，使用降级值："
                                + fallback + (ar.errorMessage.empty() ? L"" : L" (" + ar.errorMessage + L")"));
                        }
                    } catch (...) {
                        StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                        AppendAiDebugLog(L"AI文字分析 [" + modelLabel + L"]：执行异常，使用降级值：" + fallback);
                    }
                }
                else if (a.type == ActionType::AiImageAnalysis) {
                    if (StopRequested()) return;
                    MacroVariableContext ctx = makeVarCtx();
                    MacroClipboardSnapshot clipSnap;
                    std::vector<std::string> clipExtraJpeg;
                    if (PromptMentionsCtrlClipboard(a.aiPrompt)) {
                        clipSnap = ReadMacroClipboardSnapshot();
                        ctx.clipboardSnapshot = &clipSnap;
                        ctx.clipboardExpandMode = ClipboardExpandMode::Ai;
                        clipExtraJpeg = EncodeClipboardSnapshotImages(clipSnap);
                        if (!clipExtraJpeg.empty()) {
                            AppendAiDebugLog(L"AI图片分析：附加剪贴板图片 "
                                + std::to_wstring(clipExtraJpeg.size()) + L" 张");
                        } else if (clipSnap.hasBitmap || std::any_of(
                            clipSnap.files.begin(), clipSnap.files.end(), LooksLikeImageFilePath)) {
                            AppendAiDebugLog(L"AI图片分析：提示词引用了剪贴板图片，但未能编码附图");
                        }
                    }
                    const std::wstring resolvedPrompt = ResolveMacroVariables(a.aiPrompt, ctx);
                    const std::wstring outputVarName = a.aiOutputVarName.empty() ? L"aiImgResult" : a.aiOutputVarName;
                    const std::wstring fallback = a.aiFallbackValue.empty()
                        ? (a.aiOutputType == 1 ? L"0" : L"") : a.aiFallbackValue;
                    const std::wstring modelLabel = EffectiveAiModelName(a);
                    const double scale = std::clamp(a.aiImageScale, 0.1, 1.0);
                    HBITMAP screenBmp = nullptr;
                    int sw = 0, sh = 0;
                    int capX1 = 0, capY1 = 0, capX2 = 0, capY2 = 0;
                    auto resolveAiRegion = [&](int& x1, int& y1, int& x2, int& y2) -> bool {
                        ScriptAction aiProbe = a;
                        if (a.aiImageUseVar) {
                            aiProbe.aiTargetImagePath = resolveTemplatePath(true, a.aiTargetImagePath);
                            aiProbe.imagePath = aiProbe.aiTargetImagePath;
                            aiProbe.aiImageUseVar = false;
                        }
                        if (wmUsesTarget()) {
                            return wmExecPtr->ResolveAiScreenRect(
                                aiProbe, x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_);
                        }
                        int sx = 0, sy = 0, rsw = 0, rsh = 0;
                        GetVirtualScreenRect(sx, sy, rsw, rsh);
                        int searchX1 = sx, searchY1 = sy, searchX2 = sx + rsw, searchY2 = sy + rsh;
                        if (a.aiSearchX2 > a.aiSearchX1 && a.aiSearchY2 > a.aiSearchY1) {
                            searchX1 = a.aiSearchX1;
                            searchY1 = a.aiSearchY1;
                            searchX2 = a.aiSearchX2;
                            searchY2 = a.aiSearchY2;
                        }
                        if (a.aiRegionByImage && !a.aiTargetImagePath.empty()) {
                            const TemplateScale tmplScale = currentTmplScale();
                            HBITMAP tmpl = LoadBitmapFromFile(aiProbe.aiTargetImagePath);
                            if (!tmpl) return false;
                            ImageMatchOptions opt = BuildExecutionFindImageOptions(a, tmplScale);
                            RestrictFindImageToSingleAnchor(opt);
                            opt.maxOverlap = 0.5;
                            ImageMatchOutput output;
                            if (lockedScreen_) {
                                output = FindTemplateInFrozenScreenMulti(
                                    lockedScreen_, lockedVirtX_, lockedVirtY_,
                                    searchX1, searchY1, searchX2, searchY2, tmpl, opt);
                            } else {
                                output = FindTemplateOnScreenMulti(
                                    searchX1, searchY1, searchX2, searchY2, tmpl, opt);
                            }
                            DeleteBitmapHandle(tmpl);
                            if (output.matches.empty()) return false;
                            const ImageMatchResult& match = output.matches.front();
                            return ApplyImageRegionToMatch(a,
                                match.topLeftX, match.topLeftY,
                                match.bottomRightX, match.bottomRightY,
                                x1, y1, x2, y2);
                        }
                        x1 = searchX1; y1 = searchY1; x2 = searchX2; y2 = searchY2;
                        return x2 > x1 && y2 > y1;
                    };
                    if (!resolveAiRegion(capX1, capY1, capX2, capY2)) {
                        StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                        AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：无法定位分析区域，使用降级值：" + fallback);
                    } else {
                        if (wmUsesTarget()) {
                            screenBmp = wmExecPtr->CaptureScreenRegionFromWindow(
                                capX1, capY1, capX2, capY2,
                                lockedScreen_, lockedVirtX_, lockedVirtY_);
                        } else {
                            screenBmp = CaptureAiRegionComposed(capX1, capY1, capX2, capY2);
                        }
                        if (screenBmp) {
                            BITMAP bm{};
                            if (GetObject(screenBmp, sizeof(bm), &bm)) { sw = bm.bmWidth; sh = bm.bmHeight; }
                        }
                        if (!screenBmp || sw <= 0 || sh <= 0) {
                            StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                            AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：截屏失败，使用降级值：" + fallback);
                        } else {
                            std::wstring imeStatus;
                            if (!wmUsesTarget()) {
                                imeStatus = QueryForegroundImeStatusText();
                                if (imeStatus.find(L"[输入法] 英文") != std::wstring::npos)
                                    imeStatus.clear();
                            }
                            const AiImageEncodeResult encoded = EncodeBitmapForAiAnalysis(
                                screenBmp, scale, 768, imeStatus.empty() ? nullptr : &imeStatus);
                            DeleteBitmapHandle(screenBmp);
                            std::wstring captureInfo = L"AI图片分析 [" + modelLabel + L"]：截屏完成("
                                + std::to_wstring(encoded.srcWidth) + L"×" + std::to_wstring(encoded.srcHeight);
                            if (encoded.effectiveScale < 0.999
                                || encoded.outWidth != encoded.srcWidth
                                || encoded.outHeight != encoded.srcHeight) {
                                captureInfo += L"→" + std::to_wstring(encoded.outWidth)
                                    + L"×" + std::to_wstring(encoded.outHeight);
                            }
                            captureInfo += L")，发送中…";
                            AppendAiDebugLog(captureInfo);

                            if (encoded.base64.empty()) {
                                StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                                AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：图片编码失败，使用降级值：" + fallback);
                            } else {
                                AppendAiDebugLog(L"  图片数据 " + std::to_wstring(encoded.base64.size())
                                    + L" 字节(base64)，调用 API…");
                                try {
                                    const AiActionResult ar = RunAiImageAnalysisForAction(
                                        a, resolvedPrompt, encoded.base64, &aiSessions, aiLoopDepth,
                                        clipExtraJpeg.empty() ? nullptr : &clipExtraJpeg);
                                    if (ar.ok) {
                                        StoreAiOutputVar(outputVarName, a.aiOutputType, ar.textResult, fallback);
                                        AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：完成 → "
                                            + outputVarName + L" = " + aiVars_[outputVarName]);
                                    } else {
                                        StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                                        AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：失败，使用降级值："
                                            + fallback + (ar.errorMessage.empty() ? L"" : L" (" + ar.errorMessage + L")"));
                                    }
                                } catch (...) {
                                    StoreAiOutputVar(outputVarName, a.aiOutputType, L"", fallback);
                                    AppendAiDebugLog(L"AI图片分析 [" + modelLabel + L"]：执行异常，使用降级值：" + fallback);
                                }
                            }
                        }
                    }
                }
                else if (a.type == ActionType::AiActionExecute) {
                    runAiActionExecute(a, nullptr);
                }
            };

            auto executeWithBreakout = [&](const ScriptAction& action) {
                if (workerBreakoutTime_ <= 0) {
                    executeOne(action);
                    return;
                }
                while (!StopRequested() && !scheduledYieldLocalStop) {
                    breakoutUserInput_ = false;
                    executeOne(action);
                    if (StopRequested() || scheduledYieldLocalStop
                        || !breakoutUserInput_.load(std::memory_order_relaxed)) break;
                    waitBreakoutCooldown();
                    // 脱离暂停期间墙钟继续走；精密时间轴若不清零，后续 Wait 会因「已过点」
                    // 瞬间追赶跑完整轮，看起来像跳到脚本末尾，下一轮又从开头开始。
                    // 与注入后端无关（系统模拟 / Interception / VirtualHid 共用此路径）。
                    if (inputTimeline.enabled) {
                        inputTimeline.Reset();
                        timelineInterrupted = false;
                    }
                }
            };

            // ── 编辑器调试闸门（仅调试脚本主序列，嵌套 runRange 不生效）──
            auto debugGate = [&](size_t i) {
                if (!debugMode_.load(std::memory_order_relaxed)
                    || StopRequested() || scheduledYieldLocalStop) return;
                if (debugActionsPtr_ != activeActions) return;
                if (debugStepMode_.load(std::memory_order_relaxed)) {
                    if (!debugStepSignal_.load(std::memory_order_relaxed)) {
                        if (qst::desktop_tools::MacroDebug().IsCreated()) {
                            qst::desktop_tools::MacroDebug().AppendLog(
                                L"调试：等待单步（第 " + std::to_wstring(static_cast<int>(i) + 1)
                                + L" 步，按调试热键执行）");
                        }
                        while (!debugStepSignal_.load(std::memory_order_relaxed)
                            && !StopRequested() && !scheduledYieldLocalStop) {
                            SleepInterruptible(0.02);
                        }
                    }
                    debugStepSignal_.store(false, std::memory_order_relaxed);
                } else {
                    while (debugPaused_.load(std::memory_order_relaxed)
                        && !StopRequested() && !scheduledYieldLocalStop) {
                        SleepInterruptible(0.02);
                    }
                    // 独立播放器的「热键暂停」。产品内恒为 false，走不进这个循环。
                    // 只在动作边界等 —— 单个长 wait 动作不会被打断（与上面调试暂停同一取舍）。
                    while (playbackPaused_.load(std::memory_order_relaxed)
                        && !StopRequested() && !scheduledYieldLocalStop) {
                        SleepInterruptible(0.02);
                    }
                }
            };
            auto debugAfterAction = [&](size_t i) {
                if (!debugMode_.load(std::memory_order_relaxed)
                    || StopRequested() || scheduledYieldLocalStop) return;
                if (debugActionsPtr_ != activeActions) return;
                if (debugBreakpoints_.count(static_cast<int>(i))) {
                    if (qst::desktop_tools::MacroDebug().IsCreated()) {
                        qst::desktop_tools::MacroDebug().AppendLog(
                            L"调试：断点命中第 " + std::to_wstring(static_cast<int>(i) + 1)
                            + L" 步，执行后结束调试");
                    }
                    stopFlag_.store(true, std::memory_order_relaxed);
                }
            };

            runBlockByName = [&](const std::wstring& name) -> RunRangeResult {
                if (StopRequested() || scheduledYieldLocalStop || name.empty()) return RunRangeResult::Normal;
                aiSessions.ClearBlock();
                std::unordered_map<std::wstring, size_t> blockDefs;
                for (size_t i = 0; i < activeActions->size(); ++i) {
                    if ((*activeActions)[i].type == ActionType::DefineBlock && !(*activeActions)[i].blockName.empty()) {
                        blockDefs[(*activeActions)[i].blockName] = i;
                    }
                }
                const auto it = blockDefs.find(name);
                if (it == blockDefs.end()) return RunRangeResult::Normal;
                if (blockCallStack.count(name)) return RunRangeResult::Normal;
                blockCallStack.insert(name);
                const RunRangeResult result = runRange(it->second + 1, containerBodyEnd(it->second));
                blockCallStack.erase(name);
                return result;
            };
            std::function<void()> drainScheduledYield;
            runRange = [&](size_t start, size_t end) -> RunRangeResult {
                auto isDirectLoopBodyRange = [&](size_t loopIdx) {
                    return start == loopIdx + 1 && end == containerBodyEnd(loopIdx);
                };
                auto resolveGotoCursor = [&](size_t targetIdx, size_t& outCursor) {
                    const int outerLoop = OutermostEnclosingLoop(*activeActions, targetIdx);
                    if (outerLoop < 0) {
                        outCursor = targetIdx;
                        return;
                    }
                    if (isDirectLoopBodyRange(static_cast<size_t>(outerLoop))) {
                        outCursor = targetIdx;
                        return;
                    }
                    loopEntryGotoTarget = targetIdx;
                    outCursor = static_cast<size_t>(outerLoop);
                };
                auto consumePendingGoto = [&](size_t scopeStart, size_t scopeEnd, size_t& cursor) -> RunRangeResult {
                    if (!pendingGoto) return RunRangeResult::Normal;
                    const size_t targetIdx = *pendingGoto;
                    pendingGoto.reset();
                    resolveGotoCursor(targetIdx, cursor);
                    if (cursor < scopeStart || cursor >= scopeEnd) return RunRangeResult::GotoPending;
                    return RunRangeResult::Normal;
                };
                for (size_t i = start; i < end && !StopRequested() && !scheduledYieldLocalStop; ) {
                    if (activeActions == &actions) {
                        playbackActionIndex_.store(static_cast<int>(i) + 1, std::memory_order_relaxed);
                        playbackActionTotal_.store(static_cast<int>(actions.size()),
                            std::memory_order_relaxed);
                    }
                    if (drainScheduledYield) drainScheduledYield();
                    if (StopRequested() || scheduledYieldLocalStop) break;
                    if (pendingGoto) {
                        const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                        if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                        continue;
                    }
                    const auto& a = (*activeActions)[i];
                    // ★ 目标闪退取证（见成员注释）：**每个派发出去的动作**都覆盖一次，
                    //   而不是只在某个分支里记 —— 目标消失时"当时正打到第几条"必须准，
                    //   差一条就会把崩溃触发点指到隔壁动作上。
                    //   ⚠ 门控与调试窗同一条件：没开调试窗时一个字符都不格式化。
                    if (KeyFunctionDebugActive()) {
                        playbackLastActionText_ = FormatGenericActionDebug(a);
                    }
                    if (a.type == ActionType::EndLoop) return RunRangeResult::BreakLoop;
                    const bool debugEntryBlock = debugMode_.load(std::memory_order_relaxed)
                        && debugBlockEntrySet_.count(static_cast<int>(i));
                    if (SkipsInMainFlow(a.type)) {
                        if (debugEntryBlock) {
                            // 调试入口所在的顶层跳过容器（定义块/找图监视）：块体按流程执行一次
                            debugGate(i);
                            if (StopRequested() || scheduledYieldLocalStop) {
                                return RunRangeResult::Normal;
                            }
                            const size_t bodyEnd = containerBodyEnd(i);
                            const RunRangeResult bodyResult = runRange(i + 1, bodyEnd);
                            if (bodyResult == RunRangeResult::GotoPending || pendingGoto) {
                                const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                                if (gotoResult == RunRangeResult::GotoPending) {
                                    return RunRangeResult::GotoPending;
                                }
                                continue;
                            }
                            if (bodyResult == RunRangeResult::BreakLoop) {
                                return RunRangeResult::BreakLoop;
                            }
                            debugAfterAction(i);
                            i = bodyEnd;
                            continue;
                        }
                        i = containerBodyEnd(i);
                        continue;
                    }
                    // 结构性动作（Else）不设闸门；可执行动作在闸门等待后仍受停止控制
                    if (a.type != ActionType::Else) {
                        debugGate(i);
                        if (StopRequested() || scheduledYieldLocalStop) {
                            return RunRangeResult::Normal;
                        }
                    }
                    if (a.type == ActionType::Loop) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        const size_t bodyEnd = containerBodyEnd(i);
                        const int thisLoopDepth = aiLoopDepth;
                        ++aiLoopDepth;
                        aiSessions.EnsureLoopDepth(aiLoopDepth);
                        int iter = 1;
                        bool broke = false;
                        const auto loopStartTime = std::chrono::steady_clock::now();
                        while (!StopRequested() && !scheduledYieldLocalStop && !broke) {
                            inputTimeline.Reset(); // 每次循环从零重新对齐录制时间轴
                            aiSessions.ClearLoopAt(thisLoopDepth);
                            if (!a.loopVarName.empty()) loopVars_[a.loopVarName] = iter;
                            MacroVariableContext ctx = makeVarCtx();
                            const int maxLoop = ResolveLoopMaxCount(a, ctx, loopStartTime);
                            if (!(maxLoop < 0 || iter <= maxLoop)) break;
                            size_t runFrom = i + 1;
                            if (iter == 1 && loopEntryGotoTarget) {
                                const size_t entryTarget = *loopEntryGotoTarget;
                                if (entryTarget > i && entryTarget < bodyEnd) {
                                    if (EnclosingChildLoopInBody(*activeActions, i, entryTarget) < 0) {
                                        runFrom = entryTarget;
                                        loopEntryGotoTarget.reset();
                                    }
                                }
                            }
                            const RunRangeResult bodyResult = runRange(runFrom, bodyEnd);
                            if (scheduledYieldLocalStop) broke = true;
                            if (bodyResult == RunRangeResult::BreakLoop) broke = true;
                            else if (bodyResult == RunRangeResult::GotoPending) broke = true;
                            ++iter;
                        }
                        --aiLoopDepth;
                        if (!a.loopVarName.empty()) loopVars_.erase(a.loopVarName);
                        debugAfterAction(i);
                        if (pendingGoto) {
                            const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                            if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                            continue;
                        }
                        i = bodyEnd;
                    } else if (a.type == ActionType::If) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        MacroVariableContext ctx = makeVarCtx();
                        const bool cond = EvaluateConditionExpr(a.conditionExpr, ctx);
                        const int level = a.indent;
                        const size_t trueEnd = containerBodyEnd(i);
                        int elseIdx = -1;
                        for (size_t j = trueEnd; j < activeActions->size(); ++j) {
                            if ((*activeActions)[j].indent < level) break;
                            if ((*activeActions)[j].indent == level && (*activeActions)[j].type == ActionType::If) break;
                            if ((*activeActions)[j].indent == level && (*activeActions)[j].type == ActionType::Else) {
                                elseIdx = static_cast<int>(j);
                                break;
                            }
                        }
                        RunRangeResult branchResult = RunRangeResult::Normal;
                        if (cond) {
                            branchResult = elseIdx >= 0
                                ? runRange(i + 1, static_cast<size_t>(elseIdx))
                                : runRange(i + 1, trueEnd);
                            i = elseIdx >= 0 ? containerBodyEnd(static_cast<size_t>(elseIdx)) : trueEnd;
                        } else if (elseIdx >= 0) {
                            branchResult = runRange(static_cast<size_t>(elseIdx) + 1,
                                containerBodyEnd(static_cast<size_t>(elseIdx)));
                            i = containerBodyEnd(static_cast<size_t>(elseIdx));
                        } else {
                            i = trueEnd;
                        }
                        if (branchResult == RunRangeResult::GotoPending || pendingGoto) {
                            const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                            if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                            continue;
                        }
                        if (branchResult == RunRangeResult::BreakLoop) return RunRangeResult::BreakLoop;
                        debugAfterAction(i);
                    } else if (a.type == ActionType::Else) {
                        i = containerBodyEnd(i);
                    } else if (a.type == ActionType::RunBlock) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        bool runBlockGotoContinue = false;
                        for (int r = 0; r < a.clickCount && !StopRequested() && !scheduledYieldLocalStop; ++r) {
                            const RunRangeResult blockResult = runBlockByName(a.blockName);
                            if (blockResult == RunRangeResult::GotoPending || pendingGoto) {
                                const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                                if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                                runBlockGotoContinue = true;
                                break;
                            }
                            if (sleepRepeatInterval(a, r)) {
                                if (pendingBreakLoop) return RunRangeResult::BreakLoop;
                                const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                                if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                                runBlockGotoContinue = true;
                                break;
                            }
                        }
                        if (runBlockGotoContinue) continue;
                        debugAfterAction(i);
                        ++i;
                    } else if (a.type == ActionType::Goto) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatGenericActionDebug(a));
                        }
                        MacroVariableContext ctx = makeVarCtx();
                        int targetNo = 0;
                        if (TryResolveGotoStepNo(a.gotoStepExpr, ctx, targetNo)) {
                            const size_t targetIdx = FindActionIndexByNo(*activeActions, targetNo);
                            if (targetIdx < activeActions->size()) {
                                if (targetIdx < start || targetIdx >= end) {
                                    pendingGoto = targetIdx;
                                    return RunRangeResult::GotoPending;
                                }
                                resolveGotoCursor(targetIdx, i);
                                debugAfterAction(i);
                                continue;
                            }
                        }
                        debugAfterAction(i);
                        ++i;
                    } else {
                        executeWithBreakout(a);
                        if (pendingBreakLoop) {
                            pendingBreakLoop = false;
                            return RunRangeResult::BreakLoop;
                        }
                        if (pendingGoto) {
                            const RunRangeResult gotoResult = consumePendingGoto(start, end, i);
                            if (gotoResult == RunRangeResult::GotoPending) return RunRangeResult::GotoPending;
                            continue;
                        }
                        debugAfterAction(i);
                        ++i;
                    }
                }
                return RunRangeResult::Normal;
            };
            auto releaseHeldKeys = [&]() {
                if (!heldKeys.empty()) {
                    const bool markSim = !wmUsesTarget();
                    if (markSim) MarkSimulatedInput();
                    for (UINT vk : heldKeys) wmSendKey(vk, false);
                    if (markSim) UnmarkSimulatedInput();
                    heldKeys.clear();
                    heldKeyVk = 0;
                } else if (heldKeyVk != 0) {
                    wmSendKey(heldKeyVk, false);
                    heldKeyVk = 0;
                }
            };
            drainScheduledYield = [&]() {
                if (scheduledYieldDepth > 0 || StopRequested()) return;
                std::wstring path;
                if (!TakeScheduledYieldPath(path)) return;
                if (path.empty()) return;
                if (!runningScriptPath.empty()
                    && _wcsicmp(path.c_str(), runningScriptPath.c_str()) == 0) {
                    AppendDebugLog(L"定时插入改为结束后再跑：与当前脚本相同");
                    DeferScheduledPath(path);
                    return;
                }
                const ScriptFileData nestedData = LoadScriptFileData(path, false);
                if (nestedData.actions.empty()) {
                    AppendDebugLog(L"定时插入失败：无法加载脚本");
                    return;
                }
                releaseHeldKeys();
                CoordMeta nestedMeta = ScriptCoordMetaForExecution(nestedData.coordMeta);
                std::vector<ScriptAction> nested =
                    PrepareScriptActionsForExecution(nestedData.actions, nestedMeta);
                if (IsRecordingScriptPath(path) || ScriptIsTimedInputSequence(nested)
                    || nestedData.inputTimingVersion > 0) {
                    PreparePlaybackTimeline(nested, appSettings_.playback.spreadRelativeMovePackets);
                }
                if (!usesOcr && ScriptUsesTextRecognition(nested)) {
                    usesOcr = true;
                    workerUsesOcrVars_ = true;
                }
                if (usesOcr) holdOcrSession();
                const std::wstring nestedName = [&]() {
                    const auto slash = path.find_last_of(L"\\/");
                    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
                }();
                AppendDebugLog(L"定时插入开始：" + nestedName);
                const std::vector<ScriptAction>* prevActions = activeActions;
                const std::wstring prevPath = runningScriptPath;
                const CoordMeta prevCoordMeta = activeCoordMeta;
                const bool prevTl = inputTimeline.enabled;
                const auto prevPendingGoto = pendingGoto;
                const auto prevLoopEntryGoto = loopEntryGotoTarget;
                const bool prevPendingBreak = pendingBreakLoop;
                const auto prevBlockStack = blockCallStack;
                const int prevAiLoopDepth = aiLoopDepth;
                pendingGoto.reset();
                loopEntryGotoTarget.reset();
                pendingBreakLoop = false;
                blockCallStack.clear();
                aiLoopDepth = 0;
                activeCoordMeta = nestedMeta;
                if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                activeActions = &nested;
                runningScriptPath = path;
                inputTimeline.enabled = false;
                scheduledYieldActive_.store(true, std::memory_order_relaxed);
                ++scheduledYieldDepth;
                scheduledYieldLocalStop = false;
                runRange(0, nested.size());
                --scheduledYieldDepth;
                scheduledYieldLocalStop = false;
                scheduledYieldActive_.store(false, std::memory_order_relaxed);
                pendingGoto = prevPendingGoto;
                loopEntryGotoTarget = prevLoopEntryGoto;
                pendingBreakLoop = prevPendingBreak;
                blockCallStack = prevBlockStack;
                aiLoopDepth = prevAiLoopDepth;
                releaseHeldKeys();
                inputTimeline.enabled = prevTl;
                activeActions = prevActions;
                runningScriptPath = prevPath;
                activeCoordMeta = prevCoordMeta;
                if (wmExecPtr) wmExecPtr->SetCoordMeta(activeCoordMeta);
                AppendDebugLog(StopRequested()
                    ? L"定时插入中止，不再继续原脚本：" + nestedName
                    : L"定时插入结束，继续原脚本：" + nestedName);
            };
            scheduledYieldHook_ = drainScheduledYield;
            // 回放保真度基线：录制时间轴「应有」的总时长（整轮只算一次，避免每轮重编译）。
            // 与实测墙钟对比可判定回放是否被拖慢（低性能模式/机器卡顿/注入过慢）。
            uint64_t expectedTimelineUs = 0;
            if (inputTimeline.enabled) {
                const auto expectedTl = CompileInputTimeline(actions);
                if (!expectedTl.empty()) expectedTimelineUs = expectedTl.back().deadlineUs;
            }
            LARGE_INTEGER loopQpcFreq{};
            QueryPerformanceFrequency(&loopQpcFreq);
            if (debugMode_.load(std::memory_order_relaxed)) {
                // 调试：从指定序号单次执行到最后一个动作，不循环、不受「宏执行次数」影响
                if (debugStepMode_.load(std::memory_order_relaxed)
                    && qst::desktop_tools::MacroDebug().IsCreated()) {
                    qst::desktop_tools::MacroDebug().AppendLog(
                        L"调试开始（单步）：从第 " + std::to_wstring(debugStartIndex_ + 1)
                        + L" 步起，按调试热键单步执行，通用启停热键可终止");
                } else if (qst::desktop_tools::MacroDebug().IsCreated()) {
                    qst::desktop_tools::MacroDebug().AppendLog(
                        L"调试开始（运行）：从第 " + std::to_wstring(debugStartIndex_ + 1)
                        + L" 步起，断点执行后结束，调试热键可暂停/继续");
                }
                // goto 目标在调试起点之前时会向外逃逸：从目标位置重新进入，直到跑完或停止
                size_t debugCursor = static_cast<size_t>(debugStartIndex_);
                while (!StopRequested()) {
                    const RunRangeResult debugResult = runRange(debugCursor, actions.size());
                    if (debugResult != RunRangeResult::GotoPending || !pendingGoto) break;
                    debugCursor = *pendingGoto;
                    pendingGoto.reset();
                    if (debugCursor >= actions.size()) break;
                }
            } else while (!StopRequested()) {
                ++curLoops_;
                inputTimeline.Reset(actions.size());
                timelineInterrupted = false;
                // ★ 目标闪退取证用的两个锚点（见成员注释）：跑这一轮的时刻 + 最后派发的动作。
                playbackRunStartTick_ = GetTickCount64();
                playbackLastActionText_.clear();
                // 每轮独立统计，避免「第2轮 SendInput ok=上轮累计」误导。
                if (inputTimeline.enabled) MouseInputRouter::Instance().ResetStats();
                reqRelDx = 0;
                reqRelDy = 0;
                reqRelPackets = 0;
                aiSessions.ClearMacro();
                aiRootBudget = AiStepBudgetState{};
                aiCurFrame = nullptr;
                if (KeyFunctionDebugActive()) {
                    AppendDebugLog(FormatMacroLoopDebug(curLoops_));
                    if (std::abs(playbackTimeScale - 1.0) > 1e-9) {
                        const double spd = 1.0 / playbackTimeScale;
                        wchar_t buf[128]{};
                        swprintf_s(buf, L"回放倍速 %.3gx（等待/间隔已按倍速缩放）", spd);
                        AppendDebugLog(buf);
                    }
                }
                matchVars_.clear();
                matchListVars_.clear();
                if (usesOcr) ocrVars_.clear();
                loopVars_.clear();
                timerStarts_.clear();
                aiVars_.clear();
                userVars_.clear();
                ClearImageVars(imageVars_);
                pendingGoto.reset();
                loopEntryGotoTarget.reset();
                LARGE_INTEGER loopT0{};
                QueryPerformanceCounter(&loopT0);
                runRange(0, actions.size());
                LARGE_INTEGER loopT1{};
                QueryPerformanceCounter(&loopT1);
                // ⚠ 门控用 autoOutputKeyFunctionDebug 而不是 KeyFunctionDebugActive()：
                // 后者要求「调试窗口已打开」，而 [回放保真] 是判定「偏差在输入层还是目标侧」
                // 的唯一依据、且调试浮窗没有导出按钮 ⇒ 只写窗口等于用户拿不到。故改为
                // 「详细统计仍只进窗口（AppendDebugLog 内部自带门控），保真结论一律落盘」。
                if (inputTimeline.enabled
                    && appSettings_.playback.autoOutputKeyFunctionDebug) {
                    const auto st = inputTimeline.precision.Stats();
                    const auto ms = MouseInputRouter::Instance().Stats();
                    wchar_t summary[512]{};
                    uint64_t spreadBefore = 0, spreadAfter = 0;
                    LastSpreadPacketCounts(spreadBefore, spreadAfter);
                    swprintf_s(summary,
                        L"[时间轴统计] waits=%llu late>1ms=%llu p95=%lluus max=%lluus rebase=%llu | "
                        L"SendInput ok=%llu fail=%llu paced=%llu | "
                        L"ballistics=%s split=%s | spread=%llu->%llu包",
                        static_cast<unsigned long long>(st.eventCount),
                        static_cast<unsigned long long>(st.lateEventCount),
                        static_cast<unsigned long long>(st.p95LateUs),
                        static_cast<unsigned long long>(st.maxLateUs),
                        static_cast<unsigned long long>(st.rebaseCount),
                        static_cast<unsigned long long>(ms.sentEvents),
                        static_cast<unsigned long long>(ms.failedEvents),
                        static_cast<unsigned long long>(ms.pacedWaits),
                        ballisticsGuard.FlatVerified() ? L"flat" : L"accel?",
                        splitLargeMoves ? L"on" : L"off",
                        static_cast<unsigned long long>(spreadBefore),
                        static_cast<unsigned long long>(spreadAfter));
                    AppendDebugLog(summary);
                    // 回放保真度：本轮**实际执行到**的相对位移 vs 实际注入的总量。
                    // 两者相等 ⇒ 输入层忠实，落点偏差在目标侧（帧边界/游戏内非线性），
                    // 别再往注入层查；不等 ⇒ 注入被拆包/失败/拦截改动了位移。
                    // ⚠ 用 reqRel*（执行计数）而不是 SumRelativeMoves(actions)（静态条数）：
                    //    含 Loop/Goto 时两者不等，静态值会把排查引到错误的一侧。
                    const RelativeMoveTotals want{reqRelDx, reqRelDy,
                        static_cast<size_t>(reqRelPackets)};
                    // 静态条数只在「确实不同」时提一句，说明这脚本有循环/分支，
                    // 免得看的人以为「同一脚本每轮包数怎么会变」。
                    const RelativeMoveTotals staticTotals =
                        activeActions ? SumRelativeMoves(*activeActions)
                                      : RelativeMoveTotals{};
                    wchar_t loopNote[96]{};
                    if (staticTotals.packets != want.packets) {
                        swprintf_s(loopNote, L"（脚本静态 %llu 包，本轮执行 %llu 次）",
                            static_cast<unsigned long long>(staticTotals.packets),
                            static_cast<unsigned long long>(want.packets));
                    }
                    const uint64_t actualUs = (loopQpcFreq.QuadPart > 0)
                        ? static_cast<uint64_t>(
                            (static_cast<long double>(loopT1.QuadPart - loopT0.QuadPart)
                                * 1000000.0L) / loopQpcFreq.QuadPart)
                        : 0;
                    // 注入侧统计只覆盖 SendInput 路径（`MouseInputRouter`）。窗口/后台窗口模式下
                    // 若走 PostMessage/软输入（**假焦点注入成功**、模拟器）或 CDP，位移
                    // 不经该计数器 —— 此时必须明说「未统计」，否则会拿 0 去比非 0，
                    // 在一切正常时报出「⚠ 位移不一致」，把排查引到错误的一侧。
                    // ⚠ `PreferHardwareInput()` 里 `if (FakeFocusActive()) return false;`
                    //   排在游戏判据之前 ⇒ MC 这类「假焦点注入成功」的目标正是走软输入。
                    const bool injectedCounted =
                        !wmUsesTarget() || wmExecPtr->PreferHardwareInput();
                    wchar_t injectPart[192]{};
                    const wchar_t* verdict = L"";
                    if (!injectedCounted) {
                        swprintf_s(injectPart,
                            L"注入=未统计（窗口/后台窗口模式软输入/CDP 不经 SendInput 计数器）");
                    } else {
                        swprintf_s(injectPart, L"注入=(%lld,%lld)/%llu事件",
                            ms.movedDx, ms.movedDy,
                            static_cast<unsigned long long>(ms.sentEvents));
                    }
                    switch (EvaluateMoveFidelity(injectedCounted,
                                want.dx, want.dy, ms.movedDx, ms.movedDy)) {
                    case MoveFidelityVerdict::NotCounted:
                        verdict = L"⇒ 位移未统计（此模式无法判定，别按「不一致」处理）";
                        break;
                    case MoveFidelityVerdict::Match:
                        verdict = L"⇒ 位移一致（偏差在目标侧）";
                        break;
                    case MoveFidelityVerdict::Mismatch:
                    default:
                        verdict = L"⚠ 位移不一致（注入层改动了位移，先查拆包/失败）";
                        break;
                    }
                    wchar_t fidelity[560]{};
                    swprintf_s(fidelity,
                        L"[回放保真] 相对位移 请求=(%lld,%lld)/%llu包%s %s %s | "
                        L"时间轴 预期=%llums 实际=%llums%s",
                        want.dx, want.dy,
                        static_cast<unsigned long long>(want.packets),
                        loopNote,
                        injectPart, verdict,
                        static_cast<unsigned long long>(expectedTimelineUs / 1000),
                        static_cast<unsigned long long>(actualUs / 1000),
                        (expectedTimelineUs > 0 && actualUs > expectedTimelineUs * 6 / 5
                            && actualUs - expectedTimelineUs > 20000)
                            ? L" ⚠ 回放被拖慢（检查低性能模式/机器卡顿/注入耗时）"
                            : L"");
                    qst::desktop_tools::AppendRecorderDiagLog(fidelity);
                    AppendDebugLog(fidelity);
                    if (timelineInterrupted || StopRequested()) {
                        const std::wstring tail = StopRequested()
                            ? L"[时间轴] 本轮未跑完：已停止"
                            : L"[时间轴] 本轮未跑完：等待被跳出打断";
                        qst::desktop_tools::AppendRecorderDiagLog(tail);
                        AppendDebugLog(tail);
                    }
                    // 不在轮间 Flush：1744 行格式化会卡住工作线程数百 ms～数秒，
                    // 多轮 FPS 回放时游戏状态已漂。完整日志在全部结束后一次刷出。
                }
                // 轮间抬起残留按键，避免无限循环时上一轮没松开的键拖进下一轮。
                releaseHeldKeys();
                const auto& ps = appSettings_.playback;
                if (ps.enablePlaybackCount && ps.playbackCount > 0 && curLoops_ >= ps.playbackCount) break;
                if (StopRequested()) break;
                if (ps.enablePlaybackInterval) {
                    const double span = std::max(0.0, ps.playbackIntervalMaxSeconds - ps.playbackIntervalMinSeconds);
                    const double wait = ps.playbackIntervalMinSeconds + RandomDelay(span);
                    SleepInterruptible(wait);
                }
            }
            // Final release (for clean state regardless of stop path)
            releaseHeldKeys();
            ReleaseAllHeldInputs();
            scheduledYieldHook_ = nullptr;
            scheduledYieldActive_.store(false, std::memory_order_relaxed);
            clearLockedScreen();
            ClearImageVars(imageVars_);
            if (ocrSessionHeld) ReleaseOcrSession();
            if (wmCfg.enabled) wmExec.EndRun();
            ForegroundInputRouter::Instance().EndSession();
            } // MouseBallisticsGuard / 优先级：须在 PostMessage(WM_RUN_DONE) 前恢复
            breakout_input::UninstallBreakoutHooks();
            workerUsesOcrVars_ = false;
            // 脚本自然结束 / 热键终止后的最终写回（AI 段曾因路径空推迟）
            if (AiLogicConvertPendingWriteback() || AiLogicConvertSessionActive()) {
                std::wstring summary, err;
                if (TryFlushAiLogicConvertSession(selfPath, summary, err)) {
                    AppendAiDebugLog(L"逻辑转化：已写回脚本 → " + summary + L"（脚本结束）");
                    NotifyLogicConvertUi(selfPath, summary);
                    NotifyAgentScriptLibraryChanged();
                    if (hwnd_ && !selfPath.empty()
                        && _wcsicmp(selfPath.c_str(), currentPath_.c_str()) == 0) {
                        PostMessageW(hwnd_, WM_APP_LOGIC_CONVERT_DONE, 0, 0);
                    }
                } else if (AiLogicConvertPendingWriteback() && !err.empty()) {
                    AppendAiDebugLog(L"逻辑转化：脚本结束时写回失败 — " + err);
                }
                AiLogicConvertSessionEnd();
            }
            // 先刷调试缓冲再通知 UI 结束：否则下一轮回放会和上万行格式化抢 CPU，时间轴追赶连发。
            deferDbgGuard.DisarmNow();
            workerFinished_.store(true, std::memory_order_relaxed);
            PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
        });
    }

// was engine_host_window.h:14250-14300
void EngineHost::RestoreMainWindowAfterRun() {
        if (!hwnd_) return;
        SetWindowCloaked(hwnd_, false);
        breakoutTaskbarShown_ = false;
        breakoutUiVisibleOnScreen_ = false;

        if (headlessUi_) {
            // 引擎宿主始终隐藏；只恢复/保持 Web 壳可见性（对齐 autoHide / 关闭到托盘）
            ShowWindow(hwnd_, SW_HIDE);
            HWND face = (webViewHostHwnd_ && IsWindow(webViewHostHwnd_)) ? webViewHostHwnd_ : nullptr;
            if (face) {
                if (!wasVisibleBeforeRun_) {
                    // 开始前已关闭到托盘（不可见）：结束后保持隐藏，避免与 closeToTray 互踩
                    ShowWindow(face, SW_HIDE);
                } else if (wasMinimizedBeforeRun_) {
                    ShowWindow(face, SW_SHOWMINNOACTIVE);
                } else {
                    ShowWindow(face, SW_SHOWNOACTIVATE);
                }
            }
            breakoutPlacement_ = BreakoutTaskbarPlacement{};
            runSavedRectValid_ = false;
            return;
        }

        const LONG_PTR ex = runSavedRectValid_ ? runSavedExStyle_
            : (breakoutPlacement_.saved ? breakoutPlacement_.exStyle
                : GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);

        RECT rc{};
        if (runSavedRectValid_) rc = runSavedRect_;
        else if (breakoutPlacement_.saved) rc = breakoutPlacement_.rect;
        else GetWindowRect(hwnd_, &rc);

        const int w = std::max(static_cast<int>(rc.right - rc.left), 1);
        const int h = std::max(static_cast<int>(rc.bottom - rc.top), 1);

        if (!wasVisibleBeforeRun_) {
            ShowWindow(hwnd_, SW_HIDE);
        } else if (wasMinimizedBeforeRun_) {
            breakoutTaskbarTransition_ = true;
            if (IsIconic(hwnd_)) {
                ShowWindow(hwnd_, SW_RESTORE);
            }
            SetWindowPos(hwnd_, HWND_BOTTOM, rc.left, rc.top, w, h,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            PrepareHwndTaskbarLivePreview(hwnd_);
            DwmFlush();
            ShowWindow(hwnd_, SW_MINIMIZE);
            TaskbarYieldMessages();
            breakoutTaskbarTransition_ = false;
        } else {
            // 自动隐藏后恢复：进任务栏但不抢焦点，避免打断用户当前操作
            SetWindowPos(hwnd_, HWND_BOTTOM, rc.left, rc.top, w, h,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
            PrepareHwndTaskbarLivePreview(hwnd_);
        }

        breakoutPlacement_ = BreakoutTaskbarPlacement{};
        runSavedRectValid_ = false;
    }

// was engine_host_window.h:14302-14342
void EngineHost::OnRunDone() {
        // 热键看门狗与 WM_RUN_DONE 可能各走一遍；只在仍标记运行时收尾，避免结束音连响两次。
        if (!running_) return;
        running_ = false;
        if (appSettings_.other.playSoundOnEnd) PlayAppFinishSound();
        runningFromScheduled_ = false;
        nextRunFromScheduled_ = false;
        scheduledInterruptStop_ = false;
        scheduledYieldActive_.store(false, std::memory_order_relaxed);
        {
            std::wstring leftover;
            if (TakeScheduledYieldPath(leftover)) EnqueueScheduledPath(leftover);
            leftover = TakeDeferredScheduledPath();
            if (!leftover.empty()) EnqueueScheduledPath(leftover);
        }
        // 调试会话收尾：注销调试热键、清空断点/暂停状态（仅缓存，不持久化）
        if (debugMode_.exchange(false, std::memory_order_relaxed)) {
            if (hwnd_ && IsWindow(hwnd_)) {
                UnregisterHotKey(hwnd_, HOTKEY_DEBUG_ID);
            }
            debugStepMode_.store(false, std::memory_order_relaxed);
            debugPaused_.store(false, std::memory_order_relaxed);
            playbackPaused_.store(false, std::memory_order_relaxed);
            debugStepSignal_.store(false, std::memory_order_relaxed);
            debugBreakpoints_.clear();
            debugActionsPtr_ = nullptr;
            debugBlockEntrySet_.clear();
            debugHotkeyVk_ = 0;
            debugHotkeyMods_ = 0;
            if (debugRestoreUiMute_) {
                // 仅当壳仍停在宏编辑/录制优化时才恢复静音；已回主页则丢弃，否则 F8/录制热键全哑
                debugRestoreUiMute_ = false;
                if (shellUiMode_ == 1 || shellUiMode_ == 2) {
                    ghUiModeHotkeysMuted.store(true, std::memory_order_relaxed);
                }
            }
        }
        // 复位脚本取消闩锁，避免残留 true 误伤其它会话（连点已不再读此标志）。
        ghWorkerCancelFlag = nullptr;
        stopFlag_ = false;
        ghEmergencyStop.store(false, std::memory_order_release);
        ClearAllHoldSessionLatches();
        extRunPending_ = false;
        ForegroundInputRouter::Instance().EndSession();
        ResumeHotkeysAfterPlayback();
        {
            std::lock_guard<std::mutex> lock(extScriptStateMu_);
            runningScriptPath_.clear();
            runningScriptName_.clear();
            runningWindowMode_ = windowmode::DefaultWindowModeConfig();
        }
        windowmode::SetWindowModeLogSink(nullptr);
        executedSteps_.store(0, std::memory_order_relaxed);
        playbackActionIndex_.store(0, std::memory_order_relaxed);
        playbackActionTotal_.store(0, std::memory_order_relaxed);
        ghHotkeySessionBusy.store(recording_ || clicking_, std::memory_order_relaxed);
        EnsureHotkeyAuxTimers();
        ClearToggleHotkeyLatches();
        EndHighResTimer();
        breakout_input::UninstallBreakoutHooks();
        breakoutHookState_ = BreakoutHookState{};
        workerBreakoutTime_ = 0;
        breakoutUserInput_ = false;
        breakoutPaused_ = false;
        if (worker_.joinable()) worker_.detach();
        if (hwnd_ && IsWindow(hwnd_)) {
            SetWindowCloaked(hwnd_, false);
            BOOL off = FALSE;
            DwmSetWindowAttribute(hwnd_, DWMWA_FORCE_ICONIC_REPRESENTATION, &off, sizeof(off));
            DwmSetWindowAttribute(hwnd_, DWMWA_FREEZE_REPRESENTATION, &off, sizeof(off));
        }
        ApplyMainWindowNormalTaskbarPresentation(hwnd_);
        RestoreMainWindowAfterRun();
        if (hwnd_ && IsWindow(hwnd_) && !wasMinimizedBeforeRun_) {
            if (IsWindowVisible(hwnd_)) {
                ApplyMainWindowNormalTaskbarPresentation(hwnd_);
                RestoreWindowTaskbarLivePreview(hwnd_);
                // 预览/FRAMECHANGED 可能抬升 Z 序：再沉底且不激活
                SetWindowPos(hwnd_, HWND_BOTTOM, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            }
        }
        EnsureTrayIcon();
        HideStatusTip();
        if (hwnd_ && IsWindow(hwnd_) && !pendingScheduledPaths_.empty()) {
            PostMessageW(hwnd_, WM_APP_RUN_SCHEDULED_PENDING, 0, 0);
        }
    }
