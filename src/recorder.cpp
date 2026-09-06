// ──────────────────────────────────────────────────────────────────
// recorder.cpp — 鼠标/键盘录制基础设施实现
// 绝对模式（光标可见）：WH_MOUSE_LL 记录屏幕坐标 → MoveMouse
// 相对模式（光标隐藏 / ClipCursor）：Raw Input lLastX/Y → MoveMouseRelative
// 计时统一用 QPC 微秒，避免 GetTickCount(~15ms) 导致等待台阶化
// ──────────────────────────────────────────────────────────────────

#include "recorder.h"

#include "image_match.h"
#include "input/input_emergency_teardown.h"
#include "recording_to_findimage.h"
#include "utils.h"

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

std::vector<RecordedEvent> g_recordedEvents;
std::mutex g_recordMutex;
std::atomic_bool g_recording{false};
constexpr int kMaxRecordingIgnoreHotkeys = 96;
RecordingIgnoreChord g_recordingIgnoreList[kMaxRecordingIgnoreHotkeys]{};
int g_recordingIgnoreCount = 0;
std::mutex g_recordingIgnoreMu;
HHOOK g_keyboardHook = nullptr;
HHOOK g_mouseHook = nullptr;

namespace {

std::atomic<int64_t> g_recordStartQpc{0};
std::atomic<int64_t> g_qpcFreq{0};
std::atomic<uint64_t> g_lastAbsMoveUs{0};
std::atomic<uint64_t> g_lastRelStampUs{0};
/// 相对移动报告间隔 EMA（µs）；队列积压时用它拉开时间戳，贴近 HID 真实间距。
std::atomic<uint64_t> g_relReportIntervalUs{8000};
std::atomic<uint64_t> g_eventSequence{0};
std::atomic<RecordingCaptureMode> g_captureMode{RecordingCaptureMode::Auto};
/// Auto 模式：最近一次判定为相对采集的录制时间戳；用于短时粘滞，避免进游戏后光标闪一下就混入绝对 Move。
std::atomic<uint64_t> g_lastRelativeActiveUs{0};
constexpr uint64_t kAutoRelativeStickyUs = 250000; // 250ms

std::atomic<int> g_captureScope{1}; // 默认全局，与「未配置」安全侧一致；Start 前由设置覆盖
std::mutex g_scopeMu;
HWND g_scopeRoot = nullptr;
bool g_scopeArmOnExternal = false;
DWORD g_scopeOwnPid = 0;

std::mutex g_wmTargetMu;
RecordingWindowTarget g_wmTarget;

HWND RootHwndOf(HWND h) {
    if (!h || !IsWindow(h)) return nullptr;
    HWND r = GetAncestor(h, GA_ROOT);
    return r ? r : h;
}

bool SameRootTree(HWND a, HWND b) {
    HWND ra = RootHwndOf(a);
    HWND rb = RootHwndOf(b);
    return ra && rb && ra == rb;
}

bool PidOfHwnd(HWND h, DWORD* outPid) {
    if (!h || !outPid) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    *outPid = pid;
    return pid != 0;
}

std::atomic_bool g_rawThreadRun{false};
std::thread g_rawThread;
HWND g_rawHwnd = nullptr;
const wchar_t kRawWndClass[] = L"QST.RecordRawInput.Wnd";
std::mutex g_rawReadyMutex;
std::condition_variable g_rawReadyCv;
bool g_rawReady = false;
bool g_rawRegistrationOk = false;

/// 录制中已按下的 VK（过滤键盘自动重复 KEYDOWN）
std::bitset<256> g_keysHeld;

std::mutex g_recordingDebugMu;
RecordingDebugSink g_recordingDebugSink;
std::atomic<uint64_t> g_dbgKeyDown{0};
std::atomic<uint64_t> g_dbgKeyUp{0};
std::atomic<uint64_t> g_dbgKeyRepeatSkipped{0};
std::atomic<uint64_t> g_dbgMouseDown{0};
std::atomic<uint64_t> g_dbgMouseUp{0};
std::atomic<uint64_t> g_dbgWheel{0};
std::atomic<uint64_t> g_dbgAbsMove{0};
std::atomic<uint64_t> g_dbgRelMove{0};

void EmitRecordingDebug(const std::wstring& line) {
    RecordingDebugSink sink;
    {
        std::lock_guard<std::mutex> lock(g_recordingDebugMu);
        sink = g_recordingDebugSink;
    }
    if (sink) sink(line);
}

std::wstring FormatRecordingStamp(uint64_t timeUs) {
    wchar_t buf[48]{};
    swprintf_s(buf, L"[录制 t=%.3fs] ", static_cast<double>(timeUs) / 1000000.0);
    return buf;
}

const wchar_t* RecordedMouseButtonName(WPARAM vk) {
    switch (vk) {
    case VK_LBUTTON: return L"左键";
    case VK_RBUTTON: return L"右键";
    case VK_MBUTTON: return L"中键";
    case VK_XBUTTON1: return L"侧键1";
    case VK_XBUTTON2: return L"侧键2";
    default: return L"鼠标键";
    }
}

std::atomic<int> g_highResTimerDepth{0};

// ── 点击自动截模板异步队列 ──────────────────────────────────────
constexpr size_t kClickCaptureQueueMax = 64;

struct ClickCaptureRequest {
    uint64_t sequence = 0;
    HBITMAP bmp = nullptr; // 队列持有，worker 写盘后 DeleteObject
    int offsetX = 0;
    int offsetY = 0;
};

struct HoverPatch {
    HBITMAP bmp = nullptr;
    int cx = 0;
    int cy = 0;
    HWND hwnd = nullptr;
    ClickCaptureRectResult rect{};
    DWORD tick = 0;
};

std::mutex g_captureMutex;
std::condition_variable g_captureCv;
std::deque<ClickCaptureRequest> g_captureQueue;
std::atomic_bool g_captureWorkerRun{false};
std::atomic_bool g_captureAccepting{false};
std::atomic_int g_captureInFlight{0};
std::atomic_bool g_captureEnabled{true};
std::atomic_bool g_captureSkipRelative{false};  // 图片定位：相对事件不截图
std::atomic_int g_captureHalfSize{40};
std::atomic_uint64_t g_captureSessionId{0};
std::atomic_uint32_t g_captureSessionSalt{0};
std::atomic_uint64_t g_captureDropped{0};
std::thread g_captureWorker;

std::mutex g_hoverMu;
HoverPatch g_hover;
std::atomic<DWORD> g_hoverLastUpdateTick{0};
constexpr DWORD kHoverPatchMinIntervalMs = 16;
constexpr int kHoverPatchMaxClickDeltaPx = 8;

bool ShouldCaptureRelativeNow();

HBITMAP CloneHbitmap(HBITMAP src) {
    if (!src) return nullptr;
    BITMAP bm{};
    if (!GetObjectW(src, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        return nullptr;
    }
    HDC screenDc = GetDC(nullptr);
    if (!screenDc) return nullptr;
    HDC srcDc = CreateCompatibleDC(screenDc);
    HDC dstDc = CreateCompatibleDC(screenDc);
    HBITMAP dst = CreateCompatibleBitmap(screenDc, bm.bmWidth, bm.bmHeight);
    if (!srcDc || !dstDc || !dst) {
        if (dst) DeleteObject(dst);
        if (srcDc) DeleteDC(srcDc);
        if (dstDc) DeleteDC(dstDc);
        ReleaseDC(nullptr, screenDc);
        return nullptr;
    }
    HGDIOBJ oldSrc = SelectObject(srcDc, src);
    HGDIOBJ oldDst = SelectObject(dstDc, dst);
    BitBlt(dstDc, 0, 0, bm.bmWidth, bm.bmHeight, srcDc, 0, 0, SRCCOPY);
    SelectObject(srcDc, oldSrc);
    SelectObject(dstDc, oldDst);
    DeleteDC(srcDc);
    DeleteDC(dstDc);
    ReleaseDC(nullptr, screenDc);
    return dst;
}

void ClearHoverPatchLocked() {
    if (g_hover.bmp) {
        DeleteObject(g_hover.bmp);
        g_hover.bmp = nullptr;
    }
    g_hover = {};
}

void DrainCaptureQueueLocked() {
    while (!g_captureQueue.empty()) {
        ClickCaptureRequest old = g_captureQueue.front();
        g_captureQueue.pop_front();
        if (old.bmp) DeleteObject(old.bmp);
    }
}

bool GrabVisibleClickPatch(int cx, int cy, HWND clientHwnd, int half,
    HBITMAP& outBmp, ClickCaptureRectResult& outRect) {
    outBmp = nullptr;
    outRect = {};
    half = ClampClickCaptureHalfSize(half);
    if (clientHwnd && IsWindow(clientHwnd)) {
        RECT cr{};
        if (!GetClientRect(clientHwnd, &cr) || cr.right <= 0 || cr.bottom <= 0) return false;
        outRect = ComputeClickCaptureRect(cx, cy, half, 0, 0, cr.right, cr.bottom);
        if (!outRect.valid) return false;
        POINT tl{outRect.x1, outRect.y1};
        POINT br{outRect.x2, outRect.y2};
        if (!ClientToScreen(clientHwnd, &tl) || !ClientToScreen(clientHwnd, &br)) return false;
        outBmp = CaptureScreenRegion(tl.x, tl.y, br.x, br.y);
        return outBmp != nullptr;
    }
    int vsX = 0, vsY = 0, vsW = 0, vsH = 0;
    GetVirtualScreenRect(vsX, vsY, vsW, vsH);
    outRect = ComputeClickCaptureRect(cx, cy, half, vsX, vsY, vsX + vsW, vsY + vsH);
    if (!outRect.valid) return false;
    outBmp = CaptureScreenRegion(outRect.x1, outRect.y1, outRect.x2, outRect.y2);
    return outBmp != nullptr;
}

void MaybeUpdateHoverPatch(int cx, int cy, HWND clientHwnd) {
    if (!g_captureAccepting.load(std::memory_order_relaxed)) return;
    if (!g_captureEnabled.load(std::memory_order_relaxed)) return;
    if (g_captureSkipRelative.load(std::memory_order_relaxed)
        && ShouldCaptureRelativeNow()) return;
    const DWORD now = GetTickCount();
    const DWORD last = g_hoverLastUpdateTick.load(std::memory_order_relaxed);
    if (last != 0 && (now - last) < kHoverPatchMinIntervalMs) {
        std::lock_guard<std::mutex> lock(g_hoverMu);
        if (g_hover.bmp && HoverPatchCoversClick(g_hover.cx, g_hover.cy, cx, cy,
            kHoverPatchMaxClickDeltaPx)) {
            return;
        }
    }
    const int half = g_captureHalfSize.load(std::memory_order_relaxed);
    HBITMAP bmp = nullptr;
    ClickCaptureRectResult rect{};
    if (!GrabVisibleClickPatch(cx, cy, clientHwnd, half, bmp, rect)) return;
    {
        std::lock_guard<std::mutex> lock(g_hoverMu);
        ClearHoverPatchLocked();
        g_hover.bmp = bmp;
        g_hover.cx = cx;
        g_hover.cy = cy;
        g_hover.hwnd = clientHwnd;
        g_hover.rect = rect;
        g_hover.tick = now;
    }
    g_hoverLastUpdateTick.store(now, std::memory_order_relaxed);
}

HBITMAP TakePreClickPatch(int cx, int cy, HWND clientHwnd, ClickCaptureRectResult& outRect) {
    outRect = {};
    {
        std::lock_guard<std::mutex> lock(g_hoverMu);
        if (g_hover.bmp
            && g_hover.hwnd == clientHwnd
            && HoverPatchCoversClick(g_hover.cx, g_hover.cy, cx, cy, kHoverPatchMaxClickDeltaPx)) {
            outRect = g_hover.rect;
            return CloneHbitmap(g_hover.bmp);
        }
    }
    const int half = g_captureHalfSize.load(std::memory_order_relaxed);
    HBITMAP bmp = nullptr;
    if (!GrabVisibleClickPatch(cx, cy, clientHwnd, half, bmp, outRect)) return nullptr;
    return bmp;
}

void EnsureCaptureWorkerStarted() {
    bool expected = false;
    if (!g_captureWorkerRun.compare_exchange_strong(expected, true)) return;
    g_captureWorker = std::thread([]() {
        while (g_captureWorkerRun.load(std::memory_order_relaxed)) {
            ClickCaptureRequest req{};
            {
                std::unique_lock<std::mutex> lock(g_captureMutex);
                g_captureCv.wait(lock, []() {
                    return !g_captureQueue.empty()
                        || !g_captureWorkerRun.load(std::memory_order_relaxed);
                });
                if (g_captureQueue.empty()) continue;
                req = g_captureQueue.front();
                g_captureQueue.pop_front();
            }
            g_captureInFlight.fetch_add(1, std::memory_order_relaxed);
            if (req.bmp) {
                EnsureFindImagesDir();
                const uint64_t sessionId = g_captureSessionId.load(std::memory_order_relaxed);
                const std::wstring fileName = MakeRecordingClickCaptureFileName(sessionId, req.sequence);
                const std::wstring path = FindImagesDir() + L"\\" + fileName;
                if (SaveBitmapToFile(req.bmp, path)) {
                    std::lock_guard<std::mutex> lock(g_recordMutex);
                    for (auto& ev : g_recordedEvents) {
                        if (ev.sequence == req.sequence) {
                            ev.capturePath = path;
                            ev.captureOffsetX = req.offsetX;
                            ev.captureOffsetY = req.offsetY;
                            break;
                        }
                    }
                }
                DeleteObject(req.bmp);
                req.bmp = nullptr;
            }
            g_captureInFlight.fetch_sub(1, std::memory_order_relaxed);
            g_captureCv.notify_all();
        }
    });
}

void EnqueueClickCapture(uint64_t sequence, int cx, int cy, HWND clientHwnd) {
    if (!g_captureAccepting.load(std::memory_order_relaxed)) return;
    if (!g_captureEnabled.load(std::memory_order_relaxed)) return;
    // 图片定位：处于相对采集阶段时不截图（相对移动/点击无找图转换意义）
    if (g_captureSkipRelative.load(std::memory_order_relaxed)
        && ShouldCaptureRelativeNow()) return;
    ClickCaptureRectResult rect{};
    HBITMAP bmp = TakePreClickPatch(cx, cy, clientHwnd, rect);
    if (!bmp) return;
    EnsureCaptureWorkerStarted();
    ClickCaptureRequest req{};
    req.sequence = sequence;
    req.bmp = bmp;
    req.offsetX = rect.offsetX;
    req.offsetY = rect.offsetY;
    {
        std::lock_guard<std::mutex> lock(g_captureMutex);
        while (g_captureQueue.size() >= kClickCaptureQueueMax) {
            ClickCaptureRequest old = g_captureQueue.front();
            g_captureQueue.pop_front();
            if (old.bmp) DeleteObject(old.bmp);
            g_captureDropped.fetch_add(1, std::memory_order_relaxed);
        }
        g_captureQueue.push_back(req);
    }
    g_captureCv.notify_one();
}

bool IsButtonDownMsg(UINT msg) {
    return msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN
        || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN;
}

void NoteRecordedMouseButton(const RecordedEvent& ev) {
    const bool down = IsButtonDownMsg(ev.msg);
    if (down) g_dbgMouseDown.fetch_add(1, std::memory_order_relaxed);
    else g_dbgMouseUp.fetch_add(1, std::memory_order_relaxed);
    EmitRecordingDebug(
        FormatRecordingStamp(ev.timeOffsetUs)
        + (down ? L"鼠标按下 " : L"鼠标松开 ")
        + RecordedMouseButtonName(ev.vkOrButton)
        + L" (" + std::to_wstring(ev.x) + L"," + std::to_wstring(ev.y) + L")");
}

bool IsModifierDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool ModifiersMatch(UINT modifiers) {
    const bool needAlt = (modifiers & MOD_ALT) != 0;
    const bool needCtrl = (modifiers & MOD_CONTROL) != 0;
    const bool needShift = (modifiers & MOD_SHIFT) != 0;
    const bool needWin = (modifiers & MOD_WIN) != 0;
    const bool altDown = IsModifierDown(VK_MENU) || IsModifierDown(VK_LMENU) || IsModifierDown(VK_RMENU);
    const bool ctrlDown = IsModifierDown(VK_CONTROL) || IsModifierDown(VK_LCONTROL) || IsModifierDown(VK_RCONTROL);
    const bool shiftDown = IsModifierDown(VK_SHIFT) || IsModifierDown(VK_LSHIFT) || IsModifierDown(VK_RSHIFT);
    const bool winDown = IsModifierDown(VK_LWIN) || IsModifierDown(VK_RWIN);
    if (needAlt != altDown || needCtrl != ctrlDown || needShift != shiftDown || needWin != winDown) return false;
    return true;
}

bool ShouldIgnoreRecordedInput(UINT msg, WPARAM vkOrButton) {
    RecordingIgnoreChord local[kMaxRecordingIgnoreHotkeys]{};
    int count = 0;
    {
        std::lock_guard<std::mutex> lock(g_recordingIgnoreMu);
        count = g_recordingIgnoreCount;
        for (int i = 0; i < count; ++i) local[i] = g_recordingIgnoreList[i];
    }
    if (count <= 0) return false;

    auto chordMatches = [&](const RecordingIgnoreChord& chord) -> bool {
        if (!chord.vk) return false;
        if (chord.vk == VK_LBUTTON) {
            return msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP;
        }
        if (chord.vk == VK_RBUTTON) {
            return msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP;
        }
        if (chord.vk == VK_MBUTTON) {
            return msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP;
        }
        if (chord.vk == VK_XBUTTON1 || chord.vk == VK_XBUTTON2) {
            return msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP;
        }
        if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP) {
            if (static_cast<UINT>(vkOrButton) != chord.vk) return false;
            return ModifiersMatch(chord.modifiers);
        }
        return false;
    };

    for (int i = 0; i < count; ++i) {
        if (chordMatches(local[i])) return true;
    }
    return false;
}

uint64_t NextEventSequence() {
    return g_eventSequence.fetch_add(1, std::memory_order_relaxed);
}

bool ShouldCaptureRelativeNow() {
    switch (g_captureMode.load(std::memory_order_relaxed)) {
    case RecordingCaptureMode::Relative: return true;
    case RecordingCaptureMode::Absolute: return false;
    case RecordingCaptureMode::Auto: default: {
        if (IsRelativeMouseCaptureActive()) {
            g_lastRelativeActiveUs.store(RecordingOffsetUs(), std::memory_order_relaxed);
            return true;
        }
        // 粘滞：刚离开相对态的短时间内仍走 Raw，避免与强制 FPS 相对模式效果不一致。
        const uint64_t last = g_lastRelativeActiveUs.load(std::memory_order_relaxed);
        if (last == 0) return false;
        const uint64_t now = RecordingOffsetUs();
        return now >= last && (now - last) < kAutoRelativeStickyUs;
    }
    }
}

void EmitRelativeMoveEvent(int dx, int dy, uint64_t nowUs) {
    if (dx == 0 && dy == 0) return;
    if (!g_recording.load(std::memory_order_relaxed)) return;
    if (!RecordingScopeAllowsEvent(true, 0, 0)) return; // 相对视角：跟前台窗

    // 队列积压时多个 WM_INPUT 会在短时间内被处理：QPC 戳挤在亚毫秒内。
    // 用 EMA 报告间隔拉开，避免回放时在同一游戏帧内连发。
    uint64_t stamp = nowUs;
    uint64_t prev = g_lastRelStampUs.load(std::memory_order_relaxed);
    uint64_t interval = g_relReportIntervalUs.load(std::memory_order_relaxed);
    if (interval < 1000) interval = 1000;
    if (interval > 16000) interval = 16000;

    if (prev > 0 && stamp > prev) {
        const uint64_t gap = stamp - prev;
        if (gap >= 500 && gap <= 20000) {
            const uint64_t ema = (interval * 7 + gap * 3) / 10;
            g_relReportIntervalUs.store(ema, std::memory_order_relaxed);
            interval = ema;
        } else if (gap < interval / 2 && gap < 2000) {
            stamp = prev + interval;
        }
    } else if (stamp <= prev) {
        stamp = prev + interval;
    }
    g_lastRelStampUs.store(stamp, std::memory_order_relaxed);

    RecordedEvent ev{};
    ev.timeOffsetUs = stamp;
    ev.sequence = NextEventSequence();
    ev.msg = kWmRecordedRelativeMove;
    ev.vkOrButton = 0;
    ev.x = dx;
    ev.y = dy;
    ev.source = RecordedEventSource::RawInput;
    std::lock_guard<std::mutex> lock(g_recordMutex);
    g_recordedEvents.push_back(ev);
    g_dbgRelMove.fetch_add(1, std::memory_order_relaxed);
}

void HandleRawInput(HRAWINPUT hRaw) {
    // 先打时间戳，减少 GetRawInputData 拷贝带来的额外延迟。
    const uint64_t stampUs = RecordingOffsetUs();
    alignas(RAWINPUT) std::array<BYTE, sizeof(RAWINPUT)> buf{};
    UINT size = static_cast<UINT>(buf.size());
    if (GetRawInputData(hRaw, RID_INPUT, buf.data(), &size, sizeof(RAWINPUTHEADER)) != size) {
        return;
    }
    const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buf.data());
    if (!raw || raw->header.dwType != RIM_TYPEMOUSE) return;
    if (raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return;
    EmitRelativeMoveEvent(raw->data.mouse.lLastX, raw->data.mouse.lLastY, stampUs);
}

LRESULT CALLBACK RawInputWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INPUT:
        if (g_recording.load(std::memory_order_relaxed) && ShouldCaptureRelativeNow()) {
            HandleRawInput(reinterpret_cast<HRAWINPUT>(lp));
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

bool RegisterRawInputForHwnd(HWND hwnd) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = hwnd;
    return RegisterRawInputDevices(&rid, 1, sizeof(rid)) == TRUE;
}

void UnregisterRawInput() {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = RIDEV_REMOVE;
    rid.hwndTarget = nullptr;
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
}

void RawInputThreadMain() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = RawInputWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kRawWndClass;
    RegisterClassExW(&wc);

    g_rawHwnd = CreateWindowExW(0, kRawWndClass, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    if (!g_rawHwnd) {
        {
            std::lock_guard<std::mutex> lock(g_rawReadyMutex);
            g_rawReady = true;
            g_rawRegistrationOk = false;
        }
        g_rawReadyCv.notify_all();
        g_rawThreadRun.store(false, std::memory_order_relaxed);
        timeEndPeriod(1);
        return;
    }
    if (!RegisterRawInputForHwnd(g_rawHwnd)) {
        {
            std::lock_guard<std::mutex> lock(g_rawReadyMutex);
            g_rawReady = true;
            g_rawRegistrationOk = false;
        }
        g_rawReadyCv.notify_all();
        DestroyWindow(g_rawHwnd);
        g_rawHwnd = nullptr;
        g_rawThreadRun.store(false, std::memory_order_relaxed);
        timeEndPeriod(1);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_rawReadyMutex);
        g_rawReady = true;
        g_rawRegistrationOk = true;
    }
    g_rawReadyCv.notify_all();

    MSG msg{};
    while (g_rawThreadRun.load(std::memory_order_relaxed)) {
        // 先排空再等待，避免 timeout 路径漏处理已入队的 WM_INPUT。
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_rawThreadRun.store(false, std::memory_order_relaxed);
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_rawThreadRun.load(std::memory_order_relaxed)) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 1, QS_ALLINPUT);
    }

    // Stop 后排空尾包
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterRawInput();
    if (g_rawHwnd) {
        DestroyWindow(g_rawHwnd);
        g_rawHwnd = nullptr;
    }
    timeEndPeriod(1);
}

bool StartRawInputCapture() {
    if (g_rawThreadRun.load(std::memory_order_relaxed)) return g_rawRegistrationOk;
    {
        std::lock_guard<std::mutex> lock(g_rawReadyMutex);
        g_rawReady = false;
        g_rawRegistrationOk = false;
    }
    g_rawThreadRun.store(true, std::memory_order_relaxed);
    g_rawThread = std::thread(RawInputThreadMain);
    std::unique_lock<std::mutex> lock(g_rawReadyMutex);
    const bool signaled = g_rawReadyCv.wait_for(lock, std::chrono::seconds(2), [] {
        return g_rawReady;
    });
    return signaled && g_rawRegistrationOk;
}

void StopRawInputCapture() {
    if (!g_rawThreadRun.exchange(false)) {
        if (g_rawThread.joinable()) g_rawThread.join();
        return;
    }
    if (g_rawHwnd) PostMessageW(g_rawHwnd, WM_NULL, 0, 0);
    if (g_rawThread.joinable()) g_rawThread.join();
}

} // namespace

void InitRecordingClock() {
    LARGE_INTEGER freq{};
    LARGE_INTEGER counter{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    g_qpcFreq.store(freq.QuadPart > 0 ? freq.QuadPart : 1, std::memory_order_relaxed);
    g_recordStartQpc.store(counter.QuadPart, std::memory_order_relaxed);
    g_lastAbsMoveUs.store(0, std::memory_order_relaxed);
    g_lastRelStampUs.store(0, std::memory_order_relaxed);
    g_relReportIntervalUs.store(8000, std::memory_order_relaxed);
    g_lastRelativeActiveUs.store(0, std::memory_order_relaxed);
    g_eventSequence.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_recordMutex);
        g_keysHeld.reset();
    }
}

uint64_t RecordingOffsetUs() {
    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    const int64_t start = g_recordStartQpc.load(std::memory_order_relaxed);
    const int64_t freq = g_qpcFreq.load(std::memory_order_relaxed);
    if (freq <= 0) return 0;
    const int64_t delta = counter.QuadPart - start;
    if (delta <= 0) return 0;
    return static_cast<uint64_t>(
        (static_cast<long double>(delta) * 1000000.0L) / static_cast<long double>(freq));
}

void BeginHighResTimer() {
    if (g_highResTimerDepth.fetch_add(1) == 0) {
        timeBeginPeriod(1);
    }
}

void EndHighResTimer() {
    const int prev = g_highResTimerDepth.fetch_sub(1);
    if (prev == 1) {
        timeEndPeriod(1);
    } else if (prev <= 0) {
        g_highResTimerDepth.store(0, std::memory_order_relaxed);
    }
}

bool IsRelativeMouseCaptureActive() {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (GetCursorInfo(&ci)) {
        if ((ci.flags & CURSOR_SHOWING) == 0) return true;
    }

    RECT clip{};
    if (!GetClipCursor(&clip)) return false;
    const int cw = clip.right - clip.left;
    const int ch = clip.bottom - clip.top;
    if (cw <= 0 || ch <= 0) return false;

    const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vsW <= 0 || vsH <= 0) return false;

    constexpr int kSlack = 16;
    const bool clippedSmaller = (cw < vsW - kSlack) || (ch < vsH - kSlack);
    const bool clipTouchesDesktop = clip.left >= vsX - kSlack && clip.top >= vsY - kSlack
        && clip.right <= vsX + vsW + kSlack && clip.bottom <= vsY + vsH + kSlack;
    return clippedSmaller && clipTouchesDesktop;
}

void SetRecordingCaptureMode(RecordingCaptureMode mode) {
    g_captureMode.store(mode, std::memory_order_relaxed);
}

RecordingCaptureMode GetRecordingCaptureMode() {
    return g_captureMode.load(std::memory_order_relaxed);
}

void SetRecordingWindowTarget(const RecordingWindowTarget& target) {
    std::lock_guard<std::mutex> lock(g_wmTargetMu);
    g_wmTarget = target;
}

RecordingWindowTarget GetRecordingWindowTarget() {
    std::lock_guard<std::mutex> lock(g_wmTargetMu);
    return g_wmTarget;
}

bool MapRecordingPointToClientIfWindowRelative(int& x, int& y) {
    const RecordingWindowTarget t = GetRecordingWindowTarget();
    if (!t.enabled || !t.hwnd || !IsWindow(t.hwnd)) return false;
    RECT client{};
    if (!GetClientRect(t.hwnd, &client)) return false;
    POINT pt{x, y};
    if (!ScreenToClient(t.hwnd, &pt)) return false;
    if (pt.x < 0 || pt.y < 0 || pt.x >= client.right || pt.y >= client.bottom) {
        return false;
    }
    x = pt.x;
    y = pt.y;
    return true;
}

namespace {

/// 窗口相对录制：屏幕坐标 → 目标窗口客户区坐标；窗口外事件返回 false（不录制）。
bool MapRecordingPointToClient(const RecordingWindowTarget& t, int& x, int& y) {
    if (!t.enabled || !t.hwnd || !IsWindow(t.hwnd)) return true;
    RECT client{};
    if (!GetClientRect(t.hwnd, &client)) return false;
    POINT pt{x, y};
    if (!ScreenToClient(t.hwnd, &pt)) return false;
    if (pt.x < 0 || pt.y < 0 || pt.x >= client.right || pt.y >= client.bottom) {
        return false;
    }
    x = pt.x;
    y = pt.y;
    return true;
}

}  // namespace

void SetRecordingIgnoreHotkeys(const RecordingIgnoreChord* items, int count) {
    std::lock_guard<std::mutex> lock(g_recordingIgnoreMu);
    g_recordingIgnoreCount = 0;
    if (!items || count <= 0) return;
    const int n = (count < kMaxRecordingIgnoreHotkeys) ? count : kMaxRecordingIgnoreHotkeys;
    for (int i = 0; i < n; ++i) {
        if (!items[i].vk) continue;
        g_recordingIgnoreList[g_recordingIgnoreCount++] = items[i];
    }
}

void SetRecordingIgnoreHotkey(UINT modifiers, UINT vk, bool enabled) {
    if (!enabled || !vk) {
        SetRecordingIgnoreHotkeys(nullptr, 0);
        return;
    }
    RecordingIgnoreChord one{modifiers, vk};
    SetRecordingIgnoreHotkeys(&one, 1);
}

void SetRecordingDebugSink(RecordingDebugSink sink) {
    std::lock_guard<std::mutex> lock(g_recordingDebugMu);
    g_recordingDebugSink = std::move(sink);
}

void ClearRecordingDebugStats() {
    g_dbgKeyDown.store(0, std::memory_order_relaxed);
    g_dbgKeyUp.store(0, std::memory_order_relaxed);
    g_dbgKeyRepeatSkipped.store(0, std::memory_order_relaxed);
    g_dbgMouseDown.store(0, std::memory_order_relaxed);
    g_dbgMouseUp.store(0, std::memory_order_relaxed);
    g_dbgWheel.store(0, std::memory_order_relaxed);
    g_dbgAbsMove.store(0, std::memory_order_relaxed);
    g_dbgRelMove.store(0, std::memory_order_relaxed);
}

RecordingDebugStats GetRecordingDebugStats() {
    RecordingDebugStats s{};
    s.keyDown = g_dbgKeyDown.load(std::memory_order_relaxed);
    s.keyUp = g_dbgKeyUp.load(std::memory_order_relaxed);
    s.keyRepeatSkipped = g_dbgKeyRepeatSkipped.load(std::memory_order_relaxed);
    s.mouseDown = g_dbgMouseDown.load(std::memory_order_relaxed);
    s.mouseUp = g_dbgMouseUp.load(std::memory_order_relaxed);
    s.wheel = g_dbgWheel.load(std::memory_order_relaxed);
    s.absMove = g_dbgAbsMove.load(std::memory_order_relaxed);
    s.relMove = g_dbgRelMove.load(std::memory_order_relaxed);
    return s;
}

LRESULT CALLBACK KeyboardHookProc(int code, WPARAM wp, LPARAM lp) {
    input_emergency::LlHookGuard llGuard;
    if (code >= 0 && g_recording) {
        auto* ks = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN
            || wp == WM_KEYUP || wp == WM_SYSKEYUP) {
            if (!RecordingScopeAllowsEvent(true, 0, 0)) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            if (ShouldIgnoreRecordedInput(
                    wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN ? WM_KEYDOWN : WM_KEYUP,
                    ks->vkCode)) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            const bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
            const UINT vk = static_cast<UINT>(ks->vkCode) & 0xFFu;
            bool skippedRepeat = false;
            bool accepted = false;
            uint64_t stampUs = 0;
            {
                std::lock_guard<std::mutex> lock(g_recordMutex);
                if (down) {
                    if (g_keysHeld.test(vk)) {
                        skippedRepeat = true;
                    } else {
                        g_keysHeld.set(vk);
                        accepted = true;
                    }
                } else {
                    g_keysHeld.reset(vk);
                    accepted = true;
                }
                if (accepted) {
                    stampUs = RecordingOffsetUs();
                    RecordedEvent ev{};
                    ev.timeOffsetUs = stampUs;
                    ev.sequence = NextEventSequence();
                    ev.msg = down ? WM_KEYDOWN : WM_KEYUP;
                    ev.vkOrButton = ks->vkCode;
                    ev.x = 0; ev.y = 0;
                    ev.source = RecordedEventSource::LowLevelHook;
                    g_recordedEvents.push_back(ev);
                }
            }
            if (skippedRepeat) {
                g_dbgKeyRepeatSkipped.fetch_add(1, std::memory_order_relaxed);
            } else if (accepted) {
                if (down) g_dbgKeyDown.fetch_add(1, std::memory_order_relaxed);
                else g_dbgKeyUp.fetch_add(1, std::memory_order_relaxed);
                EmitRecordingDebug(
                    FormatRecordingStamp(stampUs)
                    + (down ? L"键盘按下 " : L"键盘松开 ")
                    + VkName(static_cast<UINT>(ks->vkCode)));
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK MouseHookProc(int code, WPARAM wp, LPARAM lp) {
    input_emergency::LlHookGuard llGuard;
    if (code >= 0 && g_recording) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        if (!RecordingScopeAllowsEvent(false, ms->pt.x, ms->pt.y)) {
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        const RecordingWindowTarget wmTgt = GetRecordingWindowTarget();
        int px = ms->pt.x;
        int py = ms->pt.y;
        // 窗口相对录制：只录目标窗口内事件，坐标转客户区（跟随窗口回放）。
        if (!MapRecordingPointToClient(wmTgt, px, py)) {
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        const uint64_t nowUs = RecordingOffsetUs();
        if (wp == WM_MOUSEMOVE) {
            MaybeUpdateHoverPatch(px, py, wmTgt.enabled ? wmTgt.hwnd : nullptr);
            if (ShouldCaptureRelativeNow()) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            const uint64_t lastUs = g_lastAbsMoveUs.load(std::memory_order_relaxed);
            if (nowUs - lastUs >= kAbsoluteMoveSampleUs) {
                g_lastAbsMoveUs.store(nowUs, std::memory_order_relaxed);
                RecordedEvent ev{};
                ev.timeOffsetUs = nowUs;
                ev.sequence = NextEventSequence();
                ev.msg = WM_MOUSEMOVE;
                ev.vkOrButton = 0;
                ev.x = px; ev.y = py;
                ev.source = RecordedEventSource::LowLevelHook;
                std::lock_guard<std::mutex> lock(g_recordMutex);
                g_recordedEvents.push_back(ev);
                g_dbgAbsMove.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (wp == WM_LBUTTONDOWN || wp == WM_LBUTTONUP) {
            if (ShouldIgnoreRecordedInput(static_cast<UINT>(wp), VK_LBUTTON)) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            RecordedEvent ev{};
            ev.timeOffsetUs = nowUs;
            ev.sequence = NextEventSequence();
            ev.msg = static_cast<UINT>(wp);
            ev.vkOrButton = VK_LBUTTON;
            ev.x = px; ev.y = py;
            ev.source = RecordedEventSource::LowLevelHook;
            {
                std::lock_guard<std::mutex> lock(g_recordMutex);
                g_recordedEvents.push_back(ev);
            }
            NoteRecordedMouseButton(ev);
            if (IsButtonDownMsg(ev.msg)) {
                EnqueueClickCapture(ev.sequence, ev.x, ev.y, wmTgt.enabled ? wmTgt.hwnd : nullptr);
            }
        } else if (wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP) {
            if (ShouldIgnoreRecordedInput(static_cast<UINT>(wp), VK_RBUTTON)) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            RecordedEvent ev{};
            ev.timeOffsetUs = nowUs;
            ev.sequence = NextEventSequence();
            ev.msg = static_cast<UINT>(wp);
            ev.vkOrButton = VK_RBUTTON;
            ev.x = px; ev.y = py;
            ev.source = RecordedEventSource::LowLevelHook;
            {
                std::lock_guard<std::mutex> lock(g_recordMutex);
                g_recordedEvents.push_back(ev);
            }
            NoteRecordedMouseButton(ev);
            if (IsButtonDownMsg(ev.msg)) {
                EnqueueClickCapture(ev.sequence, ev.x, ev.y, wmTgt.enabled ? wmTgt.hwnd : nullptr);
            }
        } else if (wp == WM_MBUTTONDOWN || wp == WM_MBUTTONUP) {
            if (ShouldIgnoreRecordedInput(static_cast<UINT>(wp), VK_MBUTTON)) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            RecordedEvent ev{};
            ev.timeOffsetUs = nowUs;
            ev.sequence = NextEventSequence();
            ev.msg = static_cast<UINT>(wp);
            ev.vkOrButton = VK_MBUTTON;
            ev.x = px; ev.y = py;
            ev.source = RecordedEventSource::LowLevelHook;
            {
                std::lock_guard<std::mutex> lock(g_recordMutex);
                g_recordedEvents.push_back(ev);
            }
            NoteRecordedMouseButton(ev);
            if (IsButtonDownMsg(ev.msg)) {
                EnqueueClickCapture(ev.sequence, ev.x, ev.y, wmTgt.enabled ? wmTgt.hwnd : nullptr);
            }
        } else if (wp == WM_XBUTTONDOWN || wp == WM_XBUTTONUP) {
            UINT btn = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            if (ShouldIgnoreRecordedInput(static_cast<UINT>(wp), btn)) {
                return CallNextHookEx(nullptr, code, wp, lp);
            }
            RecordedEvent ev{};
            ev.timeOffsetUs = nowUs;
            ev.sequence = NextEventSequence();
            ev.msg = static_cast<UINT>(wp);
            ev.vkOrButton = btn;
            ev.x = px; ev.y = py;
            ev.source = RecordedEventSource::LowLevelHook;
            {
                std::lock_guard<std::mutex> lock(g_recordMutex);
                g_recordedEvents.push_back(ev);
            }
            NoteRecordedMouseButton(ev);
            if (IsButtonDownMsg(ev.msg)) {
                EnqueueClickCapture(ev.sequence, ev.x, ev.y, wmTgt.enabled ? wmTgt.hwnd : nullptr);
            }
        } else if (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL) {
            RecordedEvent ev{};
            ev.timeOffsetUs = nowUs;
            ev.sequence = NextEventSequence();
            ev.msg = static_cast<UINT>(wp);
            ev.vkOrButton = 0;
            ev.x = px; ev.y = py;
            ev.wheelDelta = GET_WHEEL_DELTA_WPARAM(ms->mouseData);
            ev.source = RecordedEventSource::LowLevelHook;
            {
                std::lock_guard<std::mutex> lock(g_recordMutex);
                g_recordedEvents.push_back(ev);
            }
            g_dbgWheel.fetch_add(1, std::memory_order_relaxed);
            EmitRecordingDebug(
                FormatRecordingStamp(ev.timeOffsetUs)
                + (wp == WM_MOUSEWHEEL ? L"滚轮 " : L"水平滚轮 ")
                + std::to_wstring(ev.wheelDelta));
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void SetRecordingCaptureScope(int scope) {
    g_captureScope.store(scope <= 0 ? 0 : 1, std::memory_order_relaxed);
}

int GetRecordingCaptureScope() {
    return g_captureScope.load(std::memory_order_relaxed);
}

void BeginRecordingScopeSession(HWND preferredForeground, DWORD ownProcessId) {
    std::lock_guard<std::mutex> lock(g_scopeMu);
    g_scopeOwnPid = ownProcessId;
    g_scopeRoot = nullptr;
    g_scopeArmOnExternal = false;
    if (g_captureScope.load(std::memory_order_relaxed) != 0) {
        return; // Global
    }
    HWND fg = preferredForeground && IsWindow(preferredForeground)
        ? preferredForeground : GetForegroundWindow();
    HWND root = RootHwndOf(fg);
    DWORD pid = 0;
    if (root && PidOfHwnd(root, &pid) && ownProcessId != 0 && pid == ownProcessId) {
        // 从本软件 UI 点开始录制：等第一个外部窗口输入再锁定
        g_scopeArmOnExternal = true;
        return;
    }
    g_scopeRoot = root;
}

void EndRecordingScopeSession() {
    std::lock_guard<std::mutex> lock(g_scopeMu);
    g_scopeRoot = nullptr;
    g_scopeArmOnExternal = false;
    g_scopeOwnPid = 0;
}

bool RecordingScopeAllowsEvent(bool isKeyboard, int screenX, int screenY) {
    const int scope = g_captureScope.load(std::memory_order_relaxed);
    if (scope != 0) return true;

    HWND candidate = nullptr;
    if (isKeyboard) {
        candidate = RootHwndOf(GetForegroundWindow());
    } else {
        POINT pt{screenX, screenY};
        candidate = RootHwndOf(WindowFromPoint(pt));
    }

    DWORD candPid = 0;
    const bool own = PidOfHwnd(candidate, &candPid) && g_scopeOwnPid != 0 && candPid == g_scopeOwnPid;

    std::lock_guard<std::mutex> lock(g_scopeMu);
    const auto decision = EvaluateRecordingScopeFilter(
        scope, g_scopeRoot, g_scopeArmOnExternal, candidate, own);
    if (decision.newRoot) {
        g_scopeRoot = decision.newRoot;
        g_scopeArmOnExternal = false;
    }
    return decision.accept;
}

bool InstallRecordingHooks() {
    if (g_keyboardHook || g_mouseHook) return g_keyboardHook && g_mouseHook;
    HINSTANCE inst = GetModuleHandleW(nullptr);
    g_keyboardHook = SetWindowsHookExW(
        WH_KEYBOARD_LL, KeyboardHookProc, inst, 0);
    g_mouseHook = SetWindowsHookExW(
        WH_MOUSE_LL, MouseHookProc, inst, 0);
    const bool rawOk = StartRawInputCapture();
    const bool hooksOk = g_keyboardHook && g_mouseHook;
    return hooksOk && (GetRecordingCaptureMode() == RecordingCaptureMode::Absolute || rawOk);
}

void UninstallRecordingHooks() {
    StopRawInputCapture();
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
}

void SetRecordingClickCaptureConfig(bool enabled, int halfSize) {
    g_captureEnabled.store(enabled, std::memory_order_relaxed);
    g_captureHalfSize.store(ClampClickCaptureHalfSize(halfSize), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_captureMutex);
        DrainCaptureQueueLocked();
    }
    {
        std::lock_guard<std::mutex> lock(g_hoverMu);
        ClearHoverPatchLocked();
    }
    g_hoverLastUpdateTick.store(0, std::memory_order_relaxed);
    const uint32_t salt = g_captureSessionSalt.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t sessionId = (GetTickCount64() << 16) ^ static_cast<uint64_t>(salt);
    g_captureSessionId.store(sessionId, std::memory_order_relaxed);
    g_captureDropped.store(0, std::memory_order_relaxed);
    g_captureAccepting.store(enabled, std::memory_order_relaxed);
    if (enabled) {
        EnsureFindImagesDir();
        EnsureCaptureWorkerStarted();
    }
}

void SetRecordingClickCaptureSkipRelative(bool skip) {
    g_captureSkipRelative.store(skip, std::memory_order_relaxed);
}

void FlushClickCaptures(DWORD timeoutMs) {
    g_captureAccepting.store(false, std::memory_order_relaxed);
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        bool idle = false;
        {
            std::lock_guard<std::mutex> lock(g_captureMutex);
            idle = g_captureQueue.empty()
                && g_captureInFlight.load(std::memory_order_relaxed) == 0;
        }
        if (idle) break;
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::unique_lock<std::mutex> lock(g_captureMutex);
        g_captureCv.wait_for(lock, std::chrono::milliseconds(50), []() {
            return g_captureQueue.empty()
                && g_captureInFlight.load(std::memory_order_relaxed) == 0;
        });
    }
}

void DiscardClickCaptures() {
    g_captureAccepting.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_captureMutex);
        DrainCaptureQueueLocked();
    }
    std::lock_guard<std::mutex> lock(g_hoverMu);
    ClearHoverPatchLocked();
    g_hoverLastUpdateTick.store(0, std::memory_order_relaxed);
}

uint64_t GetRecordingClickCaptureSessionId() {
    return g_captureSessionId.load(std::memory_order_relaxed);
}
