// engine_script_run.cpp — F2 slice extracted from engine_host_window.h
#include "engine/engine_host_window.h"
#include "action_utils.h"
#include "agent_ui_notify.h"
#include "ai_action_service.h"
#include "ai_locate_cache.h"
#include "ai_fast_paths.h"
#include "ai_ui_layout.h"
#include "ai_locate_verify.h"
#include "ai_logic_convert.h"
#include "color_match.h"
#include "desktop_tools/desktop_tools.h"
#include "image_var_util.h"
#include "low_power_mode.h"
#include "macro_execute_tools.h"
#include "ocr_engine.h"
#include "script_io.h"
#include "var_compute.h"
#include "window_mode/ui_element_probe.h"
#include "window_mode/window_list.h"
#include "window_mode/window_capture.h"
#include "window_mode/window_capture_wgc.h"
#include "page_snapshot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

/// 日志用短截断（本 TU 内自用；只影响调试输出）
static std::wstring TruncLog(const std::wstring& s, size_t n) {
    return s.size() <= n ? s : (s.substr(0, n) + L"…");
}

static std::wstring ResolveNestedLibraryTarget(const std::wstring& targetPath,
    const std::wstring& blockName) {    std::wstring resolved;
    if (!targetPath.empty() && ResolveLibraryScriptPath(targetPath, resolved))
        return resolved;
    if (!blockName.empty() && ResolveLibraryScriptPath(blockName, resolved))
        return resolved;
    return {};
}

// ── 找图「上一帧命中」本地复核：跨动作存活的极小状态表 ──────────────────
// 判据（纯函数）在 image_match.h 的 PlanFindImageFastPath / AcceptFindImageFastPathHit，
// 这里只负责「按请求指纹存/取上一次全屏命中的结果」。
// 指纹包含模板路径、搜索区、阈值/尺度、模板尺寸、目标分辨率 —— 任一变化即视为新请求。
// **每次开始跑脚本都清空**：绝不让上一次运行的命中影响到这一次。
namespace {

struct FindImageFastPathEntry {
    int prevTLX = 0;
    int prevTLY = 0;
    int tplW = 0;
    int tplH = 0;
    double lastFullSearchMs = 0.0;
    std::chrono::steady_clock::time_point at{};
};

std::unordered_map<std::wstring, FindImageFastPathEntry> g_findImageFastPath;
constexpr size_t kFindImageFastPathMaxEntries = 8;

void ResetFindImageFastPath() {
    g_findImageFastPath.clear();
}

std::wstring FindImageFastPathKey(const std::wstring& tplPath, const ImageMatchOptions& opt,
    int x1, int y1, int x2, int y2, int tplW, int tplH, int targetW, int targetH) {
    wchar_t buf[256]{};
    swprintf_s(buf, L"|%d,%d,%d,%d|%d,%d|%.1f|%.4f,%.4f,%.4f|%d,%d|%d,%d",
        x1, y1, x2, y2, tplW, tplH, opt.thresholdPercent,
        opt.scaleMin, opt.scaleMax, opt.scaleStep, targetW, targetH,
        opt.perfectMatch ? 1 : 0, opt.crossResolutionMatch ? 1 : 0);
    return tplPath + buf;
}

}  // namespace

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

// ── 「一次性目标」判定：本次运行里这个目标被定位过几次 ───────────────────
// 用户实测反馈：**不是每个按钮都值得抽象出可复用定位** —— 很多按钮只点一次。
// 复用缓存（定位模板 + 布局记忆）只对「反复点的位置」有意义：
//  · 给一次性按钮存缓存没有收益（下次根本不再点它）；
//  · 却要付出真金白银（布局记忆要整屏转灰度、算 8×8 外观签名）；
//  · 而且会把**一次性的**坐标固化成「下次直接用」——实测「一键全选」那次就是这么点错的
//    （面板已关，记忆命中把点击打到了别的地方）。
// 所以改成：同一目标**第二次**被定位成功后才写缓存/记忆（第一次只定位，不记）。
namespace {

std::unordered_map<std::wstring, int> g_locateTargetSeen;

int NoteLocateTargetSeen(const std::wstring& targetDesc) {
    const std::wstring key = Trim(targetDesc);
    if (key.empty()) return 0;
    int& n = g_locateTargetSeen[key];
    if (n < 1000000) ++n;
    return n;
}

void ResetLocateTargetSeen() {
    g_locateTargetSeen.clear();
}

/// 错点自纠开关（备用项）：默认开；`QST_NO_MISS_SELFCORRECT=1` 关掉。
bool AiMissSelfCorrectEnabled() {
    static const bool enabled = []() {
        wchar_t buf[8]{};
        return GetEnvironmentVariableW(L"QST_NO_MISS_SELFCORRECT", buf, 8) == 0;
    }();
    return enabled;
}

/// 「屏幕像素(x,y)＝归一化(nx,ny)」：模型自己的坐标系是 0~1000（computer/mouseClick 都是），
/// 只告诉它屏幕像素时它会反复自问「这是屏幕还是图上的坐标」白烧轮次（实测第十二份日志）。
std::wstring DescribeClickPointForModel(int x, int y) {
    const int sw = (std::max)(1, GetSystemMetrics(SM_CXSCREEN));
    const int sh = (std::max)(1, GetSystemMetrics(SM_CYSCREEN));
    return L"屏幕像素(" + std::to_wstring(x) + L"," + std::to_wstring(y) + L")＝归一化("
        + std::to_wstring(x * 1000 / sw) + L"," + std::to_wstring(y * 1000 / sh)
        + L")（0~1000，与 computer/mouseClick 同一套）";
}

/// 明确告诉宿主「这个目标会被反复点」（网格锚点）：允许它写复用缓存。
/// 网格锚点每次 grid 调用都要用，属于典型的可复用目标；只靠「第二次才存」
/// 会让第 2 次 grid 调用又走一遍识图并**再点一次锚点格**（那是一次多余点击）。
void MarkLocateTargetReusable(const std::wstring& targetDesc) {
    const std::wstring key = Trim(targetDesc);
    if (key.empty()) return;
    int& n = g_locateTargetSeen[key];
    if (n < 1) n = 1;   // 紧接着的这次定位自增后即为 2 → 可复用
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
                promptModal_.ShowInfo(err.empty() ? L"窗口模式未就绪" : err);
                return;
            }
        }

        CoordMeta execMeta = ScriptCoordMetaForExecution(loadedCoordMeta_);
        std::vector<ScriptAction> execActions = actions_;
        SyncNormFieldsFromPixels(execActions,
            CaptureCurrentCoordMeta(runCfg.enabled ? &runCfg : nullptr));
        execActions = PrepareScriptActionsForExecution(execActions, execMeta);
        // 旧录制里相对移动间隔可能被 Raw 积压压成 0~1ms；回放前按设备报告间隔拉开。
        if (IsRecordingScriptPath(currentPath_) || ScriptIsTimedInputSequence(execActions))
            RepairCompressedRelativeGaps(execActions);

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
        execActions = PrepareScriptActionsForExecution(execActions, execMeta);
        if (IsRecordingScriptPath(displayName) || ScriptIsTimedInputSequence(execActions)) {
            RepairCompressedRelativeGaps(execActions);
        }
        windowmode::WindowModeScriptConfig wmCfg = wmCfgIn;
        bool anyRel = wmCfg.windowRelativeCoordinates;
        for (const auto& a : execActions) {
            if (a.windowRelative) { anyRel = true; break; }
        }
        windowmode::FinalizeWindowModeForPlayback(wmCfg, anyRel, false);
        if (!ResolveWindowModeSelectMethod(wmCfg)) {
            RestoreMainWindowForUser();
            err = L"窗口模式未能绑定目标窗口";
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
                L"[窗口模式] 本次运行解析后配置：enabled=%d executionKind=%s targetExe=%ls autoLaunch=%d selectMethod=%d",
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
            // 窗口模式假前台 SendInput 必须在 UI 线程先把壳藏掉，否则工作线程
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
            // 扩展桥常开：未开窗口模式时勿把桥心跳灌进宏调试窗（与 AI/默认宏无关）
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
        if (qst::desktop_tools::MacroDebug().IsCreated()) qst::desktop_tools::MacroDebug().ClearLog();
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
                        ? L"[窗口模式] 后台窗口模式：工作线程已启动"
                        : L"[窗口模式] 窗口模式：工作线程已启动");
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
            // 错点自纠预算（每次运行几次）：点下去没反应时，用「刚点的错点」当红叉锚点补一次点。
            // 备用项，不是主链路；用完就老实回主模型。
            int missSelfCorrectBudget = 3;
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
            wmExec.SetInjectionTechnique(
                windowmode::inject::TechniqueFromInt(
                    appSettings_.windowMode.injectionTechnique));
            wmExec.SetHideInjectedModule(
                appSettings_.windowMode.hideInjectedModule);

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
                            ? L"窗口模式启动失败" : wmErr;
                        // 先结束运行并恢复主窗口，再弹提示，避免遮罩坐标错位导致「确定」点不到。
                        PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
                        PostMessageW(hwnd_, WM_APP_PROMPT, 0, 0);
                    } else {
                        PostMessageW(hwnd_, WM_RUN_DONE, 0, 0);
                    }
                    return;
                }
                wmExec.SetCoordMeta(activeCoordMeta);
                windowmode::WindowModeLog(L"[窗口模式] 已绑定目标，开始运行");
                windowmode::WindowModeLogDesktopSnap(L"绑定后", wmExec.TargetHwnd());
                if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                    HWND th = wmExec.TargetHwnd();
                    wchar_t cls[128]{};
                    if (th) GetClassNameW(th, cls, 128);
                    wchar_t buf[192]{};
                    swprintf_s(buf, L"窗口模式已绑定 hwnd=0x%p class=%s%s",
                        th, cls,
                        wmExec.UsesBackgroundWindow() ? L" [后台]"
                            : (wmExec.IsCdpInputMode() ? L" [鼠标宏·扩展]" : L" [鼠标宏桌面]"));
                    AppendDebugLog(buf);
                }
            } else if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                bool anyRel = wmCfg.windowRelativeCoordinates;
                for (const auto& act : actions) {
                    if (act.windowRelative) { anyRel = true; break; }
                }
                if (anyRel) {
                    AppendDebugLog(
                        L"窗口模式未启用：脚本含窗口相对坐标/找图，将误走全屏桌面"
                        L"（游戏常见匹配度约 50% 失败）。请用窗口模式+图片定位重新录制。");
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

            runAiActionExecute = [this, &usesOcr, &holdOcrSession, &ocrVerifyBudget, &missSelfCorrectBudget, &heldKeyVk, &runRange, &runningScriptPath,
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
                bool lastUiSettleSettled = false;
                std::wstring lastUiChangeRoisText;
                std::vector<ScreenChangeRoi> lastUiChangeRois;
                /// settle 刚写入 aiObs 后，下一轮观察必须上传，避免被「未变」短路
                bool forceNextObserveUpload = false;
                /// 最近一次定位/点击的屏幕坐标（动作局部验收，抑制视频区抢注意力）
                int lastActionScreenX = -1;
                int lastActionScreenY = -1;
                int nearDupClickCount = 0;
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
                auto notePointerClick = [&](int sx, int sy, bool isPrimaryLeft) -> std::wstring {
                    if (sx < 0 || sy < 0) return {};
                    const std::wstring fgTitle = foregroundTitle();
                    if (!lastPointerClickFgTitle.empty() && fgTitle != lastPointerClickFgTitle) {
                        nearDupClickCount = 0;
                        lastActionScreenX = -1;
                        lastActionScreenY = -1;
                    }
                    lastPointerClickFgTitle = fgTitle;
                    // 右键菜单常需同点再试/点菜单项，勿用近点拒绝误伤
                    if (!isPrimaryLeft) {
                        lastActionScreenX = sx;
                        lastActionScreenY = sy;
                        nearDupClickCount = 0;
                        return {};
                    }
                    if (lastActionScreenX >= 0 && lastActionScreenY >= 0) {
                        const long long dx = static_cast<long long>(sx) - lastActionScreenX;
                        const long long dy = static_cast<long long>(sy) - lastActionScreenY;
                        if (dx * dx + dy * dy <= 56LL * 56LL) {
                            ++nearDupClickCount;
                            if (nearDupClickCount >= 2) {
                                return L"[错误] 已连续在相近位置左键点击（同一屏重复点）。"
                                    L"若刚跳转了页面，请对「新页面」上的目标重新定位。"
                                    L"若目标态已达成（选中/点赞/按下）请 completeTask；"
                                    L"未变则 observePage 看 checked/pressed，勿对同一坐标连点。"
                                    L"切窗用 activateWindow；打开桌面图标请 doubleClick=true。";
                            }
                        } else {
                            nearDupClickCount = 1;
                        }
                    } else {
                        nearDupClickCount = 1;
                    }
                    lastActionScreenX = sx;
                    lastActionScreenY = sy;
                    return {};
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
                        // 只有这里才是真的「JSON 不合法」——报错必须带上原因，
                        // 否则模型只会看到「JSON 解析失败」而不知道错在哪（实测白烧两轮）。
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：动作 JSON 解析失败："
                            + FromUtf8(std::string(e.what())));
                        return L"[错误] 动作 JSON 解析失败：" + FromUtf8(std::string(e.what()))
                            + L"。请重新生成动作 JSON（勿手写残缺字段）。";
                    }
                    try {
                        if (!steps.is_array())
                            return L"[错误] 返回内容不是 JSON 数组";
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：即时执行本批 "
                            + std::to_wstring(steps.size()) + L" 个动作");
                        bool settleNoReactionThisBatch = false;

                        // 截图坐标系 → 屏幕坐标（与 CompositeClick 一致；缩放/选区未映射会点偏）
                        const bool remapApi = liveMapValid
                            && liveMap.apiWidth > 0 && liveMap.apiHeight > 0
                            && liveMap.capX2 > liveMap.capX1 && liveMap.capY2 > liveMap.capY1;
                        // false = 应跳过本步（坐标无法解释，禁止放大飞点）
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
                            if (!ResolveAgentPointerToApiImage(apiX, apiY,
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
                            return true;
                        };

                        int stepCount = 0;
                        int skippedBadPointer = 0;
                        int skippedInvalid = 0;
                        std::wstring firstInvalidError;
                        // 近点重复点击被拒 ≠ 坐标越界，回给模型的理由必须分开
                        std::wstring skippedDupNote;
                        // ★逐步生效校验（通用）：盲批量里「先选中/切换 → 再作用」这类依赖链，
                        //   第一步没生效后面全是空转；而动态画面上的整批 settle 只会说「仍在变化」，
                        //   看不出第一步其实没点上。这里记下**第一次点击的落点**（点完光标就在那儿）
                        //   与点击步数，稍后用 settle 的变化区判断它到底有没有引起局部变化。
                        int firstClickScreenX = -1;
                        int firstClickScreenY = -1;
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
                        for (size_t stepIdx = 0; stepIdx < steps.size(); ++stepIdx) {
                            const auto& step = steps[stepIdx];
                            if (StopRequested()) break;
                            if (!step.is_object()) continue;

                            nlohmann::json params;
                            std::wstring actionType;
                            if (step.contains("action")) {
                                actionType = FromUtf8(step["action"].get<std::string>());
                                if (actionType == L"mouseMove") actionType = L"moveMouse";
                                params = step.value("params", nlohmann::json::object());
                                if (!params.is_object()) params = nlohmann::json::object();
                                params["type"] = ToUtf8(actionType);
                            } else if (step.contains("type")) {
                                params = step;
                                actionType = FromUtf8(step["type"].get<std::string>());
                            } else {
                                continue;
                            }

                            // Agent 闭环：stopMacro 不占步骤预算（构建器常自动追加，否则 10 步很快耗尽）
                            if (actionType == L"stopMacro") continue;

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
                                    // locateAndClick 已带 coordSpace=screen 并做过近点校验；此处只拦 Agent 盲点
                                    const bool fromLocate =
                                        params.contains("coordSpace") && params["coordSpace"].is_string()
                                        && params["coordSpace"].get<std::string>() == "screen";
                                    std::string btn = "left";
                                    if (params.contains("button") && params["button"].is_string())
                                        btn = params["button"].get<std::string>();
                                    const bool primaryLeft = (btn == "left" || btn.empty());
                                    if (!fromLocate) {
                                        if (const std::wstring dup = notePointerClick(sx, sy, primaryLeft);
                                            !dup.empty()) {
                                            AppendAiDebugLog(L"  " + dup);
                                            if (skippedDupNote.empty()) skippedDupNote = dup;
                                            continue;
                                        }
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

                            AppendAiDebugLog(L"  执行 " + ActionName(stepAction)
                                + L" (步" + std::to_wstring(stepCount + 1) + L")");
                            // 开网页/启动程序：操作前截基线，操作后本地「反应→稳定」二次校验
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
                                const bool gameForeground = AiActionGameForegroundLikely();
                                if (gameForeground) {
                                    sopt.pollIntervalMs = 80;
                                    sopt.reactDeadlineMs = 350;
                                    sopt.stableHoldMs = 150;
                                    sopt.maxTotalMs = 900;
                                    sopt.refreshSuggestMs = 100000;  // 游戏里别提「建议刷新/重开」
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
                                lastUiSettleSettled = settled.settled;
                                // 权威的「点完到底有没有反应」：给「错点自纠」这类补点逻辑看
                                NoteAiUiSettleReacted(settled.reacted);
                                if (!settled.reacted)
                                    settleNoReactionThisBatch = true;
                                // 播放器大面积在动不算翻页：勿清近点计数，否则会再点一次把开关取消。
                                lastUiChangeRois = settled.lastChangeRois;
                                // ★逐步生效判定：本批是「多点」时，第一次点击有没有在它附近
                                //   引起**小范围结构变化**（大面积动态区不算，那是播放器/游戏背景）。
                                //   没有 → 明说「第一步很可能没生效」，并提醒后续步骤在空转。
                                //   这条对任何界面都成立：先选中/先切换没成功，后面点什么都是白点。
                                if (clickedSteps >= 2 && firstClickScreenX >= 0) {
                                    const int lx = firstClickScreenX - cx1;
                                    const int ly = firstClickScreenY - cy1;
                                    // 变量名勿用 near/far/small（Windows 旧头历史宏：#define small char）
                                    bool nearHit = false;
                                    bool sawSmallRoi = false;
                                    for (const auto& rr : settled.lastChangeRois) {
                                        const int rw = rr.x2 - rr.x1;
                                        const int rh = rr.y2 - rr.y1;
                                        const bool roiSmall = rw > 0 && rh > 0 && rw <= 220 && rh <= 180
                                            && rw * rh <= 48000;
                                        if (!roiSmall) continue;
                                        sawSmallRoi = true;
                                        if (lx >= rr.x1 - 40 && lx <= rr.x2 + 40
                                            && ly >= rr.y1 - 40 && ly <= rr.y2 + 40) {
                                            nearHit = true;
                                            break;
                                        }
                                    }
                                    if (!nearHit && sawSmallRoi) {
                                        stepEffectFact = L"\n[事实] 本批**第一次点击**" 
                                            + DescribeClickPointForModel(firstClickScreenX, firstClickScreenY)
                                            + L"附近没有任何结构变化（变化都发生在别处）：该步很可能没生效"
                                              L"（点错了/没选中/不可点）。★后面的步骤依赖它，可能全在空转 —— "
                                              L"先单独确认这一步的状态（看截图、或再点一次这一步），再继续后续。"
                                              L"批量做「先选中/先切换 → 再作用于目标」这类链条时应改用 "
                                              L"locateAndClick(targets=[…])（逐步校验、失败即停）。";
                                    } else if (!nearHit && !sawSmallRoi && !settled.reacted) {
                                        stepEffectFact = L"\n[事实] 本批点了 " 
                                            + std::to_wstring(clickedSteps)
                                            + L" 次但整屏没有可归因的变化：很可能一步都没生效。"
                                              L"先确认第一步（选中/切换）的状态，别继续往下堆动作。";
                                    }
                                    AppendAiDebugLog(L"  [诊断] 批量逐步校验：点击 "
                                        + std::to_wstring(clickedSteps) + L" 次，首次点击"
                                        + (nearHit ? L"附近已变" : L"附近无变化")
                                        + (sawSmallRoi ? L"" : L"（整屏无小范围变化）"));
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
                                    // 交互后界面没动就别强推图：交给差分决定，省一张截图的钱
                                    if (wantsLaunchSettle || settled.reacted)
                                        forceNextObserveUpload = true;
                                }
                            } else if (settleBaseline) {
                                DeleteBitmapHandle(settleBaseline);
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
                        if (stepCount == 0 && !skippedDupNote.empty()) {
                            return skippedDupNote
                                + L"\n若目标是桌面图标/文件：单击只会选中，打开请用 "
                                  L"locateAndClick(target=..., doubleClick=true) 或 openFile(路径)。"
                                + altNote;
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
                            return L"[错误] 本批 0 步：没有可执行的动作（空数组或全部被跳过）"
                                + altNote;
                        }
                        std::wstring settleFact;
                        if (settleNoReactionThisBatch) {
                            settleFact = L"\n[事实] settle无反应：界面相对操作前几乎无变化。";
                        }
                        settleFact += stepEffectFact;
                        if (!skippedDupNote.empty()) {
                            return L"已执行 " + std::to_wstring(stepCount)
                                + L" 步；另跳过重复近点点击 1 步" + altNote + settleFact;
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
                                + firstInvalidError + altNote + settleFact;
                        }
                        return L"已执行 " + std::to_wstring(stepCount) + L" 步" + altNote
                            + settleFact;
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
                auto captureObservationNow = [&](std::string& outB64, int& outW, int& outH,
                    int maxLongEdge = 768, double scaleOverride = 0.0) -> bool {
                    outB64.clear();
                    outW = outH = 0;
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    parkCursorAwayFromUi();
                    Sleep(40);
                    int cx1 = 0, cy1 = 0, cx2 = 0, cy2 = 0;
                    if (!resolveAiRegion(cx1, cy1, cx2, cy2)) return false;
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
                            + L") 窗口模式=" + (wmUsesTarget() ? L"是" : L"否"));
                    }
                    HBITMAP bmp = nullptr;
                    if (wmUsesTarget()) {
                        bmp = wmExecPtr->CaptureScreenRegionFromWindow(
                            cx1, cy1, cx2, cy2, lockedScreen_, lockedVirtX_, lockedVirtY_);
                    } else {
                        bmp = CaptureAiRegionComposed(cx1, cy1, cx2, cy2);
                    }
                    if (!bmp) return false;
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
                    const AiImageEncodeResult enc = EncodeBitmapForAiAnalysis(
                        bmp, scale, maxLongEdge, imeStatus.empty() ? nullptr : &imeStatus);
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
                    const bool skipUnchangedCheck = forceRefresh || forceNextObserveUpload
                        || AiTakeExplicitScreenshotRequest();
                    forceNextObserveUpload = false;

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
                                    if (structurallyQuiet && !actionLocalStructural) {
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
                    // ★屏幕文字坐标索引（通用）：本地 OCR 出「哪些文字在哪」，
                    // 让模型不靠看图也能准确点。OCR 未安装则整段静默跳过。
                    // 这是「AI 看不懂界面/靠猜坐标」的正解，也与文档 §21 的 PP-OCR 评估对应。
                    if (AiFastPathsEnabled() && CheckOcrEnvironment(false).state == OcrEnvState::Ready) {
                        const OcrEngineOutput ocr = RunOcrOnBitmap(bmp, cx1, cy1, false);
                        // 原始行表留一份给「文字直点」（textIndex 只是给模型看的裁剪版）
                        StoreOcrScreenIndex(ocr, cx1, cy1, cx2, cy2);
                        if (ocr.success && !ocr.lines.empty()) {
                            std::wstring idx;
                            int kept = 0;
                            int numericKept = 0;
                            for (const auto& ln : ocr.lines) {
                                const std::wstring t = Trim(ln.text);
                                if (t.size() < 2 || t.size() > 24) continue;
                                if (ln.confidence < 0.55) continue;
                                // ★纯数字（价格/血量/分数）不是可点按钮，排在后面且限量：
                                // 实测满屏价格把索引塞满（33 条全是 100/75/6666…），
                                // 模型反而找不到「一键全选/上一页/确认」这类**文字按钮**。
                                bool hasLetter = false;
                                for (const wchar_t c : t) {
                                    if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')
                                        || c >= 0x4E00) { hasLetter = true; break; }
                                }
                                if (!hasLetter) {
                                    if (numericKept >= 6) continue;
                                    ++numericKept;
                                }
                                if (kept >= 24) break;
                                if (!idx.empty()) idx += L"；";
                                idx += t + L"("
                                    + std::to_wstring((ln.x1 + ln.x2) / 2) + L","
                                    + std::to_wstring((ln.y1 + ln.y2) / 2) + L")";
                                ++kept;
                            }
                            if (kept > 0) {
                                r.textIndex = L"屏幕文字索引（本地 OCR，坐标为**屏幕绝对像素**；"
                                    L"带文字的才是按钮/标签，纯数字是价格数值不用点；"
                                    L"可直接 locateAndClick(target=其中任一文字) 或按坐标点）：" + idx;
                                r.textIndexCount = kept;
                            }
                        }
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
                    // ★前台不是浏览器窗口（游戏/自绘应用）时抬到 1024：这类画面没有控件树，
                    //   模型只能靠这一张图认格子/单位，768×432 上小目标（植物/僵尸/道具）
                    //   只剩十几个像素，识图基本靠猜。像素 ×1.78，只在非浏览器前台付出。
                    const int observeLongEdge = ForegroundWindowIsBrowserClass() ? 768 : 1024;
                    const AiImageEncodeResult enc = EncodeBitmapForAiAnalysis(
                        bmp, scale, observeLongEdge, imeStatus.empty() ? nullptr : &imeStatus);
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
                agentHooks.onActivateWindow = [&](const std::wstring& query) -> std::wstring {
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    // Alt 还按着时切窗会被预览吃掉，先落地
                    releaseAltTabIfHeld();
                    const auto all = windowmode::ListSwitchableWindows();
                    const auto hits = windowmode::MatchWindows(all, query);
                    if (hits.empty()) {
                        return L"[错误] 没有标题/进程名包含「" + query + L"」的窗口。"
                            L"当前窗口（Z 序）：\n" + windowmode::FormatWindowList(all)
                            + L"\n换个关键词再调；确实没开就用 runProgram/openFile 打开。";
                    }
                    // 多候选一律拒绝自动切：模糊 match（edge/excel）极易切错窗
                    if (hits.size() > 1) {
                        return L"[错误] activateWindow「" + query + L"」匹配到 "
                            + std::to_wstring(hits.size())
                            + L" 个窗口，拒绝自动选择以免切错。"
                            L"请把 match 改成能唯一锁定的标题关键词（如「历史记录」「浏览记录.xlsx」），"
                            L"不要只写进程名 edge/excel/msedge。\n候选：\n"
                            + windowmode::FormatWindowList(hits);
                    }
                    const auto& target = hits.front();
                    std::wstring error;
                    const bool ok = windowmode::ActivateWindow(target.hwnd, error);
                    AppendAiDebugLog(L"  [诊断] activateWindow「" + query + L"」→ "
                        + (ok ? L"已切到：" + target.title : L"失败：" + error));
                    if (!ok) {
                        return L"[错误] 切窗失败：" + error
                            + L"。可改用 switchWindow(action=openPreview, force=true) 兜底。";
                    }
                    if (AiLogicConvertSessionActive())
                        AiLogicConvertNoteWindowActivate(query);
                    std::wstring out = L"已切到前台：" + target.title;
                    if (!target.processName.empty()) out += L" [" + target.processName + L"]";
                    return out;
                };
                agentHooks.onActivateByProcess = [&](const std::wstring& processName) -> std::wstring {
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
                auto ensureExtensionConnected = [&](const wchar_t* what) -> std::wstring {
                    auto& bridge = windowmode::ExtBridgeServer::Instance();
                    if (bridge.IsExtensionConnected()) return {};
                    bridge.RefreshDiscovery();
                    std::wstring waitErr;
                    AppendAiDebugLog(L"  [诊断] 扩展未连接，等待本机桥 port="
                        + std::to_wstring(bridge.Port()) + L"…");
                    if (!bridge.WaitForExtension(12000, waitErr)) {
                        return std::wstring(L"[错误] 未连接配套扩展，无法 ")
                            + what
                            + L"。本机桥已监听 port=" + std::to_wstring(bridge.Port())
                            + L"（HTTP探测=" + std::to_wstring(bridge.HttpProbeCount())
                            + L" WS握手失败=" + std::to_wstring(bridge.WsHandshakeFailCount())
                            + L"）。请点工具栏「键鼠工坊」看弹窗状态并点重连；"
                              L"仍失败再用 locateAndClick。";
                    }
                    AppendAiDebugLog(L"  [诊断] 扩展已连接");
                    return {};
                };
                agentHooks.onObservePage = [&](bool force, const std::wstring& titleHintIn,
                    const std::wstring& query) -> std::wstring {
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
                        // 不是浏览器窗口标题：窗口模式配置/游戏绑窗名兜底，否则不给 hint
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
                    std::string result;
                    std::wstring err;
                    if (!bridge.Request("observePage", extra, result, err, 15000)) {
                        return L"[错误] observePage 失败：" + err;
                    }
                    const PageSnapshot snap = ParsePageSnapshotJson(result);
                    if (!snap.error.empty() && snap.kind == PageKind::Unknown)
                        return L"[错误] " + snap.error;
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
                            why = L"控件树 0 个可交互节点（canvas/内置页/侧边栏，DOM 看不到）";
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
                        if (actionLine.find(L"typeRef") != std::wstring::npos) {
                            body += L"\n[事实] 提交后地址未变。搜人/搜词请 searchOnPage(query)，勿再 keyClick(Enter)。";
                        } else if (actionLine.find(L"clickRef") != std::wstring::npos) {
                            body += L"\n[事实] 页面未跳转。点筛选项无效。"
                                L"请点 href 含 space.bilibili.com 或 /video/ 的卡片（会直接打开）。";
                        }
                    }
                    if (actionLine.empty()) return body;
                    return actionLine + L"\n" + body;
                };
                agentHooks.onClickRef = [&](const std::wstring& ref, bool doubleClick) -> std::wstring {
                    if (const std::wstring wait = ensureExtensionConnected(L"clickRef");
                        !wait.empty())
                        return wait;
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
                        if (LooksLikeUserSpaceSiteUrl(after.url)
                            && !LooksLikeWatchVideoSiteUrl(after.url)
                            && !after.url.empty()) {
                            AiLogicConvertNoteOpenWebpage(after.url);
                        } else if (!tmpl.empty()) {
                            AiLogicConvertNoteLocate(clickName.empty() ? ref : clickName,
                                sx, sy, L"left", doubleClick ? 2 : 1, tmpl);
                        } else if (!after.url.empty() && !LooksLikeWatchVideoSiteUrl(after.url)) {
                            AiLogicConvertNoteOpenWebpage(after.url);
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
                    const std::wstring tree = agentHooks.onObservePage(false, fgTitle, keyword);
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
                // + 1~2 轮 VLM。仅左键单击、非窗口模式、前台不是浏览器/自己时尝试；
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
                    if (pickIdx < 0) {
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
                    if (!windowmode::InvokeUiControlByName(hit.name, hit.id, actualName,
                            actualId, rc, invoked, warn)) {
                        outWhy = L"UIA 元素已失效（界面可能刚变）";
                        return {};
                    }
                    const int cx = (rc.left + rc.right) / 2;
                    const int cy = (rc.top + rc.bottom) / 2;
                    AppendAiDebugLog(L"  [诊断] UIA 优先：命中「" + actualName + L"」["
                        + hit.controlType + L" id=" + std::to_wstring(actualId) + L"]"
                        + (invoked ? L" → InvokePattern" : L" → 点矩形中心")
                        + L"（未整屏截图/未识图）"
                        + (warn.empty() ? L"" : (L"；" + warn)));
                    if (!invoked) {
                        // 无 InvokePattern 的控件（列表项/树项）：仍是点中心，但按 UIA
                        // 矩形而不是像素猜测；同屏重复点击守卫照旧生效
                        if (const std::wstring dup = notePointerClick(cx, cy, true);
                            !dup.empty()) {
                            return dup;
                        }
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
                agentHooks.onListUiControls = [&](int maxCount) -> std::wstring {
                    HWND fg = GetForegroundWindow();
                    if (!fg || !IsWindow(fg)) return L"[错误] 没有前台窗口。";
                    wchar_t titleBuf[512]{};
                    GetWindowTextW(fg, titleBuf, 512);
                    const auto items = windowmode::ListInteractiveUiControls(fg, maxCount);
                    if (items.empty()) {
                        return L"[错误] 前台窗口「" + std::wstring(titleBuf)
                            + L"」UIA 枚举不到可交互控件（自绘界面/游戏/未实现 UIA）。"
                              L"网页内容请用 observePage/clickRef；"
                              L"自绘界面请改用 locateAndClick 识图点击。";
                    }
                    AppendAiDebugLog(L"  [诊断] listUiControls: 前台「"
                        + std::wstring(titleBuf) + L"」枚举到 "
                        + std::to_wstring(items.size()) + L" 个可交互控件（未截屏）");
                    std::wstring out = L"前台窗口：" + std::wstring(titleBuf) + L"\n";
                    out += windowmode::FormatUiControlListForAgent(items, 2000);
                    if (LooksLikeBrowserWindowTitle(titleBuf)) {
                        out += L"（浏览器：这里只有外壳控件——地址栏/标签/菜单；"
                               L"网页里的按钮请用 observePage/clickRef）\n";
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
                    if (!windowmode::InvokeUiControlByName(name, id, actualName, actualId, rc,
                            invoked, warn)) {
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
                    if (invoked) {
                        how = L"InvokePattern 触发（未打像素）";
                    } else {
                        if (const std::wstring dup = notePointerClick(cx, cy, true);
                            !dup.empty()) {
                            return dup;
                        }
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
                std::function<std::wstring(const std::wstring&, int, const std::wstring&, int)>
                    locateAndClickFn = nullptr;
                // 布局记忆用：最近一次真识图定位到的屏幕框（网格推断的锚点）
                AiUiLayoutRect lastLocateScreenRect{};
                auto MakeAiUiLayoutKey = [](const std::wstring& targetDesc) -> AiUiLayoutKey {
                    AiUiLayoutKey k;
                    // ★键必须保真：不做「去掉按钮后缀/折叠」这类模糊归一 ——
                    // 实测「一键全选」命中了「自选僵尸卡牌」的坐标（把选卡面板点关了）。
                    // 措辞不同就当作新目标（miss 只是多识图一次，安全；错命中会点错东西）。
                    std::wstring t = Trim(targetDesc);
                    for (auto& c : t) {
                        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
                    }
                    if (t.empty()) return k;
                    HWND fg = GetForegroundWindow();
                    if (!fg) return k;
                    k.target = t;
                    k.windowIdentity = AiUiWindowIdentityForLayout(fg);
                    int sx = 0, sy = 0, sw = 0, sh = 0;
                    GetVirtualScreenRect(sx, sy, sw, sh);
                    k.screenW = sw;
                    k.screenH = sh;
                    return k;
                };
                locateAndClickFn =
                    [&](const std::wstring& targetDesc, int refineLevels,
                        const std::wstring& button, int clickCount) -> std::wstring {
                    LocateAndClickNestGuard locateGuard;
                    if (!locateGuard.entered()) {
                        return L"[错误] locateAndClick 不可嵌套（防无限外包定位）。";
                    }
                    // 这个目标本次运行被点过几次（1 = 一次性目标：不做复用缓存/记忆）
                    const int targetSeenCount = NoteLocateTargetSeen(targetDesc);
                    const bool reusableTarget = targetSeenCount >= 2;
                    // ★布局记忆：这个界面上「这个目标」之前定位成功过 → 直接用旧坐标，
                    // 0 次识图（Midscene caching / UiPath Object Repository 的同类做法）。
                    // 键含窗口身份+屏幕尺寸：换窗/挪窗/改分辨率自动作废。
                    const AiUiLayoutKey layoutKey = MakeAiUiLayoutKey(targetDesc);
                    if (AiFastPathsEnabled() && !layoutKey.empty() && clickCount == 1) {
                        AiUiLayoutRect mem{};
                        if (AiUiLayoutRecall(layoutKey, mem)) {
                            // ★命中前硬校验（「点错按钮」的闸）：用当前画面重算外观签名，
                            // 对不上说明界面已经变了（面板关了/换页/换内容）→ 回退真识图，绝不盲点。
                            bool sigOk = true;
                            {
                                int rx1 = 0, ry1 = 0, rx2 = 0, ry2 = 0;
                                if (resolveAiRegion(rx1, ry1, rx2, ry2)) {
                                    HBITMAP vf = captureAiRegionBmp(rx1, ry1, rx2, ry2);
                                    std::vector<uint8_t> gb;
                                    int gw = 0, gh = 0, gs = 0;
                                    if (vf && AiUiBitmapToGrayBytes(vf, gb, gw, gh, gs)) {
                                        uint8_t sig[kAiUiSigN * kAiUiSigN]{};
                                        const AiUiLayoutRect boxInFrame{ mem.x1 - rx1, mem.y1 - ry1,
                                            mem.x2 - rx1, mem.y2 - ry1 };
                                        if (AiUiLayoutSignature(gb.data(), gw, gh, gs, boxInFrame, sig))
                                            sigOk = AiUiLayoutSignatureMatches(layoutKey, sig);
                                    }
                                    if (vf) DeleteBitmapHandle(vf);
                                }
                            }
                            if (!sigOk) {
                                AppendAiDebugLog(L"  [诊断] 布局记忆命中但外观校验不通过（界面变了）"
                                    L"→ 回退真识图");
                                AiUiLayoutNoteClickNoEffect(layoutKey);
                            } else {
                            const std::wstring clickJson = BuildScreenClickActionsJson(
                                mem.cx(), mem.cy(), false, button, clickCount);
                            const std::wstring execMsg = executeActionsJsonNow(clickJson);
                            AppendAiDebugLog(L"  [诊断] 布局记忆命中：「"
                                + TruncLog(targetDesc, 16) + L"」→ 屏幕("
                                + std::to_wstring(mem.cx()) + L"," + std::to_wstring(mem.cy())
                                + L")，省一次识图");
                            lastLocateScreenRect = mem;
                            std::wstring out = L"locateAndClick 已点击"
                                + DescribeClickPointForModel(mem.cx(), mem.cy())
                                + L"（布局记忆命中：这个界面上该目标位置没变，未重新识图）";
                            if (!execMsg.empty()) out += L"；" + execMsg;
                            return out;
                            }
                        }
                    }
                    // 网页左键：先试 DOM（右键门禁在 ShouldUseDomFirstAction 里挡住）
                    if (button != L"right") {
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
                                + L"），回落识图定位");
                        }
                    }
                    // 整段定位+点击期间藏壳/调试窗（含 Zoom 二次截屏、缓存模板裁剪）
                    qst::desktop_tools::ScopedHideOwnUiForCapture hideOwn(UserFacingMainHwnd());
                    // ── 定位缓存（第三档加速）：同一目标在循环里反复点时，
                    // 屏幕没动就不该再烧一次 VLM。命中条件很严（窗口键一致 + 模板高分 +
                    // 唯一命中 + 距缓存点不远），且点击后画面没变就立刻作废这条缓存。
                    AiLocateCacheKey cacheKey;
                    cacheKey.target = AiLocateNormalizeTarget(targetDesc);
                    cacheKey.captureW = liveMap.capX2 - liveMap.capX1;
                    cacheKey.captureH = liveMap.capY2 - liveMap.capY1;
                    {
                        HWND fgWnd = GetForegroundWindow();
                        if (fgWnd) {
                            wchar_t tbuf[512]{};
                            GetWindowTextW(fgWnd, tbuf, 512);
                            cacheKey.windowTitle = tbuf;
                            wchar_t cbuf[128]{};
                            GetClassNameW(fgWnd, cbuf, 128);
                            cacheKey.windowClass = cbuf;
                        }
                    }
                    bool usedLocateCache = false;
                    int cachedX = 0;
                    int cachedY = 0;
                    AiLocateCacheEntry cacheEntry;
                    if (clickCount <= 1 && !wmUsesTarget()
                        && AiLocateCacheLookup(cacheKey, &cacheEntry)) {
                        // ★窗口位移重锚：窗口被拖动/移动（窗口化游戏、用户拖窗）后，
                        // 模板仍在窗口内同一相对位置；按窗口矩形差把期望点挪过去，
                        // 否则「距缓存点太远」会把完全可用的缓存判掉、白回一次识图。
                        RECT fgRectNow{};
                        bool haveFgRectNow = false;
                        if (HWND fgNow = GetForegroundWindow()) {
                            if (GetWindowRect(fgNow, &fgRectNow)) haveFgRectNow = true;
                        }
                        int expectX = cacheEntry.screenX;
                        int expectY = cacheEntry.screenY;
                        if (cacheEntry.hasWindowRect && haveFgRectNow) {
                            const int moved = std::abs(fgRectNow.left - cacheEntry.windowRect.left)
                                + std::abs(fgRectNow.top - cacheEntry.windowRect.top);
                            if (moved > 0) {
                                AiLocateCacheReanchor(cacheEntry.screenX, cacheEntry.screenY,
                                    cacheEntry.windowRect, fgRectNow, &expectX, &expectY);
                                AppendAiDebugLog(L"  [诊断] 定位缓存按窗口位移重锚：("
                                    + std::to_wstring(cacheEntry.screenX) + L","
                                    + std::to_wstring(cacheEntry.screenY) + L") → ("
                                    + std::to_wstring(expectX) + L"," + std::to_wstring(expectY)
                                    + L") 位移 " + std::to_wstring(moved) + L"px");
                            }
                        }
                        // 只在期望点周围一小片搜（模板匹配够用且不会被同屏相似控件带偏）
                        const int half = 200;
                        const int sx1 = (std::max)(liveMap.capX1, expectX - half);
                        const int sy1 = (std::max)(liveMap.capY1, expectY - half);
                        const int sx2 = (std::min)(liveMap.capX2, expectX + half);
                        const int sy2 = (std::min)(liveMap.capY2, expectY + half);
                        if (sx2 - sx1 >= 16 && sy2 - sy1 >= 16) {
                            ImageMatchOptions mopt;
                            mopt.thresholdPercent = 90.0;
                            mopt.scaleMin = 0.97;
                            mopt.scaleMax = 1.03;
                            mopt.scaleStep = 0.03;
                            mopt.disablePyramid = true;
                            mopt.maxMatches = 6;
                            // 单帧采样：返回最佳分/次佳分/与期望点的距离
                            auto sampleCache = [&](AiLocateCacheAcceptInput* outAcc,
                                                   int* outX, int* outY) {
                                const ImageMatchOutput mo = FindTemplateOnScreenMulti(
                                    sx1, sy1, sx2, sy2, cacheEntry.tmpl, mopt);
                                AiLocateCacheAcceptInput acc;
                                acc.bestScore = -1.0;
                                int bx = 0, by = 0;
                                for (const auto& m : mo.matches) {
                                    if (!m.found) continue;
                                    int mx = 0, my = 0;
                                    FindImageMatchCenter(m, mx, my);
                                    const int dx = mx - expectX;
                                    const int dy = my - expectY;
                                    const int dist = static_cast<int>(
                                        std::sqrt(static_cast<double>(dx) * dx
                                            + static_cast<double>(dy) * dy) + 0.5);
                                    if (m.score > acc.bestScore) {
                                        acc.secondScore = acc.bestScore;
                                        acc.secondDistToCached = acc.bestDistToCached;
                                        acc.bestScore = m.score;
                                        acc.bestDistToCached = dist;
                                        bx = mx;
                                        by = my;
                                    } else if (m.score > acc.secondScore) {
                                        acc.secondScore = m.score;
                                        acc.secondDistToCached = dist;
                                    }
                                }
                                if (outAcc) *outAcc = acc;
                                if (outX) *outX = bx;
                                if (outY) *outY = by;
                            };
                            // ★N-of-M 迟滞：实测单帧检测在 60~70% 摆动（最低 40%），
                            // 「保留多数帧都出现的目标」后稳定率 80~100%。画面在动
                            // （游戏/视频/滚动）时单帧就点极易点在残影上，多花两次
                            // 毫秒级小区域匹配非常划算。
                            constexpr int kCacheSamples = 3;
                            constexpr int kCacheNeedPass = 2;
                            int passCount = 0;
                            AiLocateCacheAcceptInput accept;
                            accept.bestScore = -1.0;
                            int lastGoodX = 0, lastGoodY = 0;
                            double bestPassScore = -1.0;
                            for (int s = 0; s < kCacheSamples; ++s) {
                                if (s > 0) Sleep(45);
                                AiLocateCacheAcceptInput acc;
                                int bx = 0, by = 0;
                                sampleCache(&acc, &bx, &by);
                                if (acc.bestScore >= 0 && ShouldAcceptAiLocateCacheHit(acc)) {
                                    ++passCount;
                                    cachedX = bx;
                                    cachedY = by;
                                    if (acc.bestScore > bestPassScore) {
                                        bestPassScore = acc.bestScore;
                                        accept = acc;
                                    }
                                } else if (acc.bestScore >= 0) {
                                    // 留一份「没通过门槛」的样本用于日志
                                    if (accept.bestScore < 0) accept = acc;
                                }
                            }
                            // 后续日志/判定统一用「通过次数」口径
                            if (passCount > 0) {
                                accept.bestScore = bestPassScore;
                                cachedX = lastGoodX;
                                cachedY = lastGoodY;
                            }
                            if (ShouldAcceptAiLocateCacheHitHysteresis(passCount, kCacheSamples,
                                    kCacheNeedPass, kCacheSamples)) {
                                usedLocateCache = true;
                                AiLocateCacheNoteHit(cacheKey);
                                AppendAiDebugLog(L"  [诊断] 定位缓存命中：屏幕("
                                    + std::to_wstring(cachedX) + L","
                                    + std::to_wstring(cachedY) + L") 匹配 "
                                    + std::to_wstring(static_cast<int>(accept.bestScore + 0.5))
                                    + L"% 偏差 " + std::to_wstring(accept.bestDistToCached)
                                    + L"px；迟滞 " + std::to_wstring(passCount) + L"/"
                                    + std::to_wstring(kCacheSamples) + L" 帧一致（省一次识图）");
                            } else if (passCount > 0) {
                                AppendAiDebugLog(L"  [诊断] 定位缓存抖动："
                                    + std::to_wstring(passCount) + L"/"
                                    + std::to_wstring(kCacheSamples)
                                    + L" 帧才命中（画面在动）→ 不打缓存点，回识图");
                                AiLocateCacheInvalidate(cacheKey);
                            } else if (accept.bestScore >= 0) {
                                AppendAiDebugLog(L"  [诊断] 定位缓存未通过门槛（最佳 "
                                    + std::to_wstring(static_cast<int>(accept.bestScore + 0.5))
                                    + L"% / 次佳 "
                                    + std::to_wstring(static_cast<int>(accept.secondScore + 0.5))
                                    + L"%），回落识图");
                            } else {
                                AppendAiDebugLog(L"  [诊断] 定位缓存未命中模板，回落识图");
                            }
                        }
                    }
                    // ── 错点自纠（备用项，用户提出）────────────────────────────────
                    // 场景：点下去**没有任何变化证据** → 那一点就是**已知的错点**。
                    // 这正是 PrecisionCUA 红叉闭环需要的锚点，而且不必动真光标：把红叉画在
                    // **放大图**上（本机 GDI，<1ms），让识图子模型对着红叉重新给出目标的绝对坐标，
                    // 然后补点一次。比让主模型重看整屏便宜（省一整轮 10~40s），也躲开
                    // 「反复给出同一个错坐标」（Attentional Fixation, MEGA-GUI 记录的失败模式）。
                    // 与「真光标伺服」的差别：不动用户桌面、不触发 hover、不要求 32px 光标
                    // 在 960 宽的下采样图里还被认出来（那个方案唯一开源实现实测退步 15pp）。
                    auto tryMissSelfCorrect = [&](const std::wstring& target, int failedX,
                                                  int failedY, std::wstring* outWhy) -> std::wstring {
                        if (outWhy) outWhy->clear();
                        auto bail = [&](const wchar_t* why) -> std::wstring {
                            if (outWhy) *outWhy = why;
                            return {};
                        };
                        const std::wstring visionModel = ResolveVisionSubtaskModelName(
                            appSettings_.ai, effModel);
                        const std::wstring model = visionModel.empty() ? effModel : visionModel;
                        if (!ModelSupportsVision(model)) return bail(L"无多模态模型可用");
                        // 裁「错点附近」的放大图（±260px，含容错），画上红叉
                        const int half = 260;
                        const int rx1 = failedX - half;
                        const int ry1 = failedY - half;
                        const int rx2 = failedX + half;
                        const int ry2 = failedY + half;
                        HBITMAP shot = CaptureScreenRegion(rx1, ry1, rx2, ry2);
                        if (!shot) return bail(L"局部截图失败");
                        DrawPredictionCrossOnBitmap(shot, failedX - rx1, failedY - ry1, 22, 4);
                        const AiImageEncodeResult enc = EncodeBitmapForAiZoomUpload(shot, 768);
                        DeleteBitmapHandle(shot);
                        if (enc.base64.empty()) return bail(L"局部编码失败");
                        auto coreOk = ModelSupportsVision(model);
                        if (!coreOk) return bail(L"无多模态模型可用");
                        const std::wstring prompt = BuildMissSelfCorrectPrompt(
                            target, enc.outWidth, enc.outHeight, failedX, failedY);
                        const AiActionResult vr = RunAiOneShotVisionQuery(model,
                            appSettings_.ai.savedModels, appSettings_.ai.apiUrl,
                            appSettings_.ai.apiKey,
                            BuildAiActionVisionQuerySystemPrompt(enc.outWidth, enc.outHeight),
                            prompt, enc.base64,
                            ResolveAiLocateVisionTimeoutSec(eff.aiTimeoutSec) * 1000,
                            stopFlag_, &aiHttpAbort_);
                        if (!vr.ok) return bail(L"自纠识图失败");
                        if (IsVisionLocateNotFound(vr.textResult)) {
                            return bail(L"自纠识图也判定目标不在画面内");
                        }
                        int ax = 0, ay = 0;
                        if (!TryParseCoordinatePair(vr.textResult, ax, ay)) {
                            return bail(L"自纠识图未给出坐标");
                        }
                        // 归一化 → 屏幕（本图就是裁切区，一一对应）
                        const int sw = (std::max)(1, rx2 - rx1);
                        const int sh = (std::max)(1, ry2 - ry1);
                        const int nx = rx1 + std::clamp(ax, 0, 1000) * sw / 1000;
                        const int ny = ry1 + std::clamp(ay, 0, 1000) * sh / 1000;
                        const int dx = nx - failedX;
                        const int dy = ny - failedY;
                        // 与错点几乎重合 → 没有新信息，别白点第二下
                        if (std::abs(dx) < 24 && std::abs(dy) < 24) {
                            return bail(L"自纠点与原错点几乎重合（无新信息）");
                        }
                        // 遮挡/前台/重复点守卫，与主链路同一套
                        if (!wmUsesTarget()
                            && !windowmode::IsScreenPointOnForegroundWindow(nx, ny)) {
                            return bail(L"自纠点不在前台窗口上");
                        }
                        if (!notePointerClick(nx, ny, true).empty()) {
                            return bail(L"自纠点命中重复点击守卫");
                        }
                        parkCursorAwayFromUi();
                        const std::wstring fixMsg = executeActionsJsonNow(
                            BuildScreenClickActionsJson(nx, ny, false, L"left", 1));
                        AppendAiDebugLog(L"  [诊断] 错点自纠：红叉="
                            + std::to_wstring(failedX) + L"," + std::to_wstring(failedY)
                            + L" → 识图给出 " + std::to_wstring(nx) + L"," + std::to_wstring(ny)
                            + L"（差 " + std::to_wstring(dx) + L"," + std::to_wstring(dy)
                            + L"px），已补点一次");
                        return L"；★第一次点" + DescribeClickPointForModel(failedX, failedY)
                            + L"没有任何反应；宿主已把该点当红叉让识图子模型重新定位，"
                              L"并补点在" + DescribeClickPointForModel(nx, ny) + L"（差 "
                            + std::to_wstring(dx) + L"," + std::to_wstring(dy)
                            + L"px）。请用本轮截图核对结果；仍不对就换更具体的短标签，"
                              L"**不要**再重复这两个坐标";
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
                                            int* outX, int* outY, std::wstring* outNote) -> bool {
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
                        HBITMAP region = CaptureScreenRegion(rx1, ry1, rx2, ry2);
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
                        // 最近的同名文字（探针框很小，等价于「就是它」）；不做模糊匹配
                        AiOcrDirectHit ph;
                        const int w = rx2 - rx1, h = ry2 - ry1;
                        if (!AiOcrPickNearestText(probe.lines, wantText, px, py,
                                (std::max)(w, h), w, h, &ph)) {
                            if (outNote) *outNote = ph.why.empty()
                                ? (L"局部没读到「" + wantText + L"」") : ph.why;
                            return false;
                        }
                        if (outX) *outX = ph.screenX;
                        if (outY) *outY = ph.screenY;
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
                    if (!usedLocateCache && AiFastPathsEnabled()
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
                                    const bool verified = ocrProbeText(wantText,
                                        hit.screenX, hit.screenY, phw, phh, &vx, &vy, &vnote);
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
                    std::string b64;
                    int aw = 0, ah = 0;
                    // 定位长边 960 + 满分辨率（不被 aiImageScale=0.5 再压一半）：
                    // 小控件/输入框在整屏缩略后仍可辨，识图请求体也保持可控
                    if (!captureObservationNow(b64, aw, ah, 960, 1.0) || b64.empty()) {
                        zr.errorMessage = L"locateAndClick 截屏失败";
                        return zr;
                    }
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
                    // UIA+视觉融合的锚点：窗口模式前台往往不是目标窗口，故只在非窗口模式取
                    std::vector<AiUiAnchor> uiAnchors;
                    if (!wmUsesTarget()) {
                        for (const auto& ctl : windowmode::ListInteractiveUiControls(
                                 GetForegroundWindow(), 60)) {
                            uiAnchors.push_back(AiUiAnchor{ ctl.rect.left, ctl.rect.top,
                                ctl.rect.right, ctl.rect.bottom, ctl.name });
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
                    if (usedLocateCache) {
                        zr.ok = true;
                        zr.screenX = cachedX;
                        zr.screenY = cachedY;
                        zr.levelsUsed = 0;
                        zr.skippedRefine = true;  // 未走 Zoom 精炼，日志沿用「跳过二级」
                    } else if (ocrDirectHit) {
                        zr.ok = true;
                        zr.screenX = ocrDirectX;
                        zr.screenY = ocrDirectY;
                        zr.levelsUsed = 0;
                        zr.skippedRefine = true;
                        // OCR 是真读到这段文字才点的 → 直接记「可用」，别让模型看到
                        // 「可疑」又回头确认一轮（这正是用户抱怨的「总要反复确认」）。
                        zr.verdict = AiLocateVerdict::Accept;
                        locateVerdict = AiLocateVerdict::Accept;
                        locateVerdictWhy = L"本地 OCR 索引命中文字标签";
                    } else {
                        zr = runVisionLocate();
                        if (!zr.ok) {
                            const std::wstring detail = zr.errorMessage.empty()
                                ? L"定位失败" : zr.errorMessage;
                            return L"[错误] " + detail
                                + L"。换短标签再 locate 最多1次，或看图换策略 / completeTask。";
                        }
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
                                    if (std::abs(dx) > 6 || std::abs(dy) > 6) {
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
                        // 识图成功 → 存模板，供循环里下一次直接复用
                        // （模板取固定 80×80 邻域：命中判定还有窗口键 + 高分 + 唯一性三重门闩）
                        // ★只在「这个目标本次运行已经点过至少一次」时才存：一次性按钮
                        //   不进复用缓存（存了也没人再查，还会把一次性坐标固化成「下次直接用」）。
                        if (!reusableTarget && clickCount <= 1 && !wmUsesTarget()) {
                            AppendAiDebugLog(L"  [诊断] 一次性目标「" + targetDesc
                                + L"」首次定位：不写定位模板缓存（再点它才存）");
                        }
                        if (reusableTarget && clickCount <= 1 && !wmUsesTarget()) {
                            const int half = 40;
                            int tx1 = zr.screenX - half;
                            int ty1 = zr.screenY - half;
                            int tx2 = zr.screenX + half;
                            int ty2 = zr.screenY + half;
                            if (tx2 - tx1 >= 20 && ty2 - ty1 >= 20) {
                                HBITMAP tmpl = CaptureScreenRegion(tx1, ty1, tx2, ty2);
                                // 低特征模板不入缓存：纯色/空白区存下来下次会匹配到别处
                                if (tmpl && BitmapRegionLooksLowFeature(tmpl, nullptr)) {
                                    DeleteBitmapHandle(tmpl);
                                    tmpl = nullptr;
                                    AppendAiDebugLog(L"  [诊断] 定位框内特征过低，跳过缓存模板");
                                }
                                if (tmpl) {
                                    // 连窗口矩形一起存：窗口被拖动后靠它重锚缓存点
                                    RECT storeRect{};
                                    const bool haveStoreRect = GetForegroundWindow()
                                        && GetWindowRect(GetForegroundWindow(), &storeRect);
                                    AiLocateCacheStore(cacheKey, tmpl, zr.screenX, zr.screenY,
                                        tx2 - tx1, ty2 - ty1,
                                        haveStoreRect ? &storeRect : nullptr);
                                    AppendAiDebugLog(L"  [诊断] 已存定位模板 "
                                        + std::to_wstring(tx2 - tx1) + L"×"
                                        + std::to_wstring(ty2 - ty1)
                                        + L"（循环复用时省识图）");
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
                                    L"请 observePage 后 clickRef 列表第1项，或 locateAndClick "
                                    L"内容区重复卡片网格里最上最左的那张。";
                            }
                        }
                    }
                    // 点击前查 UIA：控件灰掉说明前置条件没满足，点它只会白烧轮次
                    const windowmode::UiElementState uiState =
                        windowmode::ProbeUiElementAtPoint(zr.screenX, zr.screenY);
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
                    // 窗口模式下目标窗口不一定是前台，故只在非窗口模式启用。
                    if (!wmUsesTarget()
                        && !windowmode::IsScreenPointOnForegroundWindow(zr.screenX, zr.screenY)) {
                        HWND fgNow = GetForegroundWindow();
                        wchar_t nowTitle[256]{};
                        if (fgNow) GetWindowTextW(fgNow, nowTitle, 256);
                        AppendAiDebugLog(L"  [诊断] 遮挡校验失败：定位点不属于前台窗口，已拦截点击");
                        return L"[错误] 定位到了 ("
                            + std::to_wstring(zr.screenX) + L"," + std::to_wstring(zr.screenY)
                            + L")，但该点当前不属于前台窗口（当前前台：「" + nowTitle
                            + L"」）——窗口可能被覆盖或已切换，点击会打错目标，已拦截。"
                              L"请 listWindows + activateWindow 确认目标窗口在前台后再操作；"
                              L"网页请 observePage 后 clickRef。";
                    }
                    const bool isDouble = clickCount >= 2;
                    const bool isLeftSingle = (button != L"right") && !isDouble;
                    // 双击/右键是「换手法再试」，不算重复盲点
                    if (const std::wstring dup =
                            notePointerClick(zr.screenX, zr.screenY, isLeftSingle);
                        !dup.empty()) {
                        return dup;
                    }
                    // ★只记「可信」的定位：校验=可疑时（可能没点中/点错）绝不写进布局记忆，
                    // 否则会把错坐标固化成「下次直接用」的记忆。实测：一键全选那次
                    // 定位校验=可疑 + settle 无反应，仍被记住 → 后续直接点错位置。
                    // ★复用缓存（布局记忆 + 网格推断）只对「会被反复点的位置」有价值：
                    //   用户实测反馈——不是每个按钮都值得抽象出可复用定位，很多按钮只点一次。
                    //   一次性按钮既不值得存（下次不再点它），也不该把**一次性的**坐标固化成
                    //   「下次直接用」（实测「一键全选」就是这样点错的）。
                    //   两块都要「整区截屏 + 转灰度」，所以只截一次、两处共用（省一次全窗转换）。
                    if (AiFastPathsEnabled() && reusableTarget && !layoutKey.empty()
                        && isLeftSingle && zr.verdict == AiLocateVerdict::Accept) {
                        int rx1 = 0, ry1 = 0, rx2 = 0, ry2 = 0;
                        HBITMAP frame = resolveAiRegion(rx1, ry1, rx2, ry2)
                            ? captureAiRegionBmp(rx1, ry1, rx2, ry2) : nullptr;
                        std::vector<uint8_t> gb;
                        int gw = 0, gh = 0, gs = 0;
                        if (frame && AiUiBitmapToGrayBytes(frame, gb, gw, gh, gs)) {
                            const AiUiLayoutRect boxInFrame{ lastLocateScreenRect.x1 - rx1,
                                lastLocateScreenRect.y1 - ry1,
                                lastLocateScreenRect.x2 - rx1, lastLocateScreenRect.y2 - ry1 };
                            // ① 布局记忆 + 外观签名（命中前硬校验用）
                            AiUiLayoutRemember(layoutKey, lastLocateScreenRect);
                            {
                                uint8_t sig[kAiUiSigN * kAiUiSigN]{};
                                if (AiUiLayoutSignature(gb.data(), gw, gh, gs, boxInFrame, sig))
                                    AiUiLayoutRememberSignature(layoutKey, sig);
                            }
                            // ② 顺手推断网格：锚点带内周期 → 整片格子坐标（草坪/卡槽/图标阵列）
                            AiUiGridSpec grid{};
                            if (!AiUiLayoutRecallGrid(layoutKey, grid)) {
                                int px = 0, py = 0;
                                if (AiUiDetectGridPeriod(gb.data(), gw, gh, gs, boxInFrame,
                                        16, 420, px, py)) {
                                    AiUiGridSpec g;
                                    g.originX = zr.screenX;
                                    g.originY = zr.screenY;
                                    g.stepX = px;
                                    g.stepY = py;
                                    g.cols = px > 0 ? (std::max)(1, (rx2 - zr.screenX) / px) : 0;
                                    g.rows = py > 0 ? (std::max)(1, (ry2 - zr.screenY) / py) : 0;
                                    if (g.valid()) {
                                        AiUiLayoutRememberGrid(layoutKey, g);
                                        AppendAiDebugLog(L"  [诊断] 网格推断："
                                            + TruncLog(targetDesc, 20) + L" 周期 "
                                            + std::to_wstring(px) + L"×" + std::to_wstring(py)
                                            + L"px（右侧 " + std::to_wstring(g.cols)
                                            + L" 列 / 下方 " + std::to_wstring(g.rows)
                                            + L" 行）→ 之后按 (行,列) 直接点，无需识图");
                                    }
                                }
                            }
                        }
                        if (frame) DeleteBitmapHandle(frame);
                    } else if (!reusableTarget && isLeftSingle
                        && zr.verdict == AiLocateVerdict::Accept && !layoutKey.empty()) {
                        AppendAiDebugLog(L"  [诊断] 一次性目标（本次运行首次定位）：不写布局记忆/"
                            L"网格缓存（同一个目标再点一次才存）");
                    }
                    int br[kClickColorGridN]{}, bg[kClickColorGridN]{}, bb[kClickColorGridN]{};
                    parkCursorAwayFromUi();
                    const bool hadBefore = SampleClickColorGrid(
                        zr.screenX, zr.screenY, br, bg, bb);
                    const std::wstring clickJson = BuildScreenClickActionsJson(
                        zr.screenX, zr.screenY, false, button, clickCount);
                    lastLocateScreenRect = AiUiLayoutRect{
                        zr.screenX - 24, zr.screenY - 24, zr.screenX + 24, zr.screenY + 24 };
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
                    // 坐标单位写清楚（见 DescribeClickPointForModel 的注释）
                    std::wstring out = L"locateAndClick 已"
                        + std::wstring(isDouble ? L"双击" : (button == L"right" ? L"右键点击" : L"点击"))
                        + DescribeClickPointForModel(zr.screenX, zr.screenY)
                        + L" 识图轮次=" + std::to_wstring(zr.levelsUsed);
                    if (zr.usedFindImageSnap) {
                        out += L"；找图精修"
                            + std::to_wstring(static_cast<int>(zr.findImageScore + 0.5)) + L"%";
                    }
                    // 本地校验定级：可疑时明确提示模型别盲信这一个点（不阻断点击，只提示）
                    if (!usedLocateCache && zr.verdict != AiLocateVerdict::Accept) {
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
                        for (const auto& r : lastUiChangeRois) {
                            const int rw = r.x2 - r.x1;
                            const int rh = r.y2 - r.y1;
                            const int area = rw * rh;
                            const bool hits = zr.screenX >= r.x1 - 24 && zr.screenX <= r.x2 + 40
                                && zr.screenY >= r.y1 - 24 && zr.screenY <= r.y2 + 24;
                            if (rw > 220 || rh > 180 || area > 48000) {
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
                        if (compactRoiNear) {
                            out += L"；点击处小范围已变（开关可能已切换）。"
                                L"禁止再点同一位置（会取消）。请 observePage 看 pressed/checked；"
                                L"已是目标态则 completeTask";
                        } else if (colorChanged && !mixedPage && !largeMotion) {
                            out += L"；点击附近颜色已变。勿再点同一位置；"
                                L"请 observePage 看 pressed/checked 后 completeTask";
                        } else if (largeMotion) {
                            out += L"；大范围画面在动（播放器），不能当成开关已切换。"
                                L"请 observePage 看 pressed/checked，勿连点同一坐标";
                        } else {
                            // ★无任何变化证据 → 用「刚点的这个错点」当锚点自纠一次（备用项）。
                            // ★★先问 settle 的权威判定：**只要界面有反应就绝不补点**。
                            //   实测只看局部颜色+变化区就补点，会在开关类按钮上打第二下
                            //   （「自选僵尸卡牌」开面板→我补的一下又关回去），界面进了不一致
                            //   状态后点什么都没反应（用户报障「拿下来一张卡就点不动了」）。
                            std::wstring fixWhy;
                            std::wstring fix;
                            if (isLeftSingle && !usedLocateCache && !AiLastUiSettleReacted()
                                && missSelfCorrectBudget > 0 && AiMissSelfCorrectEnabled()) {
                                --missSelfCorrectBudget;
                                fix = tryMissSelfCorrect(targetDesc, zr.screenX, zr.screenY,
                                    &fixWhy);
                            }
                            if (!fix.empty()) {
                                out += fix;
                            } else {
                                out += L"；点击附近外观接近（若开关仍是旧态则未完成；"
                                       L"勿对同一坐标连点）";
                                if (!fixWhy.empty()) out += L"（已试自纠：" + fixWhy + L"）";
                            }
                        }
                        // 定位缓存闭环：这次是「照缓存点的」，而点下去画面没变、
                        // 也没有小范围变化 → 这条缓存多半是错点，立刻作废（下次老实识图）。
                        // 宁可丢掉优化，也不能把错点固化下来。
                        if (usedLocateCache && !colorChanged && !compactRoiNear) {
                            AiLocateCacheInvalidate(cacheKey);
                            out += L"；定位缓存已作废（点击无变化，下次改用识图）";
                            AppendAiDebugLog(L"  [诊断] 定位缓存作废：点击后画面无变化");
                        }
                        // ★同一条规矩适用于布局记忆：照记忆坐标点下去没变化 → 记一笔。
                        // **不删条目**（用户实测：小范围颜色采样会误报，工具本身耗时几秒后
                        // 画面早变了）；连续两次才让 Recall 回退真识图并刷新坐标。
                        if (!colorChanged && !compactRoiNear
                            && !layoutKey.empty() && isLeftSingle) {
                            AiUiLayoutNoteClickNoEffect(layoutKey);
                            AppendAiDebugLog(L"  [诊断] 布局记忆记一笔未生效（连续 2 次才回退识图，"
                                L"当前过期条目 " + std::to_wstring(AiUiLayoutStaleCount()) + L"）");
                        }
                    }
                    out += L"；已移开指针防 hover";
                    AppendAiTaskMemoLine(L"done: locateAndClick");
                    if (!execMsg.empty()) out += L"；" + execMsg;
                    return out;
                };
                agentHooks.onLocateAndClick = locateAndClickFn;
                // ★★网格定位：锚点只识图一次，整片坐标由画面周期推断；
                // 之后每次 grid(anchor, cells) 都是纯坐标计算（0 次识图）——
                // 「两次找图覆盖放置僵尸」就靠这个：① 定位卡槽 ② 定位草坪锚点+推断网格，
                // 后面每次放僵尸只是 grid(anchor="草坪", cells=[[r,c],…])。
                agentHooks.onLocateGrid = [&, locateAndClickFn](
                    const std::wstring& anchor,
                    const std::vector<std::pair<int, int>>& cells,
                    const std::wstring& button, int clickCount) -> std::wstring {
                    if (!locateAndClickFn) return L"[错误] 网格定位未初始化。";
                    const AiUiLayoutKey key = MakeAiUiLayoutKey(anchor);
                    if (key.empty()) {
                        return L"[错误] 网格定位拿不到前台窗口身份：先 listWindows + activateWindow "
                               L"把目标窗口切到前台。";
                    }
                    int gx1 = 0, gy1 = 0, gx2 = 0, gy2 = 0;
                    if (!resolveAiRegion(gx1, gy1, gx2, gy2))
                        return L"[错误] 网格定位无法确定截图区域。";

                    AiUiLayoutRect anchorRect{};
                    bool firstLocate = false;
                    if (!AiUiLayoutRecall(key, anchorRect)) {
                        // 第一次：真识图一次（顺带把锚点格点了 —— 与 cells 里的 (0,0) 语义一致）
                        // 锚点注定要反复用 → 先标成可复用目标，别让「一次性目标不写缓存」
                        // 的规矩把第 2 次 grid 调用又逼回识图（还会多点在锚点格一下）。
                        MarkLocateTargetReusable(anchor);
                        const std::wstring r = locateAndClickFn(anchor, 1, button, clickCount);
                        if (r.rfind(L"[错误]", 0) == 0) return r;
                        if (!lastLocateScreenRect.valid())
                            return L"[错误] 网格锚点定位成功但拿不到坐标（内部状态异常）。";
                        anchorRect = lastLocateScreenRect;
                        firstLocate = true;
                    }

                    AiUiGridSpec grid{};
                    if (!AiUiLayoutRecallGrid(key, grid)) {
                        HBITMAP frame = captureAiRegionBmp(gx1, gy1, gx2, gy2);
                        std::vector<uint8_t> grayBytes;
                        int gw = 0, gh = 0, gstride = 0;
                        const bool gotGray = frame
                            && AiUiBitmapToGrayBytes(frame, grayBytes, gw, gh, gstride);
                        if (frame) DeleteBitmapHandle(frame);
                        if (!gotGray) {
                            return L"[错误] 网格定位截图失败。";
                        }
                        AiUiLayoutRect boxInFrame{ anchorRect.x1 - gx1, anchorRect.y1 - gy1,
                            anchorRect.x2 - gx1, anchorRect.y2 - gy1 };
                        int px = 0, py = 0;
                        if (!AiUiDetectGridPeriod(grayBytes.data(), gw, gh, gstride,
                                boxInFrame, 16, 420, px, py)) {
                            return L"[错误] 这个界面看不出规则网格（周期检测不显著）。"
                                   L"改用 locateAndClick(targets=[…]) 逐个目标描述，或直接给屏幕目标。";
                        }
                        grid.originX = anchorRect.cx();
                        grid.originY = anchorRect.cy();
                        grid.stepX = px;
                        grid.stepY = py;
                        grid.cols = px > 0 ? (std::max)(1, (gx2 - grid.originX) / px) : 0;
                        grid.rows = py > 0 ? (std::max)(1, (gy2 - grid.originY) / py) : 0;
                        // ★合理性闸：任何真实界面都不会有几十列格子。实测在暂停菜单的
                        // 石纹背景上误报「周期 16×16px → 79 列 / 19 行」，照它点就是乱点。
                        if (grid.cols > 24 || grid.rows > 12) {
                            AppendAiDebugLog(L"  [诊断] 网格推断被拒：周期 "
                                + std::to_wstring(px) + L"×" + std::to_wstring(py)
                                + L"px 推出 " + std::to_wstring(grid.cols) + L" 列 / "
                                + std::to_wstring(grid.rows) + L" 行，明显是纹理不是网格");
                            grid = AiUiGridSpec{};
                        }
                        if (!grid.valid())
                            return L"[错误] 网格周期无效（stepX/stepY 都为 0）。";
                        AiUiLayoutRememberGrid(key, grid);
                        AppendAiDebugLog(L"  [诊断] 网格已建立：锚点("
                            + std::to_wstring(grid.originX) + L"," + std::to_wstring(grid.originY)
                            + L") 周期 " + std::to_wstring(px) + L"×" + std::to_wstring(py)
                            + L"px 右侧 " + std::to_wstring(grid.cols) + L" 列 / 下方 "
                            + std::to_wstring(grid.rows) + L" 行");
                    }

                    std::vector<std::pair<int, int>> pts;
                    std::wstring skipped;
                    for (const auto& rc : cells) {
                        if (grid.stepX == 0 && rc.second != 0) {
                            skipped += L"(列周期未知 r" + std::to_wstring(rc.first) + L"c"
                                + std::to_wstring(rc.second) + L")";
                            continue;
                        }
                        if (grid.stepY == 0 && rc.first != 0) {
                            skipped += L"(行周期未知 r" + std::to_wstring(rc.first) + L"c"
                                + std::to_wstring(rc.second) + L")";
                            continue;
                        }
                        int x = 0, y = 0;
                        if (!AiUiGridCellCenter(grid, rc.first, rc.second, x, y)) continue;
                        if (x < gx1 + 2 || x >= gx2 - 2 || y < gy1 + 2 || y >= gy2 - 2) {
                            skipped += L"(越界 r" + std::to_wstring(rc.first) + L"c"
                                + std::to_wstring(rc.second) + L")";
                            continue;
                        }
                        pts.emplace_back(x, y);
                    }
                    if (pts.empty()) {
                        return L"[错误] 没有可点的格子。" + skipped
                            + L" 网格：周期 " + std::to_wstring(grid.stepX) + L"×"
                            + std::to_wstring(grid.stepY) + L"px，右侧 "
                            + std::to_wstring(grid.cols) + L" 列 / 下方 "
                            + std::to_wstring(grid.rows) + L" 行。"
                              L"请把 (行,列) 收在有效范围内（可为负=锚点左上方向）。";
                    }
                    // 一次批量执行：moveMouse+mouseClick 交替，中间不插观察/验收
                    std::wstring json = L"[";
                    for (size_t i = 0; i < pts.size(); ++i) {
                        const std::wstring one = BuildScreenClickActionsJson(
                            pts[i].first, pts[i].second, false, button, clickCount);
                        // 去掉两端的 [ ] 拼成一个批次
                        std::wstring body = one;
                        if (!body.empty() && body.front() == L'[') body.erase(body.begin());
                        if (!body.empty() && body.back() == L']') body.pop_back();
                        if (i) json += L",";
                        json += body;
                    }
                    json += L"]";
                    const std::wstring execMsg = executeActionsJsonNow(json);
                    AppendAiTaskMemoLine(L"done: 网格点击 ×" + std::to_wstring(pts.size()));
                    std::wstring out = L"locateAndClick(grid) 已按网格连点 "
                        + std::to_wstring(pts.size()) + L" 格（锚点「"
                        + TruncLog(anchor, 20) + L"」周期 "
                        + std::to_wstring(grid.stepX) + L"×" + std::to_wstring(grid.stepY)
                        + L"px）";
                    if (firstLocate) out += L"；锚点是本次新定位的，之后同一界面直接用记忆坐标";
                    else out += L"；锚点走布局记忆（未重新识图）";
                    if (!skipped.empty()) out += L"；跳过：" + skipped;
                    if (!execMsg.empty()) out += L"；" + execMsg;
                    return out;
                };
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
                        const std::wstring r = locateAndClickFn(targets[i], 1, button, clickCount);
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

                    return L"[错误] switchWindow.action 须为 openPreview|move|confirm|cancel";
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
                        return;
                    }
                    if (ar.actionsAlreadyExecuted) {
                        releaseAltTabIfHeld();
                        AppendAiDebugLog(L"AI动作执行 [" + effModel + L"]：Agent 闭环结束 → "
                            + Trim(ar.textResult));
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

            // 每次开始跑脚本都清掉「上一帧命中」：绝不让上一次运行的命中影响这一次
            ResetFindImageFastPath();
            // 布局记忆同理：上一次运行的坐标/网格一律作废（窗口换了、分辨率换了都会错）
            AiUiLayoutClear();
            // 文字直点用的 OCR 行表同理：上一轮的字坐标一律作废
            ResetOcrScreenIndex();
            // 「一次性目标」计数同理：上次运行点过什么与这一次无关
            ResetLocateTargetSeen();

            bool timelineInterrupted = false;
            bool wmTargetLostLogged = false;
            auto wmAbortIfTargetLost = [this, wmExecPtr, &wmTargetLostLogged]() -> bool {
                if (!wmExecPtr || !wmExecPtr->IsActive()) return false;
                if (wmExecPtr->TargetStillAlive()) return false;
                if (!wmTargetLostLogged) {
                    wmTargetLostLogged = true;
                    windowmode::WindowModeLog(
                        std::wstring(L"[窗口模式] 目标窗口已消失（进程退出/闪退），停止脚本 ")
                        + wmExecPtr->TargetAliveDebug());
                    AppendDebugLog(L"窗口模式：目标已闪退或关闭，已停止（不会对失效窗口继续记步）");
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
                        AppendDebugLog(L"嵌套运行结束：恢复主宏窗口模式失败：" + err);
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
                std::wstring wmErr;
                if (!beginWmCfg(cfg, nestedPath, wmErr)) {
                    AppendDebugLog(L"嵌套运行失败：窗口模式启动失败 " + wmErr);
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
                        RepairCompressedRelativeGaps(nested);
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
                    }
                    runRange(0, nested.size());
                    if (isPlayback) playbackTimeScale = prevScale;
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

            executeOne = [this, &usesOcr, &holdOcrSession, &heldKeyVk, &heldKeys, &runRange, &runningScriptPath, &activeActions, &lockedScreen_, &lockedVirtX_, &lockedVirtY_, &clearLockedScreen, &makeVarCtx, &resolveTemplatePath, &executeOne, &runAiActionExecute, &aiSessions, &aiLoopDepth, &pendingBreakLoop, wmExecPtr, &wmSetPos, &wmSetLivePos, &wmSendKey, &wmSendHeldModifiers, &wmMouseButton, &wmMouseClick, &activateDesktopAt, &wmSendShortcut, &isImeToggleShortcut, &wmUsesTarget, &wmUsesBackground, &activeCoordMeta, &currentTmplScale, execTargetW, execTargetH, &inputTimeline, &waitAbsoluteTimeline, &wmAbortIfTargetLost, &playbackTimeScale, imageVarRunId, &scheduledYieldDepth, &scheduledYieldLocalStop, &fireImageWatches, &sleepWithTimeWatches, &sleepRepeatInterval, &pendingGoto, &findLocateAnchor, &matchScriptImageAll, &runNestedLibrary, &keepCursorAtFind, &lastFindX, &lastFindY, &loggedRdpFindPin_, &applyFindCursor](const ScriptAction& a) {
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
                    if (KeyFunctionDebugActive()) {
                        AppendDeferredMoveRelDebug(a, dx, dy);
                    }
                    if (wmUsesTarget()) {
                        const bool hw = wmExecPtr->PreferHardwareInput();
                        if (hw) MarkSimulatedInput();
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
                    // 窗口模式 CDP/扩展键鼠不走本机 SendInput，勿 Mark（否则脱离检测会误判忙碌）。
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
                        // 窗口模式 soft/CDP 输入直接投递给目标窗口，不经系统 IME，无需准备
                        wmExecPtr->SendQuickInputToTarget(text,
                            quickscript::ScalePlaybackTimeSeconds(a.charInterval, playbackTimeScale));
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            wchar_t buf[160]{};
                            swprintf_s(buf, L"快捷输入→目标窗口 hwnd=0x%p%s",
                                wmExecPtr->TargetHwnd(),
                                wmUsesBackground() ? L" [后台窗口模式]" : L" [窗口模式]");
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
                                    L"找图诊断(窗口模式) 无匹配 %dms bestNcc=%.1f%% pixelAgree=%.1f%% "
                                    L"（应走扩展/客户区；若见全屏找图调试则窗口模式未激活）",
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
                                    L"找图跳过：动作为窗口相对，但窗口模式未绑定；"
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
                            // 窗口模式不走这里，由 FindImageClient / ResolveClientSearchRect 用全客户区。
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
                        // ── 快速路径：上一帧命中点周围的小窗口本地复核 ──
                        // 只在「同一请求 + 上一帧搜索很慢（区域找图不值得）+ 命中新鲜」时尝试；
                        // 过了就直接用，没过**当场回退全屏搜索**（所以精度上限不变，
                        // 最坏只是多花一次小窗口的时间）。窗口内找不到/分数余量不足都会回退。
                        const FindImageFastPathParams fastParams{};
                        const std::wstring fastKey = (!wmUsesTarget() && !a.windowRelative)
                            ? FindImageFastPathKey(findAct.imagePath, opt, x1, y1, x2, y2,
                                  findPrep.templateW, findPrep.templateH, execTargetW, execTargetH)
                            : std::wstring();
                        ImageMatchOutput output;
                        bool fastAccepted = false;
                        if (!fastKey.empty()) {
                            const auto it = g_findImageFastPath.find(fastKey);
                            if (it != g_findImageFastPath.end()) {
                                const long long ageMs = std::chrono::duration_cast<
                                    std::chrono::milliseconds>(std::chrono::steady_clock::now()
                                        - it->second.at).count();
                                int wx1 = 0, wy1 = 0, wx2 = 0, wy2 = 0;
                                if (PlanFindImageFastPath(fastParams, x1, y1, x2, y2,
                                        it->second.prevTLX, it->second.prevTLY,
                                        it->second.tplW, it->second.tplH,
                                        it->second.lastFullSearchMs, ageMs, wx1, wy1, wx2, wy2)) {
                                    ImageMatchOutput fastOut;
                                    if (lockedScreen_) {
                                        fastOut = FindTemplateInFrozenScreenMulti(
                                            lockedScreen_, lockedVirtX_, lockedVirtY_,
                                            wx1, wy1, wx2, wy2, tmpl, opt);
                                    } else {
                                        fastOut = FindTemplateOnScreenMulti(wx1, wy1, wx2, wy2, tmpl, opt);
                                    }
                                    if (!fastOut.matches.empty()) {
                                        const ImageMatchResult& fm = fastOut.matches.front();
                                        if (AcceptFindImageFastPathHit(fastParams,
                                                it->second.prevTLX, it->second.prevTLY,
                                                fm.topLeftX, fm.topLeftY,
                                                opt.thresholdPercent, fm.score)) {
                                            output = fastOut;
                                            fastAccepted = true;
                                            lastFindMs = fastOut.elapsedMs;
                                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                                wchar_t buf[256]{};
                                                swprintf_s(buf,
                                                    L"找图快速路径 命中 %dms（上次全屏 %dms）tl=(%d,%d) "
                                                    L"score=%.1f 漂移=(%d,%d)",
                                                    fastOut.elapsedMs,
                                                    static_cast<int>(it->second.lastFullSearchMs),
                                                    fm.topLeftX, fm.topLeftY, fm.score,
                                                    fm.topLeftX - it->second.prevTLX,
                                                    fm.topLeftY - it->second.prevTLY);
                                                AppendDebugLog(buf);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if (!fastAccepted) {
                            output = doMatch(tmpl, opt);
                            lastFindMs = output.elapsedMs;
                        }
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
                            // 全屏也没找到 → 旧命中已失效，别让下一步再白试小窗口
                            if (!fastKey.empty()) g_findImageFastPath.erase(fastKey);
                            return {};
                        }
                        // 记下这一帧的命中，供下次同请求做本地复核。
                        // 快速路径命中时保留上次的全屏耗时（它代表「这个请求本来就慢」），
                        // 全屏命中时刷新为本次耗时。
                        if (!fastKey.empty()) {
                            FindImageFastPathEntry& entry = g_findImageFastPath[fastKey];
                            entry.prevTLX = output.matches.front().topLeftX;
                            entry.prevTLY = output.matches.front().topLeftY;
                            entry.tplW = findPrep.templateW;
                            entry.tplH = findPrep.templateH;
                            entry.at = std::chrono::steady_clock::now();
                            if (!fastAccepted) entry.lastFullSearchMs = output.elapsedMs;
                            if (g_findImageFastPath.size() > kFindImageFastPathMaxEntries) {
                                g_findImageFastPath.clear();
                                g_findImageFastPath[fastKey] = entry;
                            }
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
                        // 可中断等待；窗口模式单次找图常 1~2s，重试间隔宜短以便热键立刻停
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
                                    ? MakeOcrSearchVarResult(OcrTextLine{}, false)
                                    : MakeOcrTextVarResult(L"");
                            }
                            output = RunOcrOnScreenRegion(
                                x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_, a.ocrDigitsOnly);
                        }
                        if (!output.success) {
                            return a.ocrResultMode == 1
                                ? MakeOcrSearchVarResult(OcrTextLine{}, false)
                                : MakeOcrTextVarResult(L"");
                        }
                        if (a.ocrResultMode == 0) {
                            return MakeOcrTextVarResult(ConcatOcrLines(output));
                        }
                        MacroVariableContext ctx = makeVarCtx();
                        const std::wstring target = ResolveMacroVariables(a.ocrSearchText, ctx);
                        const auto found = FindTextInOcrLines(output, target);
                        if (found.has_value()) return MakeOcrSearchVarResult(*found, true);
                        return MakeOcrSearchVarResult(OcrTextLine{}, false);
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
                    auto emitOcrDebug = [&](const std::wstring& textContent, bool searchFound) {
                        if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                            AppendDebugLog(FormatOcrDebug(a, textContent, searchFound, makeVarCtx()));
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
                                emitOcrDebug(L"", false);
                                return;
                            }
                            output = RunOcrOnScreenRegion(
                                x1, y1, x2, y2, lockedScreen_, lockedVirtX_, lockedVirtY_, a.ocrDigitsOnly);
                        }
                        const std::wstring text = output.success ? ConcatOcrLines(output) : L"";
                        ocrVars_[varName] = MakeOcrTextVarResult(text);
                        emitOcrDebug(text, false);
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
                        emitOcrDebug(result.text, false);
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
                        emitOcrDebug(lastResult.text, lastResult.found != 0);
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
                    if (gotPos) {
                        const std::wstring varName = a.matchVarName.empty() ? L"a" : a.matchVarName;
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
                    }
                }
                else if (a.type == ActionType::VarCompute) {
                    MacroVariableContext ctx = makeVarCtx();
                    const VarComputeResult vr = RunVarCompute(a.computeCode, ctx, &stopFlag_);
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
                    }
                }
                else if (a.type == ActionType::FindColor) {
                    int x1 = a.searchX1, y1 = a.searchY1, x2 = a.searchX2, y2 = a.searchY2;
                    HBITMAP colorBmp = lockedScreen_;
                    int colorVx = lockedVirtX_, colorVy = lockedVirtY_;
                    HBITMAP colorTmp = nullptr;
                    if (a.imageLocate) {
                        ImageMatchResult loc{};
                        int tplW = 0, tplH = 0;
                        if (!findLocateAnchor(a, loc, tplW, tplH)) {
                            const std::wstring varName = a.matchVarName.empty() ? L"colorRet" : a.matchVarName;
                            matchVars_[varName] = {};
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                AppendDebugLog(L"找色未命中（未找到定位图） "
                                    + FormatColorHex(a.colorR, a.colorG, a.colorB));
                            }
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
                            const std::wstring varName = a.matchVarName.empty() ? L"colorRet" : a.matchVarName;
                            matchVars_[varName] = {};
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                AppendDebugLog(L"找色未命中（无法解析目标窗口客户区）");
                            }
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
                            const std::wstring varName = a.matchVarName.empty() ? L"colorRet" : a.matchVarName;
                            matchVars_[varName] = {};
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                AppendDebugLog(L"找色未命中（无法截取目标窗口）");
                            }
                            return;
                        }
                    }
                    const ColorMatchHit hit = FindColorInScreenRegion(
                        x1, y1, x2, y2, a.colorR, a.colorG, a.colorB, a.colorTolerance,
                        colorBmp, colorVx, colorVy, 2, &stopFlag_);
                    if (colorTmp) DeleteBitmapHandle(colorTmp);
                    if (StopRequested()) return;
                    const std::wstring varName = a.matchVarName.empty() ? L"colorRet" : a.matchVarName;
                    ImageMatchResult match{};
                    if (hit.found) {
                        match.found = true;
                        match.x = hit.x;
                        match.y = hit.y;
                        match.topLeftX = hit.x;
                        match.topLeftY = hit.y;
                        match.bottomRightX = hit.x;
                        match.bottomRightY = hit.y;
                        match.score = 100.0 - hit.distance;
                        aiVars_[varName] = FormatColorHex(hit.r, hit.g, hit.b);
                        const int tx = hit.x + a.offsetX;
                        const int ty = hit.y + a.offsetY;
                        if (a.findImageFollowUp == 0) {
                            applyFindCursor(tx, ty, true, a.button, a);
                        } else if (a.findImageFollowUp == 1) {
                            applyFindCursor(tx, ty, false, a.button, a);
                        }
                    }
                    matchVars_[varName] = match;
                    if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                        AppendDebugLog(hit.found
                            ? (L"找色命中 " + FormatColorHex(a.colorR, a.colorG, a.colorB)
                                + L" @ " + std::to_wstring(hit.x) + L"," + std::to_wstring(hit.y))
                            : (L"找色未命中 " + FormatColorHex(a.colorR, a.colorG, a.colorB)));
                    }
                }
                else if (a.type == ActionType::ColorMatch) {
                    MacroVariableContext ctx = makeVarCtx();
                    int px = a.x, py = a.y;
                    if (a.imageLocate) {
                        ImageMatchResult loc{};
                        int tplW = 0, tplH = 0;
                        if (!findLocateAnchor(a, loc, tplW, tplH)) {
                            const std::wstring varName = a.matchVarName.empty() ? L"colorRet" : a.matchVarName;
                            ImageMatchResult miss{};
                            miss.found = false;
                            matchVars_[varName] = miss;
                            if (appSettings_.playback.autoOutputKeyFunctionDebug) {
                                AppendDebugLog(L"颜色匹配失败（未找到定位图）");
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
                    const bool matched = MatchColorAtScreenPoint(px, py,
                        a.colorR, a.colorG, a.colorB, a.colorTolerance,
                        &r, &g, &b, &dist, lockedScreen_, lockedVirtX_, lockedVirtY_);
                    const std::wstring varName = a.matchVarName.empty() ? L"colorRet" : a.matchVarName;
                    ImageMatchResult match{};
                    match.found = matched;
                    match.x = px;
                    match.y = py;
                    match.topLeftX = px;
                    match.topLeftY = py;
                    match.bottomRightX = px;
                    match.bottomRightY = py;
                    match.score = matched ? (100.0 - dist) : 0.0;
                    matchVars_[varName] = match;
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
                    RepairCompressedRelativeGaps(nested);
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
                // 每轮独立统计，避免「第2轮 SendInput ok=上轮累计」误导。
                if (inputTimeline.enabled) MouseInputRouter::Instance().ResetStats();
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
                runRange(0, actions.size());
                if (inputTimeline.enabled && KeyFunctionDebugActive()) {
                    const auto st = inputTimeline.precision.Stats();
                    const auto ms = MouseInputRouter::Instance().Stats();
                    wchar_t summary[400]{};
                    swprintf_s(summary,
                        L"[时间轴统计] waits=%llu late>1ms=%llu p95=%lluus max=%lluus rebase=%llu | "
                        L"SendInput ok=%llu fail=%llu paced=%llu | "
                        L"ballistics=%s split=%s",
                        static_cast<unsigned long long>(st.eventCount),
                        static_cast<unsigned long long>(st.lateEventCount),
                        static_cast<unsigned long long>(st.p95LateUs),
                        static_cast<unsigned long long>(st.maxLateUs),
                        static_cast<unsigned long long>(st.rebaseCount),
                        static_cast<unsigned long long>(ms.sentEvents),
                        static_cast<unsigned long long>(ms.failedEvents),
                        static_cast<unsigned long long>(ms.pacedWaits),
                        ballisticsGuard.FlatVerified() ? L"flat" : L"accel?",
                        splitLargeMoves ? L"on" : L"off");
                    AppendDebugLog(summary);
                    if (timelineInterrupted || StopRequested()) {
                        AppendDebugLog(
                            StopRequested()
                                ? L"[时间轴] 本轮未跑完：已停止"
                                : L"[时间轴] 本轮未跑完：等待被跳出打断");
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
