#include "macro_execute_tools.h"

#include "agent_ai_actions.h"
#include "agent_web.h"
#include "ai_logic_convert.h"
#include "image_var_util.h"
#include "process_utils.h"
#include "script_action_builder.h"
#include "utils.h"

#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <shlobj.h>
#include <imm.h>

namespace {

using json = nlohmann::json;

constexpr const wchar_t* kTaskCompleteMarker = L"[TASK_COMPLETE]";
constexpr const wchar_t* kExecutedObserveMarker = L"[EXECUTED][OBSERVE]";
constexpr const wchar_t* kExecutedSkipObserveMarker = L"[EXECUTED][SKIP_OBSERVE]";
constexpr size_t kAiMemoMaxLines = 16;
constexpr size_t kAiMemoMaxLineChars = 120;

/// 固定章节（对齐 Claude Code Session Memory 的槽位思路）
constexpr const wchar_t* kMemoSectionOrder[] = {
    L"goal", L"facts", L"todos", L"windows", L"recipe", L"notes"
};
constexpr size_t kMemoSectionCount =
    sizeof(kMemoSectionOrder) / sizeof(kMemoSectionOrder[0]);

bool IsMemoSectionName(const std::wstring& s) {
    for (size_t i = 0; i < kMemoSectionCount; ++i) {
        if (s == kMemoSectionOrder[i]) return true;
    }
    return false;
}

bool MemoSectionIsSingleton(const std::wstring& section) {
    return section == L"goal" || section == L"windows" || section == L"recipe";
}

std::wstring MemoSectionTag(const std::wstring& section) {
    return L"[" + section + L"] ";
}

std::wstring NormalizeMemoLine(std::wstring line);

struct AiActionSessionState {
    unsigned long long runId = 0;
    std::wstring lastOpenWebpageUrl;
    /// 已往保存框输入、等待 Enter/保存 提交的完整文件路径（目录路径不记）
    std::wstring pendingSavePath;
    /// 最近一次 runProgram/openFile 成功的时刻（刚启动的窗口会自动前台，无需切窗）
    unsigned long long lastLaunchTick = 0;
    bool launchSwitchWarned = false;
    /// 已执行动作序号；用于判断「显示桌面」是不是在原地连按
    int actionSeq = 0;
    /// 上次 Win+D / Win+Down 等藏窗动作的序号（-100 = 没用过）
    int lastHideWindowsSeq = -100;
    /// resolveSystemPath 最近取到的真实目录 / 完整路径（保存链路的唯一可信路径）
    std::wstring lastResolvedDir;
    std::wstring lastResolvedFullPath;
    /// 已就「Escape 会取消保存」劝阻过的动作序号（-100 = 没劝过）
    int saveEscapeWarnedSeq = -100;
    /// 顶层 Agent 是否启用「先写 goal/todos 再动手」门闩（自检默认关）
    bool planGateEnabled = false;
    /// 本轮已成功写入 goal/todos
    bool planUnlocked = false;
    /// 最近一次工具调用签名（名称+关键参数摘要）；用于连续重复动作劝阻
    std::wstring lastToolSig;
    int lastToolSigSeq = -100;
    /// runActionRecipe：steps 模板签名；试跑组数；模板是否已核对通过可全量复用
    std::wstring recipeVerifiedSig;
    int recipePreviewGroups = 0;
    bool recipeTemplateTrusted = false;
    /// 试跑时实际执行的前几行指纹；确认时若 rows 前缀匹配则跳过，否则视为剩余行全跑
    std::wstring recipePreviewRowsFp;
    /// 最近观察的动态覆盖；过高则禁在页面内容上识图
    double lastBusyCoverage = -1.0;
    bool lastSettleStillChanging = false;
    int lookaheadStarts = 0;
    /// 另存为里连续 scrollWheel 次数（软提示，不硬拦）
    int saveAsScrollStreak = 0;
    /// locate 失败后拦乱快捷键（白名单外需 confirmBlindKey）
    bool locateFailKeyBlock = false;
    /// locate 失败重试计数：连败超限后只允许 scroll/activate/completeTask
    int locateRetryAfterFail = 0;
    /// 最近一次定位失败的目标描述（用于「同一目标」连败拦截）
    std::wstring lastLocateFailTarget;
    /// 连续纯 Tab（无修饰）次数；配方失败后盲 Tab 会把表格列对齐打崩
    int bareTabStreak = 0;
    /// Midscene 式：UIA 提交钮灰后，禁止 Enter 空转直至输入/界面变化
    bool submitUiBlocked = false;
};

AiActionSessionState& AiSession() {
    thread_local AiActionSessionState s;
    return s;
}

/// F2 重命名后资源管理器已选中「主文件名」（不含扩展名）；下一拍 quickInput 勿 Ctrl+A
thread_local bool g_keepInputSelectionAfterF2 = false;

/// 真实系统目录（勿让模型猜 C:\Users\Administrator 之类不存在的路径）
std::wstring KnownFolderPathByName(const std::wstring& nameRaw) {
    std::wstring name = nameRaw;
    for (auto& c : name) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    int csidl = -1;
    std::wstring suffix;
    if (name == L"desktop" || name == L"桌面") csidl = CSIDL_DESKTOPDIRECTORY;
    else if (name == L"documents" || name == L"文档") csidl = CSIDL_PERSONAL;
    else if (name == L"pictures" || name == L"图片") csidl = CSIDL_MYPICTURES;
    else if (name == L"profile" || name == L"user" || name == L"home") csidl = CSIDL_PROFILE;
    else if (name == L"downloads" || name == L"下载") {
        csidl = CSIDL_PROFILE;
        suffix = L"\\Downloads";
    } else if (name == L"appdata") csidl = CSIDL_APPDATA;
    if (csidl < 0) return {};

    wchar_t buf[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf)))
        return {};
    std::wstring path = buf;
    if (path.empty()) return {};
    if (!suffix.empty()) path += suffix;
    return path;
}

bool DirectoryExists(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD attr = GetFileAttributesW(dir.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool LooksLikeAbsolutePath(const std::wstring& s) {
    return s.size() >= 3 && s[1] == L':' && (s[2] == L'\\' || s[2] == L'/')
        && ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z'));
}

bool ContainsAny(const std::wstring& text, std::initializer_list<const wchar_t*> keys) {
    for (const wchar_t* k : keys) {
        if (text.find(k) != std::wstring::npos) return true;
    }
    return false;
}

/// 保存框里已写入完整文件路径时的「绕远路」目标（点了会丢掉路径）
bool IsSaveDialogDetourTarget(const std::wstring& targetRaw) {
    // 「浏览记录」等词里的「浏览」不是浏览按钮
    std::wstring target = targetRaw;
    for (const wchar_t* w : { L"浏览记录", L"浏览器", L"浏览历史", L"浏览数据" }) {
        const size_t n = wcslen(w);
        for (size_t p = target.find(w); p != std::wstring::npos; p = target.find(w, p))
            target.erase(p, n);
    }
    // 「浏览 / 更多选项」是从迷你保存框进经典对话框的正路，不拦
    return ContainsAny(target, {
        L"这台电脑", L"此电脑", L"我的电脑", L"其他位置", L"添加位置",
        L"导航栏", L"最近", L"OneDrive", L"新建文件夹", L"This PC" });
}

/// 目标描述看起来是「想切到某个窗口」（任务栏图标/某程序窗口）
bool LooksLikeWindowSwitchTarget(const std::wstring& target) {
    return ContainsAny(target, {
        L"任务栏", L"窗口", L"缩略图", L"预览", L"taskbar", L"window" });
}

/// 提交保存的目标（点了就算路径已用掉）
bool IsSaveSubmitTarget(const std::wstring& target) {
    return ContainsAny(target, { L"保存", L"确定", L"是(", L"Save", L"OK" });
}

/// 从宿主返回的 "kind=…;title=…" 里取某个字段值
std::wstring DialogProbeField(const std::wstring& probe, const std::wstring& key) {
    const std::wstring needle = key + L"=";
    size_t pos = probe.find(needle);
    while (pos != std::wstring::npos && pos != 0 && probe[pos - 1] != L';')
        pos = probe.find(needle, pos + 1);
    if (pos == std::wstring::npos) return {};
    const size_t from = pos + needle.size();
    const size_t end = probe.find(L';', from);
    return probe.substr(from, end == std::wstring::npos ? std::wstring::npos : end - from);
}

std::wstring ProbeForegroundDialogKind(AiActionHostHooks* hooks, std::wstring* full) {
    if (!hooks || !hooks->onProbeForegroundDialog) return {};
    const std::wstring probe = hooks->onProbeForegroundDialog();
    if (full) *full = probe;
    if (probe.empty()) return {};
    return DialogProbeField(probe, L"kind");
}

/// 绝对路径的父目录是否真实存在（保存对话框里输错路径会静默存到别处/报错）
bool AbsolutePathParentExists(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    if (slash <= 2) return true; // 盘根 C:\name
    return DirectoryExists(path.substr(0, slash));
}

std::wstring AiTaskMemoPath() {
    const unsigned long long id = AiSession().runId;
    if (id == 0)
        return ImageVarRuntimeDir() + L"\\ai_task_memo.txt";
    return ImageVarRuntimeDir() + L"\\" + std::to_wstring(id) + L"_aiMemo.txt";
}

}  // namespace（数据缓存 / IME 控制导出到全局命名空间，声明见 macro_execute_tools.h）

// ── 任务数据缓存（md 分块，长任务「识图一次→缓存→反复读取」） ──────────
// 文件格式：## 数据块名\n 内容行...。块名即 saveTaskData 的 name。
constexpr const wchar_t* kTaskDataBlockTag = L"## ";
constexpr size_t kTaskDataBlockNameMax = 48;
constexpr size_t kTaskDataMaxBlocks = 12;
constexpr size_t kTaskDataMaxBlockChars = 2000;

std::wstring AiTaskDataPath() {
    const unsigned long long id = AiSession().runId;
    if (id == 0)
        return ImageVarRuntimeDir() + L"\\ai_task_data.md";
    return ImageVarRuntimeDir() + L"\\" + std::to_wstring(id) + L"_aiData.md";
}

/// 解析为 {name, content} 列表（保持写入顺序）
std::vector<std::pair<std::wstring, std::wstring>> LoadAiTaskDataBlocks() {
    std::vector<std::pair<std::wstring, std::wstring>> blocks;
    std::ifstream in(ToUtf8(AiTaskDataPath()), std::ios::binary);
    if (!in) return blocks;
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3
        && static_cast<unsigned char>(raw[0]) == 0xEF
        && static_cast<unsigned char>(raw[1]) == 0xBB
        && static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }
    std::wstring curName;
    std::wstring curBody;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t nl = raw.find('\n', start);
        if (nl == std::string::npos) nl = raw.size();
        std::string piece = raw.substr(start, nl - start);
        if (!piece.empty() && piece.back() == '\r') piece.pop_back();
        std::wstring line = FromUtf8(piece);
        if (line.rfind(kTaskDataBlockTag, 0) == 0) {
            if (!curName.empty())
                blocks.emplace_back(curName, curBody);
            curName = Trim(line.substr(2));
            curBody.clear();
        } else {
            if (!curBody.empty()) curBody += L"\n";
            curBody += line;
        }
        if (nl >= raw.size()) break;
        start = nl + 1;
    }
    if (!curName.empty())
        blocks.emplace_back(curName, curBody);
    return blocks;
}

bool SaveAiTaskDataBlocks(const std::vector<std::pair<std::wstring, std::wstring>>& blocks) {
    std::ofstream out(ToUtf8(AiTaskDataPath()), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    out.write(reinterpret_cast<const char*>(bom), 3);
    for (const auto& [name, body] : blocks) {
        const std::string tag = ToUtf8(std::wstring(kTaskDataBlockTag) + name + L"\n");
        out.write(tag.data(), static_cast<std::streamsize>(tag.size()));
        if (!body.empty()) {
            const std::string u8 = ToUtf8(body);
            out.write(u8.data(), static_cast<std::streamsize>(u8.size()));
            out.put('\n');
        }
    }
    return static_cast<bool>(out);
}

/// 写入/追加一个数据块；replace=false 时追加到末尾。返回新块内容或空串失败
std::wstring UpsertAiTaskDataBlock(const std::wstring& nameIn, const std::wstring& content,
    bool replace) {
    std::wstring name = Trim(nameIn);
    if (name.size() > kTaskDataBlockNameMax)
        name = name.substr(0, kTaskDataBlockNameMax);
    if (name.empty()) return {};
    if (!EnsureParentDir(AiTaskDataPath())) return {};

    std::wstring body = content;
    // 块内禁止再嵌套 "## " 行（会破坏块边界），转为缩进占位
    {
        size_t p = 0;
        std::wstring fixed;
        while (p < body.size()) {
            size_t nl = body.find(L'\n', p);
            if (nl == std::wstring::npos) nl = body.size();
            std::wstring ln = body.substr(p, nl - p);
            if (ln.rfind(L"## ", 0) == 0) ln = L"- " + ln;
            if (!fixed.empty()) fixed += L"\n";
            fixed += ln;
            p = nl + 1;
            if (p > body.size()) break;
        }
        body = fixed;
    }
    if (body.size() > kTaskDataMaxBlockChars)
        body = body.substr(0, kTaskDataMaxBlockChars);

    auto blocks = LoadAiTaskDataBlocks();
    bool found = false;
    for (auto& [n, b] : blocks) {
        if (_wcsicmp(n.c_str(), name.c_str()) == 0) {
            if (replace) {
                b = body;
            } else {
                if (!b.empty()) b += L"\n";
                b += body;
                if (b.size() > kTaskDataMaxBlockChars)
                    b = b.substr(0, kTaskDataMaxBlockChars);
            }
            found = true;
            break;
        }
    }
    if (!found) {
        if (blocks.size() >= kTaskDataMaxBlocks)
            blocks.erase(blocks.begin());
        blocks.emplace_back(name, body);
    }
    if (!SaveAiTaskDataBlocks(blocks)) return {};
    std::wstring cur = body;
    for (const auto& [n, b] : blocks) {
        if (_wcsicmp(n.c_str(), name.c_str()) == 0) { cur = b; break; }
    }
    return cur;
}

std::wstring ReadAiTaskDataBlock(const std::wstring& name) {
    const std::wstring target = Trim(name);
    for (const auto& [n, b] : LoadAiTaskDataBlocks()) {
        if (_wcsicmp(n.c_str(), target.c_str()) == 0) return b;
    }
    return {};
}

/// 整份数据缓存文本（含块名），供 readTaskData 无参读取；超过上限截断
std::wstring ReadAiTaskDataAllText() {
    const auto blocks = LoadAiTaskDataBlocks();
    std::wstring s;
    for (const auto& [n, b] : blocks) {
        if (!s.empty()) s += L"\n";
        s += std::wstring(kTaskDataBlockTag) + n + L"\n" + b;
        if (s.size() > 6000) {
            s = s.substr(0, 6000) + L"\n…(缓存过长已截断)";
            break;
        }
    }
    return s;
}

/// 注入用摘要：每块只取块名 + 前两行，控制每轮注入 token
std::wstring ReadAiTaskDataSummary() {
    const auto blocks = LoadAiTaskDataBlocks();
    std::wstring s;
    for (const auto& [n, b] : blocks) {
        if (!s.empty()) s += L"\n";
        std::wstring head;
        size_t pos = 0;
        int lines = 0;
        while (pos < b.size() && lines < 2) {
            size_t nl = b.find(L'\n', pos);
            if (nl == std::wstring::npos) nl = b.size();
            if (!head.empty()) head += L" | ";
            std::wstring piece = b.substr(pos, nl - pos);
            if (piece.size() > 40) piece = piece.substr(0, 40) + L"…";
            head += piece;
            pos = nl + 1;
            ++lines;
        }
        s += L"· " + n + (head.empty() ? L"" : L": " + head);
        if (s.size() > 900) break;
    }
    return s;
}

// ── 输入法（IME）控制 ──────────────────────────────────────────────────
/// 是否前台线程键盘布局是中文（PRIMARYLANGID == LANG_CHINESE）
bool ForegroundImeIsChineseLayout() {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return false;
    const DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    if (!tid) return false;
    const HKL hkl = GetKeyboardLayout(tid);
    const LANGID lang = static_cast<LANGID>(reinterpret_cast<ULONG_PTR>(hkl) & 0xFFFF);
    return PRIMARYLANGID(lang) == LANG_CHINESE;
}

/// 把前台窗口的 IME 切到中文(拼音/原生)/英文(字母数字) 模式。
/// 走 IME 默认窗口 WM_IME_CONTROL + 焦点窗口 Imm 上下文双保险；
/// 失败不报错（尽力而为），返回 false 表示「没能确认切换成功」。
bool SetForegroundImeMode(bool chineseMode) {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return false;
    if (!ForegroundImeIsChineseLayout()) return false;
    const DWORD flag = chineseMode ? IME_CMODE_NATIVE : IME_CMODE_ALPHANUMERIC;

    bool sent = false;
    HWND imeWnd = ImmGetDefaultIMEWnd(fg);
    if (imeWnd && IsWindow(imeWnd)) {
        const LRESULT r = SendMessageTimeoutW(imeWnd, WM_IME_CONTROL,
            0x4 /*IMC_SETCONVERSIONMODE*/, flag, SMTO_ABORTIFHUNG, 200, nullptr);
        sent = (r != 0);
    }
    HWND focus = fg;
    const DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    GUITHREADINFO gi{};
    gi.cbSize = sizeof(gi);
    if (tid && GetGUIThreadInfo(tid, &gi) && gi.hwndFocus) focus = gi.hwndFocus;
    HIMC imc = ImmGetContext(focus);
    if (imc) {
        ImmSetConversionStatus(imc, flag,
            chineseMode ? IME_SMODE_NONE : IME_SMODE_NONE);
        ImmReleaseContext(focus, imc);
        sent = true;
    }
    return sent;
}

/// 取消前台输入法的挂起组字（CPS_CANCEL，不向应用发 Escape，安全用于表格单元格）。
/// 现代 TSF 输入法（Win10/11 微软拼音等）里 ImmSetConversionStatus 经常不生效，
/// 组字串会一直挂着，把后续 KEYEVENTF_UNICODE 的字符拼进组字串（如「1」→「h1」）。
/// 因此检测到组字时，在 CPS_CANCEL 之后补一次物理 Escape 兜底清掉——
/// 组字态下 Escape 只清组字串、不伤已提交内容；没有组字则完全不动应用。
void CancelPendingImeComposition() {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return;
    const DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    if (!tid) return;
    HWND focus = fg;
    GUITHREADINFO gi{};
    gi.cbSize = sizeof(gi);
    if (GetGUIThreadInfo(tid, &gi) && gi.hwndFocus && IsWindow(gi.hwndFocus))
        focus = gi.hwndFocus;
    HIMC imc = ImmGetContext(focus);
    bool hadComposition = false;
    if (imc) {
        // 先读组字串再 CPS_CANCEL；TSF 下偶发读不到，仍一律通知取消一次
        if (ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0) > 0)
            hadComposition = true;
        ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
        ImmReleaseContext(focus, imc);
    }
    // 仅确认有组字时补 Escape：无组字时 Escape 会关掉另存为/确认框
    if (hadComposition && IsWindow(fg)) {
        INPUT inp{};
        inp.type = INPUT_KEYBOARD;
        inp.ki.wVk = VK_ESCAPE;
        SendInput(1, &inp, sizeof(inp));
        inp.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &inp, sizeof(inp));
    }
}

/// ASCII 单键（字母/数字/标点）走 keyClick 时先确保英文 IME：
/// 中文模式下按 y/数字会被拼音输入法吃掉（如覆盖确认框按 Y 无反应）。
void ForceEnglishImeBeforeAsciiKey() {
    // 已是英文布局时跳过：连续 keyClick 热路径上每次 Imm/IME 往返会拖慢整机按键手感
    if (!ForegroundImeIsChineseLayout()) return;
    CancelPendingImeComposition();
    SetForegroundImeMode(false);
}

/// quickInput 前准备：清挂起组字并强制英文 IME。
/// 即便文本是纯中文（走 Unicode），中文布局下挂起的拼音组字串仍会把后续字符拼进去
/// （如组字 "h" + Unicode「序号」→「h序号」/「H序号」），故一律切英文。
void PrepareImeForTextInput(const std::wstring& text) {
    (void)text;
    ForceEnglishImeBeforeAsciiKey();
}

/// 把输入法状态文字绘制到位图左上角（观察帧编码前调用）。
/// Windows 的 IME 候选框/组字框是独立进程 DirectComposition 悬浮窗口，
/// BitBlt/CAPTUREBLT 物理上截不到；画一行状态文字是让 Agent 从截图
/// 「看到输入法」的唯一可靠办法。
void DrawImeStatusOverlay(HBITMAP hbm, const std::wstring& text) {
    if (!hbm || text.empty()) return;
    BITMAP bm{};
    if (!GetObjectW(hbm, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return;
    HDC memDc = CreateCompatibleDC(nullptr);
    if (!memDc) return;
    HGDIOBJ old = SelectObject(memDc, hbm);
    // 字体按位图高度比例，缩放到观察帧（768 长边）后仍可读
    const int fontH = std::max(16, static_cast<int>(bm.bmHeight / 22));
    HFONT font = CreateFontW(-fontH, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Microsoft YaHei");
    if (font) {
        HGDIOBJ oldFont = SelectObject(memDc, font);
        SetBkMode(memDc, TRANSPARENT);
        SetTextColor(memDc, RGB(255, 80, 80));
        RECT rc{ 6, 4, bm.bmWidth - 6, bm.bmHeight - 4 };
        DrawTextW(memDc, text.c_str(), -1, &rc,
            DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(memDc, oldFont);
        DeleteObject(font);
    }
    SelectObject(memDc, old);
    DeleteDC(memDc);
}

/// 查询前台输入法状态文本，供 Agent 每轮注入观测（截图读不出输入法状态）。
/// 返回形如：
///   [输入法] 英文
///   [输入法] 中文，正在组字: "nihao"（按键正被输入法拦截，先 Escape 取消或切英文）
std::wstring QueryForegroundImeStatusText() {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return L"";
    const DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    if (!tid) return L"";
    const HKL hkl = GetKeyboardLayout(tid);
    const LANGID lang = static_cast<LANGID>(reinterpret_cast<ULONG_PTR>(hkl) & 0xFFFF);
    const bool chinese = PRIMARYLANGID(lang) == LANG_CHINESE;
    std::wstring out = chinese ? L"[输入法] 中文" : L"[输入法] 英文";
    if (!chinese) return out;
    HWND focus = fg;
    GUITHREADINFO gi{};
    gi.cbSize = sizeof(gi);
    if (GetGUIThreadInfo(tid, &gi) && gi.hwndFocus && IsWindow(gi.hwndFocus))
        focus = gi.hwndFocus;
    HIMC imc = ImmGetContext(focus);
    if (imc) {
        const LONG n = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
        if (n > 0) {
            std::wstring comp(static_cast<size_t>(n / sizeof(wchar_t)) + 1, L'\0');
            ImmGetCompositionStringW(imc, GCS_COMPSTR, comp.data(), n);
            comp.resize(static_cast<size_t>(n) / sizeof(wchar_t));
            // 组字串可能含未上屏的拼音/汉字；截断过长避免刷屏
            if (comp.size() > 24) comp = comp.substr(0, 24) + L"…";
            out += L"，正在组字: \"" + comp
                + L"\"（按键正被输入法拦截，先 Escape 取消或切英文再按）";
        }
        ImmReleaseContext(focus, imc);
    }
    return out;
}

/// 前台窗口是否为表格软件（Excel/WPS 表格/OnlyOffice 等）。
bool ForegroundIsSpreadsheetApp() {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    if (!ok) return false;
    // 取 exe 文件名并转大写
    const wchar_t* slash = wcsrchr(path, L'\\');
    std::wstring exe = slash ? (slash + 1) : path;
    for (auto& c : exe) {
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    return exe == L"EXCEL.EXE" || exe == L"ET.EXE" || exe == L"ETSD.EXE"
        || exe == L"WPS.EXE" || exe == L"SOFFICE.BIN" || exe == L"ONLYOFFICEDESKTOPEDITORS.EXE"
        || exe == L"GNUMERIC.EXE";
}

namespace {
std::wstring NormalizeMemoLine(std::wstring line) {
    while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n'
        || line.back() == L' ' || line.back() == L'\t')) {
        line.pop_back();
    }
    size_t i = 0;
    while (i < line.size() && (line[i] == L' ' || line[i] == L'\t')) ++i;
    if (i > 0) line.erase(0, i);
    if (line.size() > kAiMemoMaxLineChars)
        line = line.substr(0, kAiMemoMaxLineChars);
    return line;
}

/// 解析 "[goal] xxx" / "goal: xxx"；失败则 section=notes、body=原行
bool ParseMemoSectionLine(const std::wstring& raw, std::wstring& section, std::wstring& body) {
    section = L"notes";
    body = raw;
    if (raw.size() >= 3 && raw[0] == L'[') {
        const size_t rb = raw.find(L']');
        if (rb != std::wstring::npos && rb > 1) {
            std::wstring cand = raw.substr(1, rb - 1);
            for (auto& c : cand) c = static_cast<wchar_t>(towlower(c));
            if (IsMemoSectionName(cand)) {
                section = cand;
                body = NormalizeMemoLine(raw.substr(rb + 1));
                return true;
            }
        }
    }
    const size_t colon = raw.find(L':');
    if (colon != std::wstring::npos && colon > 0 && colon < 12) {
        std::wstring cand = raw.substr(0, colon);
        for (auto& c : cand) c = static_cast<wchar_t>(towlower(c));
        if (IsMemoSectionName(cand)) {
            section = cand;
            body = NormalizeMemoLine(raw.substr(colon + 1));
            return true;
        }
    }
    return false;
}

std::pair<std::wstring, std::wstring> SplitMemoStoredLine(const std::wstring& line) {
    std::wstring section, body;
    if (ParseMemoSectionLine(line, section, body) && !body.empty())
        return { section, body };
    // 兼容旧备忘（无章节前缀）
    return { L"notes", NormalizeMemoLine(line) };
}

std::vector<std::wstring> LoadMemoLines() {
    std::vector<std::wstring> lines;
    const std::wstring path = AiTaskMemoPath();
    std::ifstream in(ToUtf8(path), std::ios::binary);
    if (!in) return lines;
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (raw.size() >= 3
        && static_cast<unsigned char>(raw[0]) == 0xEF
        && static_cast<unsigned char>(raw[1]) == 0xBB
        && static_cast<unsigned char>(raw[2]) == 0xBF) {
        raw.erase(0, 3);
    }
    size_t start = 0;
    while (start <= raw.size()) {
        size_t nl = raw.find('\n', start);
        if (nl == std::string::npos) nl = raw.size();
        std::string piece = raw.substr(start, nl - start);
        if (!piece.empty() && piece.back() == '\r') piece.pop_back();
        std::wstring line = NormalizeMemoLine(FromUtf8(piece));
        if (!line.empty()) lines.push_back(line);
        if (nl >= raw.size()) break;
        start = nl + 1;
    }
    return lines;
}

bool SaveMemoLines(const std::vector<std::wstring>& lines) {
    const std::wstring path = AiTaskMemoPath();
    EnsureParentDir(path);
    std::ofstream out(ToUtf8(path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    // UTF-8 BOM，方便记事本打开；读写统一 UTF-8，避免中文章节损坏
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    out.write(reinterpret_cast<const char*>(bom), 3);
    for (const auto& line : lines) {
        if (line.empty()) continue;
        const std::string u8 = ToUtf8(line);
        out.write(u8.data(), static_cast<std::streamsize>(u8.size()));
        out.put('\n');
    }
    return static_cast<bool>(out);
}

/// BuildScriptActionsJsonArray 可能在数组后追加「[提示]…」；rfind(']') 会误吃提示里的 ]
std::wstring ExtractLeadingJsonArray(const std::wstring& text) {
    const size_t lb = text.find(L'[');
    if (lb == std::wstring::npos) return L"";
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (size_t i = lb; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (inStr) {
            if (esc) esc = false;
            else if (c == L'\\') esc = true;
            else if (c == L'"') inStr = false;
            continue;
        }
        if (c == L'"') {
            inStr = true;
            continue;
        }
        if (c == L'[') ++depth;
        else if (c == L']') {
            --depth;
            if (depth == 0) return text.substr(lb, i - lb + 1);
        }
    }
    return L"";
}

bool ParseObserveAfterFlag(const json& params) {
    // 默认 true：里程碑结束要验收；显式 false 表示同轮还要继续调工具
    if (!params.contains("observeAfter")) return true;
    const auto& v = params["observeAfter"];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<int>() != 0;
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s == "0" || s == "false" || s == "False" || s == "no") return false;
    }
    return true;
}

json ParseToolParams(const std::wstring& paramsJson, std::wstring& error) {
    try {
        return json::parse(ToUtf8(paramsJson));
    } catch (const json::parse_error&) {
        error = L"[错误] 参数 JSON 解析失败。";
        return json::object();
    }
}

bool PrepareAiModelFields(json& params) {
    if (!params.contains("type") || !params["type"].is_string()) return true;
    const std::string type = params["type"].get<std::string>();
    bool requireVision = false;
    if (type == "aiImageAnalysis") requireVision = true;
    else if (type == "aiActionExecute") {
        if (params.contains("aiWithImage") && params["aiWithImage"].is_boolean())
            requireVision = params["aiWithImage"].get<bool>();
        else if (params.contains("aiWithImage") && params["aiWithImage"].is_number_integer())
            requireVision = params["aiWithImage"].get<int>() != 0;
        else
            requireVision = true;
    } else if (type != "aiTextAnalysis") {
        return true;
    }

    if (ApplyResolvedAiModelToActionParams(params)) return true;

    const quickscript::AppSettings settings = LoadAgentAppSettings();
    const std::wstring preferred = FromUtf8(params.value("aiModelName", ""));
    const std::wstring resolved = ResolveAiModelName(settings.ai, requireVision, preferred);
    if (Trim(resolved).empty()) return false;
    params["aiModelName"] = ToUtf8(resolved);
    return true;
}

std::wstring ValidateAndMergeActions(const json& params, bool allowAbsolutePointer) {
    if (!params.contains("actions") || !params["actions"].is_array())
        return L"[错误] 缺少 actions 数组。场景/分步：lookupMacroAction(section=agent|usage)。\n\n"
            + ScriptActionCatalog();

    std::vector<json> items;
    for (const auto& item : params["actions"]) {
        if (item.is_object()) items.push_back(item);
    }
    if (items.empty())
        return L"[错误] actions 数组为空。";

    // 先展开 children，再按扁平列表做坐标/必填检查，最后整批构建。
    // 逐条 Build 会在每个片段末尾插 stopMacro，有限循环的循环体会被拆到循环外。
    std::vector<json> flat;
    std::wstring flattenErr;
    if (!FlattenNestedActionParamList(items, flat, flattenErr))
        return L"[错误] " + flattenErr;

    auto isTruthyMoveFromVar = [](const json& one) -> bool {
        if (!one.contains("moveFromVar")) return false;
        const auto& mv = one["moveFromVar"];
        if (mv.is_boolean()) return mv.get<bool>();
        if (mv.is_number_integer()) return mv.get<int>() != 0;
        return false;
    };

    std::vector<json> prepared;
    prepared.reserve(flat.size());
    for (size_t i = 0; i < flat.size(); ++i) {
        json one = flat[i];
        if (!one.contains("type") || !one["type"].is_string())
            return L"[错误] 第 " + std::to_wstring(i + 1) + L" 个动作缺少 type。";

        const std::string type = one["type"].get<std::string>();
        // 嵌套 aiActionExecute：允许但有深度熔断（顶层+1）；优先用原子动作
        if (type == "aiActionExecute" && !AiActionExecuteCanNestMore()) {
            return L"[错误] aiActionExecute 嵌套已达上限（最多顶层再嵌 1 层）。"
                L"请改用键鼠/找图/快捷键等原子动作，或结束本层后再分轮处理。";
        }
        if (type == "aiActionExecute") {
            std::wstring nestedPrompt;
            if (one.contains("aiPrompt") && one["aiPrompt"].is_string())
                nestedPrompt = FromUtf8(one["aiPrompt"].get<std::string>());
            if (LooksLikeLocateOnlyOutsourcePrompt(nestedPrompt)) {
                return L"[错误] 禁止用 aiActionExecute 外包「定位/点击按钮」。"
                    L"请 locateAndClick(target=短描述，≤"
                    + std::to_wstring(kMaxLocateAndClickTargetChars)
                    + L"字)；宿主会开无历史识图会话，勿把整段任务塞进嵌套 Agent。";
            }
        }
        if (type == "quickInput") {
            std::wstring inputText;
            if (one.contains("inputText") && one["inputText"].is_string())
                inputText = FromUtf8(one["inputText"].get<std::string>());
            else if (one.contains("text") && one["text"].is_string())
                inputText = FromUtf8(one["text"].get<std::string>());
            if (Trim(inputText).empty()) {
                return L"[错误] 第 " + std::to_wstring(i + 1)
                    + L" 个 quickInput 的 inputText 不能为空。"
                    L"请把要输入/回复的正文写入 inputText（勿只写在思考里）。";
            }
            // 兼容模型误用 text 字段
            one["inputText"] = ToUtf8(inputText);
        }
        if (type == "keyClick" || type == "keyDown" || type == "keyUp") {
            const bool hasKeyText = one.contains("keyText") && one["keyText"].is_string()
                && !Trim(FromUtf8(one["keyText"].get<std::string>())).empty();
            const bool hasKeyVk = one.contains("keyVk") && one["keyVk"].is_number_integer()
                && one["keyVk"].get<int>() != 0;
            if (!hasKeyText && !hasKeyVk) {
                return L"[错误] 第 " + std::to_wstring(i + 1)
                    + L" 个 " + FromUtf8(type)
                    + L" 必须指定 keyText（如 Enter）或 keyVk。"
                    L"发送消息用 keyText=\"Enter\"，禁止省略（省略会误变成按键 7）。";
            }
        }
        if (type == "mouseClick" || type == "moveMouse" || type == "mouseDown" || type == "mouseUp") {
            if (!isTruthyMoveFromVar(one)) {
                const bool hasX = one.contains("x");
                const bool hasY = one.contains("y");
                if (!hasX || !hasY) {
                    return L"[错误] 第 " + std::to_wstring(i + 1)
                        + L" 个 " + FromUtf8(type)
                        + L" 缺少 x/y。点击输入框须给截图坐标；若输入框已聚焦可直接 quickInput，勿乱点。";
                }
            }
        }
        if (!allowAbsolutePointer
            && (type == "moveMouse" || type == "mouseClick"
                || type == "mouseDown" || type == "mouseUp")
            && !isTruthyMoveFromVar(one)
            && one.contains("x") && one.contains("y")) {
            return L"[错误] 无截图时禁止用绝对坐标点击/移动。"
                L"请改用 hotkeyShortcut、keyClick、quickInput、findImage（模板图），"
                L"或开启「带截图」后再定位。";
        }

        if (!PrepareAiModelFields(one))
            return L"[错误] 第 " + std::to_wstring(i + 1)
                + L" 个 AI 动作未配置可用模型。";
        prepared.push_back(std::move(one));
    }

    std::wstring err;
    const std::wstring jsonArray = BuildScriptActionsJsonArray(prepared, err, true);
    if (!err.empty())
        return L"[错误] " + err;
    const std::wstring arr = ExtractLeadingJsonArray(jsonArray);
    if (arr.empty())
        return L"[错误] 动作无有效 JSON 数组。";
    try {
        return FromUtf8(json::parse(ToUtf8(arr)).dump());
    } catch (...) {
        return L"[错误] 动作 JSON 解析失败。";
    }
}

/// 工具结果入历史必须短：完整动作 JSON 会在每轮 API 重复计费
std::wstring SummarizeExecutedActionsJson(const std::wstring& merged) {
    try {
        const std::wstring arr = ExtractLeadingJsonArray(merged);
        if (arr.empty()) return L"已执行动作";
        const json parsed = json::parse(ToUtf8(arr));
        if (!parsed.is_array()) return L"已执行动作";
        std::wstring s = L"已执行:";
        int n = 0;
        for (const auto& step : parsed) {
            if (!step.is_object() || !step.contains("type") || !step["type"].is_string())
                continue;
            const std::string t = step["type"].get<std::string>();
            if (t == "stopMacro") continue;
            if (n++) s += L", ";
            s += FromUtf8(t);
            if (t == "quickInput" && step.contains("inputText") && step["inputText"].is_string()) {
                std::string txt = step["inputText"].get<std::string>();
                if (txt.size() > 20) txt = txt.substr(0, 20) + "...";
                s += L"(\"" + FromUtf8(txt) + L"\")";
            } else if ((t == "openWebpage" || t == "runProgram" || t == "openFile")
                && step.contains("targetPath") && step["targetPath"].is_string()) {
                std::string p = step["targetPath"].get<std::string>();
                if (p.size() > 36) p = p.substr(0, 36) + "...";
                s += L"(" + FromUtf8(p) + L")";
            } else if (t == "hotkeyShortcut" && step.contains("shortcutPreset")
                && step["shortcutPreset"].is_number_integer()) {
                s += L"(#" + std::to_wstring(step["shortcutPreset"].get<int>()) + L")";
            } else if (t == "keyClick" && step.contains("keyText") && step["keyText"].is_string()) {
                s += L"(" + FromUtf8(step["keyText"].get<std::string>()) + L")";
            } else if (t == "wait" && step.contains("duration") && step["duration"].is_number()) {
                s += L"(" + std::to_wstring(step["duration"].get<double>()) + L"s)";
            }
            if (n >= 8) {
                s += L"…";
                break;
            }
        }
        return n > 0 ? s : L"已执行动作";
    } catch (...) {
        return L"已执行动作";
    }
}

std::wstring FinishExecuteActions(
    AiActionHostHooks* hooks, const std::wstring& merged, bool observeAfter) {
    if (merged.rfind(L"[错误]", 0) == 0) return merged;
    ++AiSession().actionSeq;
    bool needObserve = observeAfter;
    const std::wstring summary = SummarizeExecutedActionsJson(merged);
    std::wstring execMsg;
    if (hooks && hooks->onExecuteActions) {
        execMsg = hooks->onExecuteActions(merged);
        // 宿主拒绝全部步骤时勿包一层 EXECUTED，否则 Agent 会误判已执行成功
        if (execMsg.rfind(L"[错误]", 0) == 0) return execMsg;
    }
    // Midscene act→observe：不确定结果必须截屏，禁止同轮连打
    auto uncertain = [](const std::wstring& s) {
        return s.find(L"[UIA]") != std::wstring::npos
            || (s.find(L"提交钮") != std::wstring::npos
                && s.find(L"不可用") != std::wstring::npos)
            || s.find(L"[事实] settle无反应") != std::wstring::npos
            || s.find(L"几乎无变化") != std::wstring::npos
            || s.find(L"灰色不可用") != std::wstring::npos
            || s.find(L"当前不可用") != std::wstring::npos;
    };
    if (uncertain(execMsg) || uncertain(summary)) needObserve = true;
    const wchar_t* marker = needObserve ? kExecutedObserveMarker : kExecutedSkipObserveMarker;
    std::wstring out = std::wstring(marker) + L"\n" + summary;
    if (!execMsg.empty()) out += L"\n" + execMsg;
    return out;
}

AgentTool MakeLookupMacroActionTool() {
    AgentTool tool;
    tool.name = L"lookupMacroAction";
    tool.description =
        L"按需查阅参数。优先直接调规范工具；勿每轮开头先查 section=agent/usage（费 token）。"
        L"仅缺某 type 字段时查 type=该名；组合定位 section=composite。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "type": {
                "type": "string",
                "description": "动作 type，或 section：agent|usage|composite|mouse|keyboard|flow|findImage|ocr|system|ai|all|catalog"
            }
        },
        "required": ["type"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        std::wstring query;
        if (params.contains("type") && params["type"].is_string())
            query = FromUtf8(params["type"].get<std::string>());
        if (Trim(query).empty()) query = L"usage";
        std::wstring body = LookupMacroActionSchema(query);
        // 入历史截断，避免 Skill 全文在后续每轮重复计费
        constexpr size_t kMax = 900;
        if (body.size() > kMax) {
            body.resize(kMax);
            body += L"\n…(已截断；勿再 lookup 同节，缺字段再查具体 type=)";
        }
        return body;
    };
    return tool;
}

AgentTool MakeCompleteTaskTool() {
    AgentTool tool;
    tool.name = L"completeTask";
    tool.description =
        L"任务验收通过或确认无需再操作时调用。reason 可选（如「已发送回复」或「对方未发言」）。"
        L"调用后本轮 Agent 闭环结束。若上一工具返回 [错误]，禁止调用本工具，须先修正再执行。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "reason": {
                "type": "string",
                "description": "简短完成原因（可选）"
            }
        }
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        std::wstring reason;
        if (parseError.empty() && params.contains("reason") && params["reason"].is_string())
            reason = FromUtf8(params["reason"].get<std::string>());
        if (reason.empty()) return std::wstring(kTaskCompleteMarker);
        return std::wstring(kTaskCompleteMarker) + L" " + reason;
    };
    return tool;
}

/// 单动作规范工具：工具名 = 宏 type，参数字段强制 schema，内部走同一校验/执行管线
AgentTool MakeAtomicMacroTool(
    const wchar_t* typeName,
    const wchar_t* description,
    const wchar_t* parametersJson,
    AiActionHostHooks* hooks,
    bool allowAbsolutePointer,
    bool defaultObserveAfter) {
    AgentTool tool;
    tool.name = typeName;
    tool.description = description;
    tool.parameters_json = parametersJson;
    const std::string typeUtf8 = ToUtf8(std::wstring(typeName));
    tool.execute = [hooks, allowAbsolutePointer, defaultObserveAfter, typeUtf8](
        const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();
        params["type"] = typeUtf8;

        bool observeAfter = defaultObserveAfter;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);

        json wrap = json::object();
        wrap["actions"] = json::array({ params });
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbsolutePointer);
        return FinishExecuteActions(hooks, merged, observeAfter);
    };
    return tool;
}

/// 把配方里的 {0}/{1}/… 或 {col0}/{col1}/… 替换成当前行的值
std::wstring SubstituteRecipePlaceholders(std::wstring text, const json& row) {
    if (!row.is_array()) return text;
    for (size_t i = 0; i < row.size() && i < 32; ++i) {
        std::wstring value;
        const auto& cell = row[i];
        if (cell.is_string()) value = FromUtf8(cell.get<std::string>());
        else if (cell.is_number_integer()) value = std::to_wstring(cell.get<long long>());
        else if (cell.is_number_float()) value = std::to_wstring(cell.get<double>());
        else if (cell.is_boolean()) value = cell.get<bool>() ? L"true" : L"false";
        else if (cell.is_null()) value.clear();
        else value = FromUtf8(cell.dump());

        const std::wstring brace = L"{" + std::to_wstring(i) + L"}";
        const std::wstring col = L"{col" + std::to_wstring(i) + L"}";
        for (size_t p = text.find(brace); p != std::wstring::npos; p = text.find(brace, p)) {
            text.replace(p, brace.size(), value);
            p += value.size();
        }
        for (size_t p = text.find(col); p != std::wstring::npos; p = text.find(col, p)) {
            text.replace(p, col.size(), value);
            p += value.size();
        }
    }
    return text;
}

json SubstituteRecipeStep(json step, const json& row) {
    if (!step.is_object()) return step;
    for (auto it = step.begin(); it != step.end(); ++it) {
        if (!it.value().is_string()) continue;
        const std::wstring key = FromUtf8(it.key());
        // type 字段不替换；只替换文本类字段
        if (key == L"type") continue;
        it.value() = ToUtf8(SubstituteRecipePlaceholders(
            FromUtf8(it.value().get<std::string>()), row));
    }
    return step;
}

bool IsRecipeAllowedStepType(const std::string& type) {
    return type == "quickInput" || type == "keyClick" || type == "wait"
        || type == "hotkeyShortcut" || type == "keyDown" || type == "keyUp";
}

/// 配方模板的规范化签名（不含 rows 数据值，只含动作结构/键位/占位符）
std::wstring RecipeStepsSignature(const json& steps) {
    std::wstring sig;
    if (!steps.is_array()) return sig;
    for (const auto& step : steps) {
        if (!step.is_object()) continue;
        sig += L"[";
        if (step.contains("type") && step["type"].is_string())
            sig += L"t=" + FromUtf8(step["type"].get<std::string>());
        if (step.contains("keyText") && step["keyText"].is_string())
            sig += L";k=" + FromUtf8(step["keyText"].get<std::string>());
        if (step.contains("inputText") && step["inputText"].is_string())
            sig += L";i=" + FromUtf8(step["inputText"].get<std::string>());
        if (step.contains("shortcutPreset") && step["shortcutPreset"].is_number_integer())
            sig += L";s=" + std::to_wstring(step["shortcutPreset"].get<int>());
        if (step.contains("duration") && step["duration"].is_number())
            sig += L";d=" + std::to_wstring(step["duration"].get<double>());
        if (step.contains("clearFirst") && step["clearFirst"].is_boolean())
            sig += L";c=" + std::wstring(step["clearFirst"].get<bool>() ? L"1" : L"0");
        if (step.contains("holdLeftCtrl") && step["holdLeftCtrl"].is_boolean())
            sig += L";cc=" + std::wstring(step["holdLeftCtrl"].get<bool>() ? L"1" : L"0");
        if (step.contains("holdLeftAlt") && step["holdLeftAlt"].is_boolean())
            sig += L";ca=" + std::wstring(step["holdLeftAlt"].get<bool>() ? L"1" : L"0");
        if (step.contains("holdLeftShift") && step["holdLeftShift"].is_boolean())
            sig += L";cs=" + std::wstring(step["holdLeftShift"].get<bool>() ? L"1" : L"0");
        if (step.contains("holdLeftWin") && step["holdLeftWin"].is_boolean())
            sig += L";cw=" + std::wstring(step["holdLeftWin"].get<bool>() ? L"1" : L"0");
        sig += L"]";
    }
    return sig;
}

/// 配方签名 = 仅 steps 模板。核对通过后同一模板可带着任意剩余 rows 全量复用，
/// 不再因「只传剩余行」换签名而反复试跑（旧逻辑把前 2 行数据编进签名，导致每 2 行一检）。
std::wstring RecipeBatchSignature(const json& steps, const json& rows) {
    (void)rows;
    return RecipeStepsSignature(steps);
}

/// 行指纹（用于判断确认调用是否仍带试跑前缀）
std::wstring RecipeRowsFingerprint(const json& rows, size_t count) {
    std::wstring fp;
    if (!rows.is_array()) return fp;
    const size_t n = std::min(count, rows.size());
    for (size_t i = 0; i < n; ++i) {
        fp += L"{";
        const auto& row = rows[i];
        if (row.is_array()) {
            for (const auto& cell : row) {
                if (cell.is_string()) fp += FromUtf8(cell.get<std::string>());
                else if (cell.is_number_integer()) fp += std::to_wstring(cell.get<long long>());
                else if (cell.is_number_float()) fp += std::to_wstring(cell.get<double>());
                else if (cell.is_boolean()) fp += cell.get<bool>() ? L"1" : L"0";
                else if (!cell.is_null()) fp += FromUtf8(cell.dump());
                fp += L"|";
            }
        }
        fp += L"}";
    }
    return fp;
}

/// 试跑时期望值摘要（仅用于错误/提示文案）
std::wstring RecipeRowsExpectedHint(const json& rows, size_t count) {
    std::wstring expected;
    if (!rows.is_array()) return expected;
    const size_t n = std::min(count, rows.size());
    for (size_t i = 0; i < n; ++i) {
        expected += L"  第" + std::to_wstring(i + 1) + L"行期望值：";
        const auto& row = rows[i];
        if (row.is_array()) {
            for (const auto& cell : row) {
                if (cell.is_string()) expected += L"「" + FromUtf8(cell.get<std::string>()) + L"」 ";
                else if (cell.is_number_integer())
                    expected += L"「" + std::to_wstring(cell.get<long long>()) + L"」 ";
                else if (cell.is_number())
                    expected += L"「" + std::to_wstring(cell.get<double>()) + L"」 ";
            }
        }
        expected += L"\n";
    }
    return expected;
}
std::wstring ExpandActionRecipe(const json& params, std::wstring& error,
    std::wstring* warningOut = nullptr) {
    if (warningOut) warningOut->clear();
    error.clear();
    if (!params.is_object()) {
        error = L"[错误] runActionRecipe 参数须为对象。";
        return {};
    }
    if (!params.contains("steps") || !params["steps"].is_array() || params["steps"].empty()) {
        error = L"[错误] runActionRecipe 需要非空 steps 模板（一组可复用的键盘动作）。";
        return {};
    }
    if (!params.contains("rows") || !params["rows"].is_array() || params["rows"].empty()) {
        error = L"[错误] runActionRecipe 需要非空 rows（每行一组占位符取值）。"
            L"只有一组也请放进 rows:[[...]]。";
        return {};
    }
    const auto& steps = params["steps"];
    const auto& rows = params["rows"];
    constexpr size_t kMaxSteps = 24;
    constexpr size_t kMaxRows = 40;
    constexpr size_t kMaxExpanded = 480;
    if (steps.size() > kMaxSteps) {
        error = L"[错误] steps 最多 " + std::to_wstring(kMaxSteps) + L" 步。";
        return {};
    }
    if (rows.size() > kMaxRows) {
        error = L"[错误] rows 最多 " + std::to_wstring(kMaxRows) + L" 组；请拆成两次调用。";
        return {};
    }
    if (steps.size() * rows.size() > kMaxExpanded) {
        error = L"[错误] 展开后动作过多（上限 " + std::to_wstring(kMaxExpanded) + L"）。";
        return {};
    }

    json out = json::array();
    // 固定文本检测：quickInput 的 inputText 若不含 {N} 占位符，会在每组重复写入。
    // 这通常是表头/标题被误放进模板（每行都写表头会覆盖数据），收集起来返回警告。
    std::wstring fixedTextWarn;
    {
        std::wstring fixedTexts;
        for (const auto& step : steps) {
            if (step.contains("type") && step["type"].is_string()
                && step["type"].get<std::string>() == "quickInput"
                && step.contains("inputText") && step["inputText"].is_string()) {
                const std::string t = step["inputText"].get<std::string>();
                if (t.find('{') == std::string::npos && !t.empty()) {
                    if (!fixedTexts.empty()) fixedTexts += L"、";
                    fixedTexts += L"「" + FromUtf8(t) + L"」";
                }
            }
        }
        if (!fixedTexts.empty()) {
            fixedTextWarn = L"★警告：模板含固定文本 " + fixedTexts
                + L"（不含 {N} 占位符），会在每组重复写入同一内容。"
                L"若这是表头/列名，应先用一次 quickInput 只写一遍表头，"
                L"模板里只放每行都要重复的数据录入步骤；否则每行都会重写表头覆盖数据。";
        }
    }
    for (size_t si = 0; si < steps.size(); ++si) {
        const auto& step = steps[si];
        if (!step.is_object() || !step.contains("type") || !step["type"].is_string()) {
            error = L"[错误] steps[" + std::to_wstring(si) + L"] 须含 type。";
            return {};
        }
        const std::string type = step["type"].get<std::string>();
        if (!IsRecipeAllowedStepType(type)) {
            error = L"[错误] 配方只允许键盘类动作（quickInput/keyClick/wait/hotkeyShortcut），"
                L"不含 locateAndClick/mouseClick——坐标动作不能无脑复用。"
                L"非法 type=" + FromUtf8(type);
            return {};
        }
    }
    for (size_t ri = 0; ri < rows.size(); ++ri) {
        const auto& row = rows[ri];
        if (!row.is_array()) {
            error = L"[错误] rows 的每一项必须是数组，如 [\"值0\",\"值1\"]。";
            return {};
        }
        for (size_t si = 0; si < steps.size(); ++si) {
            const auto& step = steps[si];
            json sub = SubstituteRecipeStep(step, row);
            if (step.contains("type") && step["type"].is_string()
                && step["type"].get<std::string>() == "quickInput") {
                std::wstring itxt;
                if (sub.contains("inputText") && sub["inputText"].is_string())
                    itxt = FromUtf8(sub["inputText"].get<std::string>());
                // 未替换的 {N} 或空单元格 → 展开后会变成空 quickInput，引发后续盲 Tab 错位
                if (itxt.find(L'{') != std::wstring::npos && itxt.find(L'}') != std::wstring::npos) {
                    error = L"[错误] rows[" + std::to_wstring(ri) + L"] 列数不够："
                        L"步骤含未替换占位符「" + itxt + L"」。"
                        L"每行单元格数须覆盖 steps 里最大 {N}；缺列请补空串以外的真实值或删掉该步。";
                    return {};
                }
                if (Trim(itxt).empty()) {
                    error = L"[错误] rows[" + std::to_wstring(ri) + L"] 展开后出现空 quickInput。"
                        L"禁止空单元格占位；请补全该行数据或从 rows 去掉该组。"
                        L"★勿在配方失败后连按 Tab 盲填——先 Ctrl+Home 回 A1，修好 rows 再 runActionRecipe。";
                    return {};
                }
            }
            out.push_back(std::move(sub));
        }
    }
    if (warningOut) *warningOut = std::move(fixedTextWarn);
    return FromUtf8(out.dump());
}
AgentTool MakeRunActionRecipeTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"runActionRecipe";
    tool.description =
        L"★重复动作复用：把键盘步骤写成模板 steps，再用 rows 填多组数据，"
        L"宿主本地展开一次执行，中间不截屏/不识图。"
        L"★表格逐行录入的组间导航（通用键位）：行内用 Tab 逐列；"
        L"填完一行按 Enter 只下移一格（保持当前列），必须再 keyClick(Home) 才能回到下一行首列"
        L"（Excel/WPS/Google Sheets 等表格软件通用）。模板要把 Enter+Home 一并写进组间。"
        L"表头写完 Enter+Home 后禁止再 Down（会空一行）；起点用 Ctrl+Home 到 A1，禁止猜坐标点格。"
        L"★一次核对、全程复用：rows≥2 时宿主只试跑前 2 组（rows=2 时前 1 组）并截屏；"
        L"你逐格核对通过后，用相同 steps + verifiedGroups=试跑组数 再次调用，"
        L"rows 可带【全部剩余行】（也可仍带完整原 rows，宿主会跳过已试跑前缀）。"
        L"核对通过后该 steps 模板即被信任：后续同模板任意 rows 全部一次跑完，不再每 2 行重检；"
        L"全部填完后统一看一次截图验收即可。改 steps 模板会重新试跑。"
        L"若某格错了：Delete/Backspace 清错再重填或改模板；禁止拆成「写2行→检→写2行→检」。"
        L"★多组 rows 时模板只放【每行都要重复的数据录入步骤】；表头/列名不要放进模板。"
        L"★写表头：用 rows 只有 1 组的配方（允许固定文本），或几次 quickInput，勿拆成十几轮 API。"
        L"占位符 {0}/{1}；只允许 quickInput/keyClick/wait/hotkeyShortcut。"
        L"默认整批结束后观察一次。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "steps": {
                "type": "array",
                "description": "一组可复用的键盘动作模板，inputText 里写 {0}/{1} 占位符",
                "minItems": 1,
                "items": {
                    "type": "object",
                    "properties": {
                        "type": {
                            "type": "string",
                            "enum": ["quickInput", "keyClick", "wait", "hotkeyShortcut"]
                        },
                        "inputText": { "type": "string" },
                        "keyText": { "type": "string" },
                        "duration": { "type": "number" },
                        "shortcutPreset": { "type": "integer" },
                        "clearFirst": { "type": "boolean" },
                        "holdLeftCtrl": { "type": "boolean" },
                        "holdLeftAlt": { "type": "boolean" },
                        "holdLeftShift": { "type": "boolean" },
                        "holdLeftWin": { "type": "boolean" }
                    },
                    "required": ["type"]
                }
            },
            "rows": {
                "type": "array",
                "description": "多组取值，每组是数组；与 steps 笛卡尔展开",
                "minItems": 1,
                "items": {
                    "type": "array",
                    "items": { "type": ["string", "number", "boolean"] }
                }
            },
            "verifiedGroups": {
                "type": "integer",
                "minimum": 0,
                "description": "已核对通过的试跑组数。首次试跑后须≥试跑组数；模板信任后可省略或继续传"
            },
            "observeAfter": {
                "type": "boolean",
                "description": "整批结束后是否截屏；默认 true。同轮还要继续可 false"
            }
        },
        "required": ["steps", "rows"]
    })";
    tool.execute = [hooks, allowAbs](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;

        const auto& steps = params.contains("steps") ? params["steps"] : json();
        const auto& rows = params.contains("rows") ? params["rows"] : json();
        const size_t nRows = rows.is_array() ? rows.size() : 0;

        // 一次核对、全程复用（签名仅 steps）：
        //   · 未信任模板 + rows≥2：只试跑前 1/2 组并截屏
        //   · 同 steps + verifiedGroups≥试跑组数：标记信任；跳过本次 rows 中已试跑前缀（若有），
        //     其余一次跑完；之后同 steps 任意 rows 全量执行、不再试跑
        //   · 改 steps → 重新试跑
        bool previewMode = false;
        size_t skipGroups = 0;
        int verifiedGroups = 0;
        if (params.contains("verifiedGroups") && params["verifiedGroups"].is_number())
            verifiedGroups = params["verifiedGroups"].get<int>();
        const std::wstring stepsSig = RecipeBatchSignature(steps, rows);
        if (nRows >= 1 && AiSession().recipeTemplateTrusted
            && stepsSig == AiSession().recipeVerifiedSig) {
            previewMode = false;
            skipGroups = 0;
        } else if (nRows >= 1 && stepsSig == AiSession().recipeVerifiedSig
            && AiSession().recipePreviewGroups > 0
            && !AiSession().recipeTemplateTrusted) {
            if (verifiedGroups < AiSession().recipePreviewGroups) {
                const size_t checkN = static_cast<size_t>(AiSession().recipePreviewGroups);
                const std::wstring expected = RecipeRowsExpectedHint(rows, checkN);
                return L"[错误] 试跑的前 " + std::to_wstring(checkN) + L" 组还没有被你确认正确。"
                    L"请先对照刚返回的截图逐格核对（期望值：\n" + expected
                    + L"）。确认正确后：用相同 steps，rows 放入【全部剩余行】"
                      L"（或仍带完整原 rows），并传 verifiedGroups="
                    + std::to_wstring(checkN)
                    + L" 再次调用——宿主会一次填完剩余，之后同模板不再分段重检。"
                    L"若发现错格：Delete/Backspace 清错再重填或改模板后重新试跑。"
                    L"禁止拆成每 2 行检查一次。";
            }
            AiSession().recipeTemplateTrusted = true;
            // 仍带试跑前缀 → 跳过；只传剩余行 → 全跑
            const size_t prefN = static_cast<size_t>(AiSession().recipePreviewGroups);
            if (!AiSession().recipePreviewRowsFp.empty()
                && RecipeRowsFingerprint(rows, prefN) == AiSession().recipePreviewRowsFp
                && nRows > prefN) {
                skipGroups = prefN;
            } else {
                skipGroups = 0;
            }
        } else if (nRows >= 2) {
            const size_t previewTarget = (nRows >= 3) ? 2 : 1;
            previewMode = true;
            skipGroups = 0;
            AiSession().recipeVerifiedSig = stepsSig;
            AiSession().recipePreviewGroups = static_cast<int>(previewTarget);
            AiSession().recipeTemplateTrusted = false;
            AiSession().recipePreviewRowsFp.clear();
        }

        json rowsToRun = json::array();
        for (size_t i = skipGroups; i < nRows; ++i) rowsToRun.push_back(rows[i]);
        const size_t previewTarget = (nRows >= 3) ? 2 : 1;
        if (previewMode && rowsToRun.size() > previewTarget)
            rowsToRun.erase(rowsToRun.begin() + static_cast<std::ptrdiff_t>(previewTarget),
                rowsToRun.end());
        if (rowsToRun.empty()) {
            if (AiSession().recipeTemplateTrusted && skipGroups > 0) {
                return L"[已执行] 试跑前缀已跳过且无剩余行（模板已信任）。"
                    L"若还有数据，请用相同 steps 传入剩余 rows 一次跑完。";
            }
            return L"[错误] 没有可执行的组。";
        }

        json subParams = params;
        subParams["rows"] = rowsToRun;
        std::wstring expandError;
        std::wstring fixedTextWarn;
        const std::wstring actionsJson = ExpandActionRecipe(subParams, expandError, &fixedTextWarn);
        if (!expandError.empty()) return expandError;
        // rows=1：允许模板含固定文本（一次写表头/单行字面量），避免拆成 N 轮 quickInput 烧 API
        if (!fixedTextWarn.empty() && nRows >= 2) {
            return L"[错误] " + fixedTextWarn
                + L"\n已阻止执行：多组 rows 时模板里不允许写固定文本（表头/列名）。"
                L"正确做法：先用 rows=[[...]] 一次写完表头（或多次 quickInput），"
                L"然后模板 steps 里每行只放数据录入步骤（quickInput 用 {0}/{1} 占位符 + "
                L"Tab/Enter/Home 导航）。若某列每行都要填相同固定值，请把该值写进对应行 "
                L"rows 的单元格里，而不是写进模板。请重写模板后重新调用本工具。";
        }
        json wrap = json::object();
        try {
            wrap["actions"] = json::parse(ToUtf8(actionsJson));
        } catch (...) {
            return L"[错误] 配方展开结果不是合法 JSON。";
        }
        const bool observeAfter = previewMode ? true
            : (params.contains("observeAfter") ? ParseObserveAfterFlag(params) : true);
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        std::wstring out = FinishExecuteActions(hooks, merged, observeAfter);
        if (previewMode) {
            const size_t checkN = std::min<size_t>(rowsToRun.size(),
                static_cast<size_t>(AiSession().recipePreviewGroups));
            AiSession().recipePreviewRowsFp = RecipeRowsFingerprint(rowsToRun, checkN);
            const std::wstring expected = RecipeRowsExpectedHint(rowsToRun, checkN);
            out += L"\n★强制试跑：已先执行前 " + std::to_wstring(checkN) + L" 组验证（非全部）。"
                L"请对照截图逐格核对（期望值：\n" + expected
                + L"）——重点看列对齐、组间 Home、有无残留字母。"
                L"确认正确后：用相同 steps，把【全部剩余行】放进 rows，"
                L"并传 verifiedGroups=" + std::to_wstring(checkN)
                + L" 一次跑完；此后同模板不必再分段检查。"
                L"禁止「写2行→检查→再写2行」。错了先 Delete 清错或改模板重试。";
        } else if (out.rfind(L"[错误]", 0) != 0) {
            if (!AiSession().recipeTemplateTrusted && verifiedGroups > 0
                && stepsSig == AiSession().recipeVerifiedSig) {
                AiSession().recipeTemplateTrusted = true;
            }
            if (AiSession().recipeTemplateTrusted) {
                out += L"\n（模板已信任：本批 " + std::to_wstring(rowsToRun.size())
                    + L" 组已一次执行；同 steps 后续 rows 可继续全量复用，填完再统一验收）";
            }
            if (skipGroups > 0) {
                out += L"\n（已跳过试跑前缀 " + std::to_wstring(skipGroups) + L" 组）";
            }
        }
        if (out.rfind(L"[错误]", 0) != 0) {
            UpsertAiTaskMemoSection(L"recipe",
                L"rows=" + std::to_wstring(nRows) + L" steps=" + std::to_wstring(steps.size())
                    + L" preview=" + (previewMode ? L"1" : L"0")
                    + L" trusted=" + (AiSession().recipeTemplateTrusted ? L"1" : L"0"),
                true);
        }
        return out;
    };
    return tool;
}

AgentTool MakeSubmitMacroActionsToolLocal(AiActionHostHooks* hooks, bool allowAbsolutePointer) {
    AgentTool tool;
    tool.name = L"submitMacroActions";
    tool.description =
        L"【批量/冷门】一次提交多个动作或 findImage/OCR/流程等冷门 type。"
        L"常用请优先调用规范工具 quickInput / keyClick / mouseClick / moveMouse / wait / hotkeyShortcut，"
        L"其参数已强制必填，比本工具更可靠。"
        L"observeAfter 默认 true；同轮继续调工具时设 false。"
        L"每项必须含 type 及该类型必填字段（quickInput 要 inputText；keyClick 要 keyText 或 keyVk）。";
    // items 用 oneOf 约束高频 type 的必填字段；其余 type 仍允许（additional 走校验）
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "actions": {
                "type": "array",
                "description": "动作对象数组。优先改用同名规范工具。",
                "items": {
                    "oneOf": [
                        {
                            "type": "object",
                            "properties": {
                                "type": { "const": "quickInput" },
                                "inputText": { "type": "string", "minLength": 1 },
                                "charInterval": { "type": "number" },
                                "parseEscapes": { "type": "boolean" }
                            },
                            "required": ["type", "inputText"]
                        },
                        {
                            "type": "object",
                            "properties": {
                                "type": { "const": "keyClick" },
                                "keyText": { "type": "string", "minLength": 1 },
                                "keyVk": { "type": "integer" }
                            },
                            "required": ["type", "keyText"]
                        },
                        {
                            "type": "object",
                            "properties": {
                                "type": { "const": "mouseClick" },
                                "x": { "type": "number" },
                                "y": { "type": "number" },
                                "button": { "type": "string" }
                            },
                            "required": ["type", "x", "y"]
                        },
                        {
                            "type": "object",
                            "properties": {
                                "type": { "const": "moveMouse" },
                                "x": { "type": "number" },
                                "y": { "type": "number" }
                            },
                            "required": ["type", "x", "y"]
                        },
                        {
                            "type": "object",
                            "properties": {
                                "type": { "const": "wait" },
                                "duration": { "type": "number" }
                            },
                            "required": ["type", "duration"]
                        },
                        {
                            "type": "object",
                            "properties": {
                                "type": { "type": "string" }
                            },
                            "required": ["type"],
                            "additionalProperties": true
                        }
                    ]
                }
            },
            "observeAfter": {
                "type": "boolean",
                "description": "true=执行后截屏验收（默认）；false=同轮继续调用工具"
            }
        },
        "required": ["actions"]
    })";
    tool.execute = [hooks, allowAbsolutePointer](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        const bool observeAfter = ParseObserveAfterFlag(params);
        const std::wstring merged = ValidateAndMergeActions(params, allowAbsolutePointer);
        return FinishExecuteActions(hooks, merged, observeAfter);
    };
    return tool;
}

AgentTool MakeQuickInputTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"quickInput";
    tool.description =
        L"往当前焦点输入文字。inputText 必填且非空。"
        L"★★先判断意图再设 clearFirst："
        L"命名/改成/填写/覆盖 → clearFirst=true（默认：宿主 Ctrl+A+Delete 清框再写，避免拼在旧内容后）；"
        L"在已有文字后追加/添加 → clearFirst=false。"
        L"★另存为文件名/地址栏几乎总是替换：用 true（Excel 另存为也会清框；地址栏勿追加路径）。"
        L"★表格单元格：点格后直接输入即覆盖，宿主跳过清框（避免 Ctrl+A 全选整表）。"
        L"★F2 重命名：clearFirst=false（系统已选中主名）；inputText 只写主名勿带扩展名。"
        L"路径用 resolveSystemPath 核对；另存为少往文件名框塞盘符路径。"
        L"默认 observeAfter=false。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "inputText": {
                "type": "string",
                "minLength": 1,
                "description": "要输入的正文；重命名时只写主文件名（不含扩展名）"
            },
            "charInterval": {
                "type": "number",
                "description": "字间间隔秒，默认 0.01"
            },
            "parseEscapes": {
                "type": "boolean",
                "description": "true=把字面 \\n \\t \\\\ 转成换行/Tab/反斜杠；默认 false（路径按字面）"
            },
            "observeAfter": {
                "type": "boolean",
                "description": "true=执行后截屏；默认 false 以便同轮继续发送"
            },
            "clearFirst": {
                "type": "boolean",
                "description": "true=替换框内已有内容（默认）；false=在光标处追加。命名/改名用 true；追加/添加用 false；F2 后用 false"
            }
        },
        "required": ["inputText"]
    })";
    tool.execute = [hooks, allowAbs](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();
        params["type"] = "quickInput";

        bool observeAfter = false;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);

        // 绝对路径：上级目录必须真实存在；已有目录路径不能当「文件名」提交
        std::wstring typedAbsPath;
        bool typedAbsIsDir = false;
        std::wstring text;
        {
            if (params.contains("inputText") && params["inputText"].is_string())
                text = FromUtf8(params["inputText"].get<std::string>());
            else if (params.contains("text") && params["text"].is_string())
                text = FromUtf8(params["text"].get<std::string>());
            text = Trim(text);
            if (LooksLikeAbsolutePath(text) && !AbsolutePathParentExists(text)) {
                const std::wstring desktop = KnownFolderPathByName(L"desktop");
                std::wstring msg = L"[错误] 路径「" + text + L"」的上级目录不存在，已拒绝。"
                    L"用 resolveSystemPath(folder=desktop) 取真实路径，或点左侧文件夹后只写纯文件名。";
                if (!desktop.empty()) msg += L" 本机桌面：" + desktop;
                return msg;
            }
            if (LooksLikeAbsolutePath(text) && AbsolutePathParentExists(text)) {
                typedAbsIsDir = DirectoryExists(text);
                if (!typedAbsIsDir) typedAbsPath = text; // 仅完整文件路径记 pending
            }
        }

        const std::wstring dlgKindEarly = ProbeForegroundDialogKind(hooks, nullptr);
        // 目录路径不再按应用教招拦截：执行后若提交钮变灰只回报 UIA 事实
        // 默认替换已有内容；刚 F2 过则保留系统选区（主名），避免抹掉扩展名
        bool clearFirst = !g_keepInputSelectionAfterF2;
        bool clearFirstExplicit = false;
        if (params.contains("clearFirst")) {
            clearFirstExplicit = true;
            const auto& cf = params["clearFirst"];
            if (cf.is_boolean()) clearFirst = cf.get<bool>();
            else if (cf.is_number_integer()) clearFirst = cf.get<int>() != 0;
        }
        params.erase("clearFirst");
        g_keepInputSelectionAfterF2 = false;

        const std::wstring dlgKindBefore = dlgKindEarly;
        // 另存为/打开里贴绝对路径：必须整框替换。资源管理器地址栏对 KEYEVENTF_UNICODE
        // 常「不替换选区只追加」，会出现 Documents + C:\Users\... 拼成非法路径。
        if (!clearFirstExplicit && LooksLikeAbsolutePath(text)
            && (dlgKindBefore == L"saveAs" || dlgKindBefore == L"saveAsMini"
                || dlgKindBefore == L"openFile")) {
            clearFirst = true;
        }
        json actions = json::array();
        // Excel 另存为时前台仍是表格进程：以前误跳过 Ctrl+A，默认文件名会被追加。
        // 对话框内焦点在文件名框，Ctrl+A 只选文件名；表格格子内仍跳过。
        // 选中后再 Delete：壳地址栏/部分 Edit 收到 Unicode 不会覆盖选区，只往后插。
        if (AiQuickInputNeedsCtrlAClear(clearFirst, ForegroundIsSpreadsheetApp(), dlgKindBefore)) {
            json sel = json::object();
            sel["type"] = "keyClick";
            sel["keyText"] = "A";
            sel["holdLeftCtrl"] = 1;
            sel["clickCount"] = 1;
            actions.push_back(std::move(sel));
            json del = json::object();
            del["type"] = "keyClick";
            del["keyText"] = "Delete";
            del["clickCount"] = 1;
            actions.push_back(std::move(del));
        }
        actions.push_back(params);

        json wrap = json::object();
        wrap["actions"] = std::move(actions);
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        std::wstring result = FinishExecuteActions(hooks, merged, observeAfter);
        if (result.rfind(L"[错误]", 0) != 0) {
            AiClearLocateFailKeyBlock();
            AiSession().bareTabStreak = 0;
        }
        const std::wstring dlgKind = ProbeForegroundDialogKind(hooks, nullptr);
        if ((dlgKind == L"saveAs" || dlgKind == L"saveAsMini" || dlgKindBefore == L"saveAs"
                || dlgKindBefore == L"saveAsMini")
            && clearFirstExplicit && !clearFirst
            && result.rfind(L"[错误]", 0) != 0) {
            result += L"\n提示：文件名框若本意是替换默认名，应 clearFirst=true；"
                L"仅追加时用 false。";
        }
        if (result.rfind(L"[错误]", 0) != 0) {
            std::wstring disabledBtn;
            const bool submitDisabled = hooks && hooks->onProbeDisabledSubmit
                && hooks->onProbeDisabledSubmit(disabledBtn);
            if (submitDisabled) {
                AiSession().pendingSavePath.clear();
                AiSession().submitUiBlocked = true;
                result += L"\n[UIA] 提交钮「"
                    + (disabledBtn.empty() ? L"保存" : disabledBtn)
                    + L"」当前不可用。";
            } else {
                AiSession().submitUiBlocked = false;
                if (!typedAbsPath.empty()) {
                    AiSession().pendingSavePath = typedAbsPath;
                } else if (typedAbsIsDir) {
                    result += L"\n[事实] 刚输入的是已有目录路径（非文件名）。";
                }
            }
        }
        return result;
    };
    return tool;
}

/// 本地窗口台账文本（EnumWindows Z 序，不烧图）；无宿主钩子时返回空串
std::wstring WindowLedgerText(AiActionHostHooks* hooks) {
    if (!hooks || !hooks->onListWindows) return {};
    return hooks->onListWindows();
}

/// 给「找不到窗口/切窗失败」类错误补上台账，让模型下一步就能直接 activateWindow
std::wstring WithWindowLedger(AiActionHostHooks* hooks, const std::wstring& head) {
    const std::wstring list = WindowLedgerText(hooks);
    if (list.empty()) return head;
    return head + L"\n当前已打开的窗口（本地枚举，不用识图）：\n" + list;
}

/// Win+D / Win+Down 只会藏窗口，不能唤窗；原地连按必然无效，直接拦掉并给唤窗正路
std::wstring GuardHideWindowsAction(const std::wstring& what) {
    auto& s = AiSession();
    if (s.actionSeq - s.lastHideWindowsSeq <= 3) {
        return L"[错误] 刚用过「" + what + L"」，再按只会把窗口还原/最小化，唤不出任何窗口。"
            L"唤出已打开的窗口用 activateWindow(match=\"Excel/Edge/标题一段\")；"
            L"不确定开着啥先 listWindows。"
            L"要打开桌面上的文件：openFile(完整路径，路径用 resolveSystemPath 取)，"
            L"或 locateAndClick(target=桌面图标, doubleClick=true)——单击只会选中。";
    }
    s.lastHideWindowsSeq = s.actionSeq + 1;
    return {};
}

/// 保存/覆盖框里 Escape 会取消保存：劝一次，模型坚持再放行。
std::wstring GuardEscapeInSaveDialog(AiActionHostHooks* hooks) {
    auto& s = AiSession();
    if (s.actionSeq - s.saveEscapeWarnedSeq <= 3) return {};
    std::wstring probe;
    const std::wstring kind = ProbeForegroundDialogKind(hooks, &probe);
    if (kind == L"confirmOverwrite") {
        s.saveEscapeWarnedSeq = s.actionSeq;
        std::wstring buttons = DialogProbeField(probe, L"buttons");
        if (!buttons.empty()) buttons = L"，按钮：" + buttons;
        return L"[错误] 前台是「覆盖确认」框（" + DialogProbeField(probe, L"title")
            + buttons
            + L"），Escape 会取消保存。看按钮后点选；坚持放弃请再按 Escape。";
    }
    if (kind == L"saveAs" || kind == L"saveAsMini" || kind == L"openFile") {
        s.saveEscapeWarnedSeq = s.actionSeq;
        return L"[错误] 前台是「" + DialogProbeField(probe, L"title")
            + L"」，Escape 会关掉对话框。坚持放弃请再按 Escape。";
    }
    return {};
}

bool IsLocateFailKeyWhitelist(const std::wstring& keyRaw) {
    std::wstring k = keyRaw;
    for (auto& c : k) {
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    return k == L"ENTER" || k == L"RETURN" || k == L"TAB" || k == L"ESCAPE" || k == L"ESC"
        || k == L"BACKSPACE" || k == L"DELETE" || k == L"HOME" || k == L"END"
        || k == L"UP" || k == L"DOWN" || k == L"LEFT" || k == L"RIGHT"
        || k == L"PAGEDOWN" || k == L"PAGEUP" || k == L"SPACE";
}

std::wstring GuardKeysAfterLocateFail(const std::wstring& keyText, bool confirmBlindKey,
    bool hasModifiers, AiActionHostHooks* hooks) {
    (void)hooks;
    if (!AiLocateFailKeyBlockActive()) return {};
    if (confirmBlindKey) return {};
    if (!hasModifiers && IsLocateFailKeyWhitelist(keyText)) return {};
    return L"[错误] 上轮 locate 失败：禁止乱按快捷键（已拦截 "
        + keyText + L"）。换短标签再 locateAndClick 最多1次，或看图换策略 / completeTask。"
          L"允许 Enter/Tab/Escape/方向键；其它键加 confirmBlindKey=true。";
}

std::wstring GuardBareTabStreak(const std::wstring& keyText, bool hasModifiers) {
    std::wstring k = keyText;
    for (auto& c : k) {
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    if (k == L"TAB" && !hasModifiers) {
        ++AiSession().bareTabStreak;
        if (AiSession().bareTabStreak >= 3) {
            return L"[错误] 已连续 Tab×" + std::to_wstring(AiSession().bareTabStreak)
                + L"。禁止盲 Tab 找焦点。看图确认焦点后再输入，或 Ctrl+Home / completeTask。";
        }
        return {};
    }
    AiSession().bareTabStreak = 0;
    return {};
}

/// 组合键是否属于跨软件通用快捷键（编辑/保存/查找/系统菜单等）。
/// 用于劝阻模型乱按应用专属组合键（如浏览器 Ctrl+H 历史、Excel Ctrl+G 定位）。
bool IsUniversalShortcutCombo(bool ctrl, bool alt, bool win, const std::wstring& key) {
    if (ctrl && !alt && !win) {
        if (key.size() == 1) {
            const wchar_t c = key[0];
            return c == L'A' || c == L'C' || c == L'V' || c == L'X' || c == L'Z'
                || c == L'Y' || c == L'S' || c == L'F' || c == L'P' || c == L'N'
                || c == L'O' || c == L'W';
        }
        return key == L"HOME" || key == L"END" || key == L"TAB" || key == L"SPACE";
    }
    if (alt && !ctrl && !win) {
        return key == L"F4" || key == L"SPACE";
    }
    if (win && !ctrl && !alt) {
        return key == L"D" || key == L"DOWN" || key == L"R" || key == L"SPACE";
    }
    return false;
}

/// 组合键展示文本（如 Ctrl+H）
std::wstring ShortcutDisplayText(bool ctrl, bool alt, bool win, const std::wstring& key) {
    std::wstring out;
    if (ctrl) out += L"Ctrl+";
    if (alt) out += L"Alt+";
    if (win) out += L"Win+";
    out += key;
    return out;
}

AgentTool MakeKeyClickTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"keyClick";
    tool.description =
        L"点击按键。keyText 必填（Enter/Tab/Escape/F2/Down/Up/字母等）。"
        L"★打字禁止用 keyClick 逐字母敲——用 quickInput（宿主自动切英文 IME）；"
        L"单字母裸键（h/u/r 等）默认被拦（中文输入法下会被组字污染输入框），确认有用才放行。"
        L"组合键用 holdLeftCtrl/Alt/Shift/Win（布尔）；★只按你确信当前软件支持的通用快捷键"
        L"（编辑/保存/查找类），应用专属组合键（如浏览器 Ctrl+H 历史）不猜——不确定就视觉点击菜单。"
        L"重命名：F2 后接 quickInput(clearFirst=false)。"
        L"Win+D/最小化用 hotkeyShortcut(6/10)；切窗用 activateWindow，勿用 Alt+Tab。"
        L"ASCII 键/表格导航键前宿主自动切英文 IME；要主动切中英用 switchIme。"
        L"默认 observeAfter=false。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "keyText": {
                "type": "string",
                "minLength": 1,
                "description": "按键名：Enter / Tab / Escape / F2 / Down / Up / 字母数字等"
            },
            "keyVk": {
                "type": "integer",
                "description": "可选虚拟键码；一般只需 keyText"
            },
            "holdLeftCtrl": { "type": "boolean" },
            "holdLeftAlt": { "type": "boolean" },
            "holdLeftShift": { "type": "boolean" },
            "holdLeftWin": { "type": "boolean" },
            "confirmShortcut": {
                "type": "boolean",
                "description": "组合键/单字母非通用时默认被拦；确信当前软件支持才传 true"
            },
            "confirmBlindKey": {
                "type": "boolean",
                "description": "locate 失败后非白名单键默认被拦；确信需要才传 true"
            },
            "observeAfter": {
                "type": "boolean",
                "description": "true=执行后截屏；默认 false"
            }
        },
        "required": ["keyText"]
    })";
    tool.execute = [hooks, allowAbs](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();
        params["type"] = "keyClick";

        bool observeAfter = false;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);

        std::wstring keyText;
        if (params.contains("keyText") && params["keyText"].is_string())
            keyText = FromUtf8(params["keyText"].get<std::string>());
        for (auto& c : keyText) {
            if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        }
        // 单个 ASCII 字母/数字键：中文 IME 会把它们当拼音吃掉（如覆盖框按 Y 无反应），先切英文
        const bool isAsciiCharKey = (keyText.size() == 1)
            && ((keyText[0] >= L'A' && keyText[0] <= L'Z')
                || (keyText[0] >= L'0' && keyText[0] <= L'9'));
        if (isAsciiCharKey) ForceEnglishImeBeforeAsciiKey();

        // 组合键（Ctrl/Alt/Win+…）只允许确信当前软件支持的通用快捷键；
        // 应用专属组合键（Ctrl+H 等）默认劝退，加 confirmShortcut=true 才放行
        auto parBool = [&params](const char* k) {
            if (params.contains(k)) {
                const auto& v = params[k];
                if (v.is_boolean()) return v.get<bool>();
                if (v.is_number_integer()) return v.get<int>() != 0;
            }
            return false;
        };
        // 模型可能用 modifiers 数组（如 ["ctrl","alt"]）而不是 hold* 布尔字段，
        // 只查布尔会让 Ctrl+H 这种应用专属组合键绕过守卫。这里把数组并入 hold 标志。
        auto parModifiers = [&params](const char* name, bool& ctrl, bool& alt, bool& win) {
            if (!params.contains(name) || !params[name].is_array()) return;
            for (const auto& m : params[name]) {
                if (!m.is_string()) continue;
                const std::wstring s = FromUtf8(m.get<std::string>());
                if (s == L"ctrl") ctrl = true;
                else if (s == L"alt") alt = true;
                else if (s == L"win") win = true;
            }
        };
        bool holdCtl = parBool("holdLeftCtrl") || parBool("holdRightCtrl");
        bool holdAlt = parBool("holdLeftAlt") || parBool("holdRightAlt");
        bool holdWin = parBool("holdLeftWin") || parBool("holdRightWin");
        parModifiers("modifiers", holdCtl, holdAlt, holdWin);
        const bool confirmShortcut = parBool("confirmShortcut");
        const bool confirmBlindKey = parBool("confirmBlindKey");
        bool hasShift = parBool("holdLeftShift") || parBool("holdRightShift");
        {
            std::wstring t = keyText;
            bool stripped = false;
            for (;;) {
                const size_t plus = t.find(L'+');
                if (plus == std::wstring::npos) break;
                std::wstring tok = t.substr(0, plus);
                for (auto& c : tok) {
                    if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
                }
                if (tok == L"CTRL" || tok == L"CONTROL") holdCtl = true;
                else if (tok == L"ALT") holdAlt = true;
                else if (tok == L"SHIFT") hasShift = true;
                else if (tok == L"WIN" || tok == L"WINDOWS" || tok == L"META") holdWin = true;
                else break;
                t = t.substr(plus + 1);
                stripped = true;
            }
            if (stripped && t != keyText) {
                keyText = t;
                params["keyText"] = ToUtf8(t);
                if (holdCtl) params["holdLeftCtrl"] = true;
                if (holdAlt) params["holdLeftAlt"] = true;
                if (holdWin) params["holdLeftWin"] = true;
            }
        }
        if ((holdCtl || holdAlt || holdWin || hasShift)
            && !IsUniversalShortcutCombo(holdCtl, holdAlt, holdWin, keyText)
            && !confirmShortcut) {
            return L"[提示] 组合键 " + ShortcutDisplayText(holdCtl, holdAlt, holdWin, keyText)
                + L" 不是跨软件通用快捷键，不同软件定义不同。动手前先判断当前是什么软件、"
                  L"它是否支持这个快捷键；不确定就用 locateAndClick 点菜单按钮完成（勿按它碰运气）。"
                  L"若你确信当前软件支持该组合键，可重传并加 confirmShortcut=true 放行。";
        }
        // 单字母裸键（h/u/r…）：中文输入法下会被组字，且几乎总是误操作——
        // 想输入文字必须用 quickInput；想触发应用快捷键用 hotkeyShortcut 或确认过的组合键。
        if (keyText.size() == 1
            && keyText[0] >= L'A' && keyText[0] <= L'Z'
            && !holdCtl && !holdAlt && !holdWin && !hasShift
            && !confirmShortcut) {
            return L"[提示] 单字母键 keyClick(" + keyText
                + L") 几乎总是误操作（中文输入法下还会被组字成拼音，污染单元格/输入框）。"
                  L"想输入文字：用 quickInput（宿主自动切英文 IME）；想触发快捷键："
                  L"用 hotkeyShortcut 或确认软件支持的组合键。若你确信该软件里单按这个字母"
                  L"有明确作用，可重传并加 confirmShortcut=true 放行。";
        }
        if (const std::wstring guard = GuardKeysAfterLocateFail(keyText, confirmBlindKey,
                holdCtl || holdAlt || holdWin || hasShift, hooks); !guard.empty()) {
            return guard;
        }
        if (const std::wstring tabGuard = GuardBareTabStreak(keyText,
                holdCtl || holdAlt || holdWin || hasShift); !tabGuard.empty()) {
            return tabGuard;
        }

        const bool isF2 = (keyText == L"F2");
        // 非 F2 清掉「保留选区」提示，避免误伤后续搜索框输入
        if (!isF2) g_keepInputSelectionAfterF2 = false;
        // Win+D / Win+Down 绕过 hotkeyShortcut 预设也要走同一套藏窗预算
        if (holdWin && (keyText == L"D" || keyText == L"DOWN")) {
            const std::wstring what = (keyText == L"D") ? L"Win+D 显示桌面" : L"Win+Down 最小化";
            if (const std::wstring guard = GuardHideWindowsAction(what); !guard.empty())
                return guard;
        }
        if (keyText == L"ESCAPE") {
            if (const std::wstring guard = GuardEscapeInSaveDialog(hooks); !guard.empty())
                return guard;
        }
        // Midscene：提交钮已灰时 Enter 无效——直接打断并强制观察，勿同轮空转
        if (!holdCtl && !holdAlt && !holdWin && !hasShift
            && (keyText == L"ENTER" || keyText == L"RETURN")
            && AiSession().submitUiBlocked) {
            return std::wstring(kExecutedObserveMarker)
                + L"\n[事实] 提交钮不可用，Enter 未执行。看图改输入或点其它可见控件，勿空按回车。";
        }
        // Enter/Escape 后若对话框已关，清 pending
        if (keyText == L"ENTER" || keyText == L"RETURN" || keyText == L"ESCAPE") {
            // 先执行，再按结果清（见下方）
        }
        params.erase("confirmShortcut");
        params.erase("confirmBlindKey");

        json wrap = json::object();
        wrap["actions"] = json::array({ params });
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        const std::wstring out = FinishExecuteActions(hooks, merged, observeAfter);
        if (out.rfind(L"[错误]", 0) != 0) {
            // F4/Ctrl+Home 等导航进度可解除硬拦；纯 Enter 仍保留（否则 F12 测试链会被 Enter 洗掉）
            std::wstring k = keyText;
            for (auto& c : k) {
                if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
            }
            if (k == L"F4" || k == L"HOME" || (holdCtl && k == L"HOME")) {
                AiClearLocateFailKeyBlock();
            }
        }
        if ((keyText == L"ENTER" || keyText == L"RETURN" || keyText == L"ESCAPE")
            && out.rfind(L"[错误]", 0) != 0) {
            const std::wstring kind = ProbeForegroundDialogKind(hooks, nullptr);
            // 仍开着另存为/迷你保存：保留 pending；已关掉才清
            if (kind != L"saveAs" && kind != L"saveAsMini" && kind != L"openFile"
                && kind != L"confirmOverwrite") {
                AiSession().pendingSavePath.clear();
                AiSession().submitUiBlocked = false;
            }
            if (keyText == L"ESCAPE") {
                AiSession().pendingSavePath.clear();
                AiSession().submitUiBlocked = false;
            }
        }
        if (isF2 && out.rfind(L"[错误]", 0) != 0)
            g_keepInputSelectionAfterF2 = true;
        return out;
    };
    return tool;
}

AgentTool MakeMouseClickTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"mouseClick";
    tool.description =
        L"在当前 upload 截图像素坐标 (x,y) 点击。须落在截图宽高内；越界会被拒绝。"
        L"按钮/图标/菜单请用 locateAndClick，禁止猜坐标。输入框已聚焦时直接 quickInput。"
        L"★★前台是 Excel/WPS 等表格时禁止用本工具点网格单元格（空白格边线高度重复=随机点行）；"
        L"定位 A1 用 keyClick(Ctrl+Home)；点表头字母/行号等特征用 locateAndClick；录入用 Tab/Enter/Home。"
        L"打开桌面图标/文件用 locateAndClick(doubleClick=true) 或 openFile，单击打不开。"
        L"无截图时禁止调用。默认 observeAfter=false。";
    tool.parameters_json = LR"({
            "type": "object",
            "properties": {
                "x": { "type": "number", "description": "upload 图像素 X（0~宽-1）" },
                "y": { "type": "number", "description": "upload 图像素 Y（0~高-1）" },
                "button": {
                    "type": "string",
                    "enum": ["left", "right", "middle"],
                    "description": "默认 left"
                },
                "confirmSpreadsheetClick": {
                    "type": "boolean",
                    "description": "前台是表格软件时须 true 才放行（仅点功能区/非网格控件）"
                },
                "observeAfter": { "type": "boolean" }
            },
            "required": ["x", "y"]
        })";
    tool.execute = [hooks, allowAbs](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();
        const bool confirmSheet = params.contains("confirmSpreadsheetClick")
            && params["confirmSpreadsheetClick"].is_boolean()
            && params["confirmSpreadsheetClick"].get<bool>();
        if (ForegroundIsSpreadsheetApp() && !confirmSheet) {
            return L"[错误] 前台是表格软件，禁止 mouseClick 猜格子坐标（网格边线重复，极易点到错误行/"
                L"中间空行）。定位起点：keyClick(Ctrl+Home) 到 A1；需要视觉点选时 locateAndClick "
                L"「列标A」/「行号1」等有文字特征的目标，勿点空白单元格。"
                L"表头写完 Enter+Home 后直接填数据，禁止再 Down。确需点功能区非网格控件可加 "
                L"confirmSpreadsheetClick=true。";
        }
        params["type"] = "mouseClick";
        bool observeAfter = false;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);
        json wrap = json::object();
        wrap["actions"] = json::array({ params });
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        return FinishExecuteActions(hooks, merged, observeAfter);
    };
    return tool;
}

AgentTool MakeMoveMouseTool(AiActionHostHooks* hooks, bool allowAbs) {
    return MakeAtomicMacroTool(
        L"moveMouse",
        L"移动鼠标到 upload 截图像素 (x,y)。须落在图内。按钮请用 locateAndClick。无截图时禁止。",
        LR"({
            "type": "object",
            "properties": {
                "x": { "type": "number" },
                "y": { "type": "number" },
                "observeAfter": { "type": "boolean" }
            },
            "required": ["x", "y"]
        })",
        hooks, allowAbs, false);
}

/// 拖拽：按下→移动→松开。滚动条滑块、文件拖放、选区等都用这个（滚轮滚不动的面板优先试）。
AgentTool MakeMouseDragTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"mouseDrag";
    tool.description =
        L"鼠标拖拽：从 (fromX,fromY) 按住左键拖到 (toX,toY) 再松开。"
        L"★★滚动条：先看图定位滑块中心为起点，沿轨道方向拖一段（不要用 scrollWheel 空转找侧栏项）。"
        L"拖文件/选区同理。坐标为 upload 截图像素（与 moveMouse 相同）。"
        L"可选 steps=中间插值点数（默认 6）。button 默认 left。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "fromX": { "type": "number" },
            "fromY": { "type": "number" },
            "toX": { "type": "number" },
            "toY": { "type": "number" },
            "steps": {
                "type": "integer",
                "minimum": 1,
                "maximum": 40,
                "description": "中间插值移动次数，默认 6"
            },
            "button": {
                "type": "string",
                "enum": ["left", "right", "middle"],
                "description": "默认 left"
            },
            "observeAfter": { "type": "boolean" }
        },
        "required": ["fromX", "fromY", "toX", "toY"]
    })";
    tool.execute = [hooks, allowAbs](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();
        if (!params.contains("fromX") || !params.contains("fromY")
            || !params.contains("toX") || !params.contains("toY")) {
            return L"[错误] mouseDrag 需要 fromX/fromY/toX/toY。";
        }
        auto num = [](const json& j, const char* k, double* out) -> bool {
            if (!j.contains(k)) return false;
            const auto& v = j[k];
            if (v.is_number()) { *out = v.get<double>(); return true; }
            if (v.is_number_integer()) { *out = static_cast<double>(v.get<int>()); return true; }
            return false;
        };
        double fx = 0, fy = 0, tx = 0, ty = 0;
        if (!num(params, "fromX", &fx) || !num(params, "fromY", &fy)
            || !num(params, "toX", &tx) || !num(params, "toY", &ty)) {
            return L"[错误] mouseDrag 坐标须为数字。";
        }
        int steps = 6;
        if (params.contains("steps") && params["steps"].is_number_integer())
            steps = std::clamp(params["steps"].get<int>(), 1, 40);
        std::string button = "left";
        if (params.contains("button") && params["button"].is_string())
            button = params["button"].get<std::string>();
        bool observeAfter = true;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);

        json actions = json::array();
        auto pushMove = [&](double x, double y) {
            json mv = json::object();
            mv["type"] = "moveMouse";
            mv["x"] = x;
            mv["y"] = y;
            actions.push_back(std::move(mv));
        };
        pushMove(fx, fy);
        {
            json down = json::object();
            down["type"] = "mouseDown";
            down["button"] = button;
            actions.push_back(std::move(down));
        }
        for (int i = 1; i <= steps; ++i) {
            const double t = static_cast<double>(i) / static_cast<double>(steps);
            pushMove(fx + (tx - fx) * t, fy + (ty - fy) * t);
        }
        {
            json up = json::object();
            up["type"] = "mouseUp";
            up["button"] = button;
            actions.push_back(std::move(up));
        }
        json wrap = json::object();
        wrap["actions"] = std::move(actions);
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        std::wstring result = FinishExecuteActions(hooks, merged, observeAfter);
        if (result.rfind(L"[错误]", 0) != 0)
            AppendAiTaskMemoLine(L"did: mouseDrag");
        return result;
    };
    return tool;
}

AgentTool MakeWaitTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"wait";
    tool.description =
        L"等待 duration 秒（只用于等长时间加载）。duration 必填。"
        L"★点击/回车/快捷键/启动程序之后宿主已自动等界面反应并稳定，禁止再补 wait —— "
        L"每个 wait 都要多烧一整轮 API。只有画面明确还在转圈/白屏时才 wait≤2s。"
        L"同一处连着等两次还没变化：别再 wait，改去看当前画面或换做法。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "duration": { "type": "number", "description": "等待秒数（必填，建议≤3）" },
            "randomDuration": { "type": "number" }
        },
        "required": ["duration"]
    })";
    tool.execute = [hooks, allowAbs](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();
        params["type"] = "wait";
        // 短 wait 几乎总是多余：宿主点击/回车/启动后已 settle；另存为里更禁止空等
        double dur = 0.0;
        if (params.contains("duration") && params["duration"].is_number())
            dur = params["duration"].get<double>();
        else if (params.contains("duration") && params["duration"].is_number_integer())
            dur = static_cast<double>(params["duration"].get<int>());
        bool confirmWait = false;
        if (params.contains("confirmWait")) {
            const auto& v = params["confirmWait"];
            if (v.is_boolean()) confirmWait = v.get<bool>();
            else if (v.is_number_integer()) confirmWait = v.get<int>() != 0;
        }
        params.erase("confirmWait");
        if (dur <= 2.0 && !confirmWait) {
            return L"[错误] 宿主已自动等界面稳定，勿再 wait≤2s（多烧一轮 API）。"
                L"画面仍在转圈/白屏时加 confirmWait=true。";
        }
        params.erase("observeAfter");
        json wrap = json::object();
        wrap["actions"] = json::array({ params });
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        return FinishExecuteActions(hooks, merged, true);
    };
    return tool;
}

/// 语义名 → shortcutPreset；优先用 name，避免 3=保存/4=查找 数字记混
int HotkeyNameToPreset(const std::wstring& nameIn) {
    std::wstring n = nameIn;
    for (auto& c : n) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    if (n == L"copy" || n == L"ctrl+c") return 0;
    if (n == L"paste" || n == L"ctrl+v") return 1;
    if (n == L"cut" || n == L"ctrl+x") return 2;
    if (n == L"save" || n == L"ctrl+s") return 3;
    if (n == L"find" || n == L"search" || n == L"ctrl+f") return 4;
    if (n == L"close" || n == L"alt+f4") return 5;
    if (n == L"showdesktop" || n == L"win+d") return 6;
    if (n == L"run" || n == L"win+r") return 7;
    if (n == L"ctrl+alt+del" || n == L"cad") return 8;
    if (n == L"minimize" || n == L"win+down") return 10;
    if (n == L"maximize" || n == L"win+up") return 11;
    return -1;
}

AgentTool MakeHotkeyShortcutTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"hotkeyShortcut";
    tool.description =
        L"系统快捷键。★优先传 name 语义名，勿死记数字（3=保存 与 4=查找极易按错）："
        L"name=save|copy|paste|cut|find|close|showDesktop|run|minimize|maximize。"
        L"也可用 shortcutPreset：0=Ctrl+C 1=Ctrl+V 2=Ctrl+X 3=Ctrl+S(保存) 4=Ctrl+F(查找) "
        L"5=Alt+F4 6=Win+D 7=Win+R 8=Ctrl+Alt+Del 9=禁用(改 activateWindow) "
        L"10=Win+Down 11=Win+Up。name 与 preset 同时给时以 name 为准。"
        L"★6/10 只藏窗不唤窗。唤窗用 activateWindow；不确定开着啥先 listWindows。"
        L"★新建文档/表格优先 runProgram(应用名)，勿 Win+D 再桌面右键（费截图）。"
        L"应用内置功能（浏览器历史/下载/设置等）一律 locateAndClick 点菜单按钮找入口，"
        L"勿用应用专有快捷键。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "name": {
                "type": "string",
                "description": "语义名：save/copy/paste/cut/find/close/showDesktop/run/minimize/maximize"
            },
            "shortcutPreset": {
                "type": "integer",
                "minimum": 0,
                "maximum": 11,
                "description": "0~11；与 name 二选一，优先 name；勿用 9"
            },
            "observeAfter": { "type": "boolean" }
        }
    })";
    tool.execute = [hooks, allowAbs](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();

        int preset = -1;
        if (params.contains("name") && params["name"].is_string()) {
            const std::wstring name = FromUtf8(params["name"].get<std::string>());
            preset = HotkeyNameToPreset(name);
            if (preset < 0) {
                return L"[错误] 未知 hotkeyShortcut.name「" + name
                    + L"」。可用：save/copy/paste/cut/find/close/showDesktop/run/"
                      L"minimize/maximize。应用内置功能（如浏览器历史）请用 locateAndClick"
                      L"点菜单按钮逐级找入口，勿用专有快捷键。";
            }
            params.erase("name");
            params["shortcutPreset"] = preset;
        } else if (params.contains("shortcutPreset") && params["shortcutPreset"].is_number_integer()) {
            preset = params["shortcutPreset"].get<int>();
        } else {
            return L"[错误] hotkeyShortcut 须提供 name（推荐，如 save）或 shortcutPreset。"
                L"保存用 name=\"save\"，查找用 name=\"find\"——勿把 3/4 记混。";
        }

        if (preset == 9) {
            return L"[错误] 禁止 hotkeyShortcut(9) 一按即松 Alt+Tab（不知道当前选中项，易在两窗间来回切）。"
                L"请用 activateWindow(match=\"Excel/Edge/标题一段\")；不确定开着啥先 listWindows。";
        }
        if (preset == 6 || preset == 10) {
            const std::wstring what = (preset == 6) ? L"Win+D 显示桌面" : L"Win+Down 最小化";
            if (const std::wstring guard = GuardHideWindowsAction(what); !guard.empty())
                return guard;
            if (preset == 6) {
                const std::wstring memo = ReadAiTaskMemoText();
                if ((memo.find(L"新建") != std::wstring::npos
                        || memo.find(L"创建") != std::wstring::npos)
                    && (memo.find(L"Excel") != std::wstring::npos
                        || memo.find(L"表格") != std::wstring::npos
                        || memo.find(L"文档") != std::wstring::npos
                        || memo.find(L"Word") != std::wstring::npos
                        || memo.find(L"PPT") != std::wstring::npos)) {
                    return L"[错误] 新建办公文档请用 runProgram(应用名)，勿 Win+D + 桌面右键（多轮 locate 费 token）。";
                }
            }
        }
        if (AiLocateFailKeyBlockActive()) {
            return L"[错误] 上轮 locate 失败：禁止 hotkeyShortcut 碰运气。"
                L"请换短标签再 locateAndClick，或 completeTask；勿用 F 键/Ctrl+S 乱试。";
        }
        params["type"] = "hotkeyShortcut";
        bool observeAfter = false;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);
        json wrap = json::object();
        wrap["actions"] = json::array({ params });
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        return FinishExecuteActions(hooks, merged, observeAfter);
    };
    return tool;
}

AgentTool MakeSwitchWindowTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"switchWindow";
    tool.description =
        L"【兜底】Alt+Tab 预览切窗。★首选 activateWindow（本地激活，一步到位不烧图）；"
        L"本工具只在 activateWindow 反复失败时用，要 force=true。"
        L"★刚 runProgram/openWebpage 启动的窗口会自动前台，禁止再切窗（白烧 3 轮截图）。"
        L"流程：openPreview（Alt按住+Tab一次，保持Alt，截屏看预览）→"
        L"按蓝框与目标窗的格数差 move(steps,direction,confirm=true) 一步收尾。"
        L"★不松开 Alt 不会真的切过去；confirm=true 就是移完立刻松手，省一轮也避免预览挡屏。"
        L"找不到：move 多翻几格再看；仍无则 cancel，改 activateWindow(match=标题关键词)。"
        L"Alt 悬 4 轮或改做别的动作时，宿主会强制松开落地。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["openPreview", "move", "confirm", "cancel"],
                "description": "openPreview=唤出预览并保持Alt；move=在预览中移动选中；confirm=松开Alt激活；cancel=取消"
            },
            "steps": {
                "type": "integer",
                "minimum": 1,
                "maximum": 40,
                "description": "move 时按键次数（序号差），默认 1"
            },
            "direction": {
                "type": "string",
                "enum": ["right", "left", "tab"],
                "description": "move 方向：right=→（默认） left=← tab=再按Tab"
            },
            "confirm": {
                "type": "boolean",
                "description": "move 时 true=移完立刻松开 Alt 落地（推荐，省一轮；不松开不会真的切过去）"
            },
            "force": {
                "type": "boolean",
                "description": "activateWindow 已失败、确实只能靠 Alt+Tab 时才传 true"
            },
            "observeAfter": {
                "type": "boolean",
                "description": "openPreview/move 默认 true；confirm/cancel 默认 true"
            }
        },
        "required": ["action"]
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        const bool opensPreview = paramsJson.find(L"openPreview") != std::wstring::npos;
        const bool forced = paramsJson.find(L"\"force\"") != std::wstring::npos
            && paramsJson.find(L"true") != std::wstring::npos;
        // 刚 runProgram 启动的窗口会自己到前台；此时开预览要烧 3 轮带图，先劝一次
        if (opensPreview && !forced
            && AiSession().lastLaunchTick != 0 && !AiSession().launchSwitchWarned
            && GetTickCount64() - AiSession().lastLaunchTick < 8000) {
            AiSession().launchSwitchWarned = true;
            return L"[错误] 刚启动的程序会自动到前台，不需要切窗。"
                L"请先看当前画面继续操作；程序还没起来就 wait 1~2 秒后同轮继续。";
        }
        // Alt+Tab 预览要 3 轮带图还常切错；有本地台账就直接激活
        if (opensPreview && !forced && hooks && hooks->onActivateWindow) {
            return WithWindowLedger(hooks,
                L"[错误] 别用 Alt+Tab 预览切窗：要多烧 3 轮截图，还会因为数错格数切到别的窗口。"
                L"改用 activateWindow(match=\"标题或进程名的一段\")，宿主本地一步切过去。")
                + L"\n（activateWindow 连续失败时才用 switchWindow(action=openPreview, force=true) 兜底）";
        }
        if (!hooks || !hooks->onSwitchWindow) {
            return L"[错误] switchWindow 需要宿主钩子 onSwitchWindow。";
        }
        return hooks->onSwitchWindow(paramsJson);
    };
    return tool;
}

AgentTool MakeListWindowsTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"listWindows";
    tool.description =
        L"列出当前所有可切换窗口（本地枚举，纯文本不烧截图）。按 Z 序：越靠前越近期用过。"
        L"用途：确认某程序/文档是不是已经开着（开着就别重复新建），或拿到 activateWindow 用的关键词。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "match": {
                "type": "string",
                "description": "可选：只列标题或进程名含此子串的窗口（不分大小写）"
            }
        }
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!hooks || !hooks->onListWindows) {
            return L"[错误] listWindows 需要宿主钩子 onListWindows。";
        }
        std::wstring match;
        if (params.is_object() && params.contains("match") && params["match"].is_string())
            match = FromUtf8(params["match"].get<std::string>());
        const std::wstring list = hooks->onListWindows();
        if (match.empty()) {
            return L"当前窗口（Z 序，#1 最靠前）：\n" + list;
        }
        // 本地再滤一次（钩子返回全量台账）；避免模型拿着全表却以为 match 已生效
        std::wstring filtered;
        std::wstring line;
        const std::wstring needle = match;
        auto toLower = [](std::wstring s) {
            for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
            return s;
        };
        const std::wstring needleL = toLower(needle);
        int kept = 0;
        for (size_t i = 0; i <= list.size(); ++i) {
            if (i < list.size() && list[i] != L'\n') {
                line.push_back(list[i]);
                continue;
            }
            if (!line.empty() && toLower(line).find(needleL) != std::wstring::npos) {
                if (!filtered.empty()) filtered += L'\n';
                filtered += line;
                ++kept;
            }
            line.clear();
        }
        if (kept == 0) {
            return L"没有标题/进程名包含「" + match + L"」的窗口。\n全量台账：\n" + list
                + L"\n（切窗请用 activateWindow；确实没开就 runProgram；"
                  L"找不到路径用 openAppViaSearch）";
        }
        return L"匹配「" + match + L"」的窗口（" + std::to_wstring(kept) + L" 个）：\n"
            + filtered
            + L"\n（切过去请用 activateWindow(match=\"" + match + L"\")；写得更具体可消歧）";
    };
    return tool;
}

AgentTool MakeActivateWindowTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"activateWindow";
    tool.description =
        L"★切窗首选：按标题/进程名子串把已打开的窗口切到前台。宿主本地激活，"
        L"不识图、不 Alt+Tab、不点任务栏。最小化会自动还原。"
        L"★多候选时拒绝自动切换并列出清单——match 必须写到能唯一锁定标题关键词"
        L"（如「历史记录」「浏览记录」），禁止只写 edge/excel/msedge。"
        L"默认 observeAfter=false（跨窗抄录不烧图）；需要验收时再传 true。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "match": {
                "type": "string",
                "minLength": 1,
                "description": "标题关键词优先，如 浏览记录 / 历史记录；避免仅进程名"
            },
            "observeAfter": {
                "type": "boolean",
                "description": "默认 false；true=切窗后截屏验收"
            }
        },
        "required": ["match"]
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();
        std::wstring match;
        if (params.contains("match") && params["match"].is_string())
            match = Trim(FromUtf8(params["match"].get<std::string>()));
        if (match.empty()) return L"[错误] activateWindow 的 match 不能为空。";
        if (!hooks || !hooks->onActivateWindow) {
            return L"[错误] activateWindow 需要宿主钩子 onActivateWindow。";
        }
        bool observeAfter = false;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);
        const std::wstring msg = hooks->onActivateWindow(match);
        if (msg.rfind(L"[错误]", 0) == 0) return msg;
        ++AiSession().actionSeq;
        UpsertAiTaskMemoSection(L"windows", match, true);
        const wchar_t* marker = observeAfter ? kExecutedObserveMarker : kExecutedSkipObserveMarker;
        return std::wstring(marker) + L"\n" + msg;
    };
    return tool;
}

/// 窗口台账文本里是否已出现某进程（不烧图）
bool WindowLedgerMentionsProcess(const std::wstring& ledger, const std::wstring& processNeedle) {
    if (ledger.empty() || processNeedle.empty()) return false;
    std::wstring hay = ledger;
    std::wstring needle = processNeedle;
    for (auto& c : hay) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    for (auto& c : needle) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    // 台账格式：… [msedge.exe]
    if (hay.find(L"[" + needle + L"]") != std::wstring::npos) return true;
    return hay.find(needle) != std::wstring::npos;
}

/// 开始菜单搜索用的友好显示名（不知道 exe 路径时用这个搜）
std::wstring NormalizeAppSearchQuery(const std::wstring& raw) {
    std::wstring q = Trim(raw);
    if (q.empty()) return q;
    std::wstring lower = q;
    for (auto& c : lower) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    if (lower.size() > 4 && lower.rfind(L".exe") == lower.size() - 4)
        lower = lower.substr(0, lower.size() - 4);
    if (lower == L"edge" || lower == L"msedge" || lower == L"microsoft edge")
        return L"Microsoft Edge";
    if (lower == L"chrome" || lower == L"google chrome") return L"Google Chrome";
    if (lower == L"firefox" || lower == L"mozilla firefox") return L"Firefox";
    if (lower == L"excel" || lower == L"microsoft excel") return L"Excel";
    if (lower == L"word" || lower == L"winword" || lower == L"microsoft word") return L"Word";
    if (lower == L"powerpoint" || lower == L"powerpnt" || lower == L"microsoft powerpoint")
        return L"PowerPoint";
    if (lower == L"outlook" || lower == L"microsoft outlook") return L"Outlook";
    if (lower == L"notepad") return L"记事本";
    if (lower == L"calc" || lower == L"calculator") return L"计算器";
    if (lower == L"explorer" || lower == L"file explorer") return L"文件资源管理器";
    if (lower == L"cmd" || lower == L"command prompt") return L"命令提示符";
    if (lower == L"powershell" || lower == L"pwsh") return L"Windows PowerShell";
    if (lower == L"settings" || lower == L"ms-settings") return L"设置";
    // 保留用户给的中文/品牌显示名
    if (q.size() > 4) {
        std::wstring qLower = q;
        for (auto& c : qLower) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        if (qLower.rfind(L".exe") == qLower.size() - 4)
            q = q.substr(0, q.size() - 4);
    }
    return q;
}

/// 台账里是否像已打开该应用（标题/进程子串）
bool WindowLedgerLooksLikeApp(const std::wstring& ledger, const std::wstring& query) {
    if (ledger.empty() || query.empty()) return false;
    std::wstring hay = ledger;
    for (auto& c : hay) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    auto has = [&](const std::wstring& needle) {
        if (needle.empty()) return false;
        std::wstring n = needle;
        for (auto& c : n) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        return hay.find(n) != std::wstring::npos;
    };
    if (has(query)) return true;
    const std::wstring norm = NormalizeAppSearchQuery(query);
    if (norm != query && has(norm)) return true;
    std::wstring lower = query;
    for (auto& c : lower) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    if (lower.find(L"edge") != std::wstring::npos && has(L"msedge")) return true;
    if (lower.find(L"excel") != std::wstring::npos && has(L"excel")) return true;
    if (lower.find(L"chrome") != std::wstring::npos && has(L"chrome")) return true;
    if (lower.find(L"word") != std::wstring::npos && has(L"winword")) return true;
    return false;
}

AgentTool MakeRunProgramTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool = MakeAtomicMacroTool(
        L"runProgram",
        L"启动程序。targetPath=exe/完整路径，或常见名 excel/notepad/edge/chrome。"
        L"★宿主会把 edge/msedge 解析成 msedge.exe 真实路径，禁止靠「edge」裸名乱启动（会弹商店）。"
        L"★浏览器类（Edge/Chrome/Firefox）：若已有实例在跑，默认拒绝再启动——"
        L"请先 listWindows→activateWindow 复用；确需新开传 forceNew=true。"
        L"★找不到路径时勿瞎猜：改用 openAppViaSearch(query=应用显示名) 走开始菜单搜索（慢但稳）。"
        L"读历史/抄网页数据：优先复用已开浏览器，与是否新开标签无关。"
        L"Office 新建文档：targetPath=excel 等（默认允许新开）。",
        LR"({
            "type": "object",
            "properties": {
                "targetPath": {
                    "type": "string",
                    "minLength": 1,
                    "description": "程序路径或可解析名：excel / edge / notepad / 完整路径"
                },
                "inputText": {
                    "type": "string",
                    "description": "可选命令行参数"
                },
                "forceNew": {
                    "type": "boolean",
                    "description": "true=即使已有同进程窗口也强制新开；浏览器默认 false（复用）"
                },
                "observeAfter": { "type": "boolean" }
            },
            "required": ["targetPath"]
        })",
        hooks, allowAbs, true);
    auto inner = tool.execute;
    tool.execute = [inner, hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();

        std::wstring target;
        if (params.contains("targetPath") && params["targetPath"].is_string())
            target = Trim(FromUtf8(params["targetPath"].get<std::string>()));
        if (target.empty()) return L"[错误] runProgram 需要 targetPath。";

        const std::wstring resolved = ResolveProgramLaunchPath(target);
        if (resolved.empty()) {
            return L"[错误] 无法解析程序「" + target
                + L"」。请改用 openAppViaSearch(query=应用显示名) 走开始菜单搜索兜底。";
        }
        params["targetPath"] = ToUtf8(resolved);

        bool forceNew = false;
        if (params.contains("forceNew")) {
            const auto& f = params["forceNew"];
            if (f.is_boolean()) forceNew = f.get<bool>();
            else if (f.is_number_integer()) forceNew = f.get<int>() != 0;
        }
        params.erase("forceNew");

        // 浏览器：已开则拒启动，逼模型走 activateWindow（抽象复用，不限 Edge）
        if (!forceNew && IsBrowserLaunchTarget(target)) {
            const std::wstring list = WindowLedgerText(hooks);
            std::wstring proc = resolved;
            const auto slash = proc.find_last_of(L"\\/");
            if (slash != std::wstring::npos) proc = proc.substr(slash + 1);
            if (WindowLedgerMentionsProcess(list, proc)) {
                return L"[错误] 已检测到浏览器进程「" + proc
                    + L"」在运行。读历史/抄网页请 activateWindow(match=标题关键词) 复用已开窗口，"
                      L"不要再 runProgram（裸名启动还会弹「未知应用/商店」）。"
                      L"确需新开实例：forceNew=true。\n当前窗口：\n"
                    + (list.empty() ? L"（台账为空，请先 listWindows）" : list);
            }
        }

        // 解析后路径仍不像存在的文件，且不是带协议的 URL——提前拒，避免弹商店
        if (resolved.find(L"://") == std::wstring::npos
            && GetFileAttributesW(resolved.c_str()) == INVALID_FILE_ATTRIBUTES
            && resolved.find(L'\\') == std::wstring::npos) {
            const std::wstring q = NormalizeAppSearchQuery(target);
            return L"[错误] 找不到程序「" + target + L"」(解析为「" + resolved
                + L"」)。请改用 openAppViaSearch(query=\"" + q
                + L"\") 走开始菜单搜索兜底；或 listWindows 后 activateWindow 复用已开窗口。";
        }

        const std::wstring rewritten = FromUtf8(params.dump());
        std::wstring out = inner ? inner(rewritten) : L"[错误] runProgram 未初始化。";
        if (out.rfind(L"[错误]", 0) != 0 && !LastLaunchProgramError().empty()) {
            // 宿主偶发未透传失败时，仍按真实 ShellExecute 结果纠偏
            out = L"[错误] " + LastLaunchProgramError();
        }
        if (out.rfind(L"[错误]", 0) != 0) {
            AiSession().lastLaunchTick = GetTickCount64();
            AiSession().launchSwitchWarned = false;
            // 从解析路径取进程名，宿主按进程自动置顶（勿依赖 AI 再调 activateWindow(excel)——会多候选拒）
            std::wstring procName = resolved;
            {
                const auto slash = procName.find_last_of(L"\\/");
                if (slash != std::wstring::npos) procName = procName.substr(slash + 1);
            }
            if (procName.find(L'.') == std::wstring::npos) {
                // 别名 excel → EXCEL.EXE
                std::wstring alias = procName;
                for (auto& c : alias) {
                    if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
                }
                if (alias == L"EXCEL") procName = L"EXCEL.EXE";
                else if (alias == L"EDGE" || alias == L"MSEDGE") procName = L"MSEDGE.EXE";
                else if (alias == L"NOTEPAD") procName = L"NOTEPAD.EXE";
                else if (alias == L"WINWORD") procName = L"WINWORD.EXE";
                else if (alias == L"POWERPNT") procName = L"POWERPNT.EXE";
                else procName += L".EXE";
            }
            if (hooks && hooks->onActivateByProcess && !procName.empty()) {
                const std::wstring act = hooks->onActivateByProcess(procName);
                if (act.rfind(L"[错误]", 0) != 0) {
                    out += L"\n" + act;
                } else {
                    out += L"\n（自动置顶未成功：" + act
                        + L" — 请随后 activateWindow 用唯一标题关键词）";
                }
            }
            const std::wstring list = WindowLedgerText(hooks);
            if (resolved != target) {
                out += L"\n已解析启动路径：" + resolved;
            }
            if (!list.empty()) {
                out += L"\n当前窗口（Z 序，#1 最靠前）：\n" + list
                    + L"\n切到其中任意一个用 activateWindow(match=...)；已经开着的东西别重复新建。";
            }
        } else if (out.find(L"openAppViaSearch") == std::wstring::npos) {
            const std::wstring q = NormalizeAppSearchQuery(target);
            out += L"\n兜底：openAppViaSearch(query=\"" + q + L"\") 开始菜单搜索打开。";
        }
        return out;
    };
    return tool;
}

AgentTool MakeOpenAppViaSearchTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"openAppViaSearch";
    tool.description =
        L"★★启动兜底：Win+S 打开系统搜索 → 输入应用显示名 → Enter 打开第一条结果。"
        L"不知道 exe 路径、runProgram 找不到文件/弹商店时用本工具（比直接启动慢，但几乎总能打开）。"
        L"query 用开始菜单里能搜到的名字（如「Microsoft Edge」「Excel」「微信」），勿传盘符路径。"
        L"★已开同类窗口时默认拒绝——先 activateWindow 复用；确需新开 forceNew=true。"
        L"若搜索也打不开，说明本机未安装该应用。默认 observeAfter=true 验收。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "query": {
                "type": "string",
                "minLength": 1,
                "description": "开始菜单搜索关键字：应用显示名，如 Microsoft Edge / Excel / 微信"
            },
            "forceNew": {
                "type": "boolean",
                "description": "true=即使台账已有同类窗口也仍走搜索新开；默认 false"
            },
            "observeAfter": {
                "type": "boolean",
                "description": "默认 true；打开后截屏确认是否起窗"
            }
        },
        "required": ["query"]
    })";
    tool.execute = [hooks, allowAbs](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();

        std::wstring query;
        if (params.contains("query") && params["query"].is_string())
            query = Trim(FromUtf8(params["query"].get<std::string>()));
        if (query.empty()) return L"[错误] openAppViaSearch 需要 query（应用显示名）。";
        if (query.find(L':') != std::wstring::npos && query.find(L'\\') != std::wstring::npos) {
            return L"[错误] openAppViaSearch 的 query 应是显示名而非路径。"
                L"有路径请用 runProgram/openFile；无路径才用本工具。";
        }

        const std::wstring searchQuery = NormalizeAppSearchQuery(query);
        if (searchQuery.empty()) return L"[错误] openAppViaSearch 的 query 无效。";

        bool forceNew = false;
        if (params.contains("forceNew")) {
            const auto& f = params["forceNew"];
            if (f.is_boolean()) forceNew = f.get<bool>();
            else if (f.is_number_integer()) forceNew = f.get<int>() != 0;
        }

        if (!forceNew) {
            const std::wstring list = WindowLedgerText(hooks);
            if (WindowLedgerLooksLikeApp(list, searchQuery)
                || WindowLedgerLooksLikeApp(list, query)) {
                return WithWindowLedger(hooks,
                    L"[错误] 台账里已有与「" + searchQuery
                    + L"」相关的窗口。请 activateWindow(match=标题关键词) 复用，"
                      L"勿再走开始菜单搜索（慢且可能新开实例）。"
                      L"确需新开：forceNew=true。");
            }
        }

        bool observeAfter = true;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);

        // Win+S → 等搜索框 → Ctrl+A 清旧词 → 输入显示名 → 稍等索引 → Enter 打开首条
        json actions = json::array();
        actions.push_back(json{
            {"type", "keyClick"},
            {"keyText", "S"},
            {"holdLeftWin", true}
        });
        actions.push_back(json{{"type", "wait"}, {"duration", 0.45}});
        actions.push_back(json{
            {"type", "keyClick"},
            {"keyText", "A"},
            {"holdLeftCtrl", true}
        });
        actions.push_back(json{
            {"type", "quickInput"},
            {"inputText", ToUtf8(searchQuery)}
        });
        actions.push_back(json{{"type", "wait"}, {"duration", 0.55}});
        actions.push_back(json{{"type", "keyClick"}, {"keyText", "Enter"}});

        json wrap = json::object();
        wrap["actions"] = actions;
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        std::wstring out = FinishExecuteActions(hooks, merged, observeAfter);
        if (out.rfind(L"[错误]", 0) != 0) {
            AiSession().lastLaunchTick = GetTickCount64();
            AiSession().launchSwitchWarned = false;
            out += L"\n已通过开始菜单搜索打开「" + searchQuery
                + L"」。若未起窗：确认本机已安装该应用，或换更贴近开始菜单的显示名再试一次；"
                  L"仍失败则该程序本机不可用。";
        }
        return out;
    };
    return tool;
}

AgentTool MakeOpenFileTool(AiActionHostHooks* hooks, bool allowAbs) {
    return MakeAtomicMacroTool(
        L"openFile",
        L"用系统默认程序打开文件/文件夹（ShellExecute）。targetPath 必填绝对或可解析路径。",
        LR"({
            "type": "object",
            "properties": {
                "targetPath": {
                    "type": "string",
                    "minLength": 1,
                    "description": "文件或文件夹路径"
                },
                "observeAfter": { "type": "boolean" }
            },
            "required": ["targetPath"]
        })",
        hooks, allowAbs, true);
}

AgentTool MakeFindImageTool(AiActionHostHooks* hooks, bool allowAbs) {
    return MakeAtomicMacroTool(
        L"findImage",
        L"屏幕找图。imagePath 必填。"
        L"imageUseVar=true 时 imagePath 为图片变量名（如 aiObs）或磁盘路径；"
        L"followUp: click|move|saveVar|saveImage(3=保存图片变量)。"
        L"宿主观察闭环会自动用 aiObs 变量判断界面是否变化；本工具用于显式找图/存图。",
        LR"({
            "type": "object",
            "properties": {
                "imagePath": {
                    "type": "string",
                    "minLength": 1,
                    "description": "模板图路径，或 imageUseVar 时的变量名"
                },
                "imageUseVar": {
                    "type": "boolean",
                    "description": "true=imagePath 为图片变量名/路径"
                },
                "matchThreshold": {
                    "type": "number",
                    "description": "匹配阈值 1~100，默认 65"
                },
                "followUp": {
                    "type": "string",
                    "description": "click|move|saveVar|saveImage"
                },
                "findImageFollowUp": {
                    "type": "integer",
                    "description": "0点击 1移动 2保存匹配 3保存图片变量"
                },
                "matchVarName": {
                    "type": "string",
                    "description": "保存变量名；followUp=saveImage 时为图片变量名"
                },
                "searchFullScreen": { "type": "boolean" },
                "searchX1": { "type": "number" },
                "searchY1": { "type": "number" },
                "searchX2": { "type": "number" },
                "searchY2": { "type": "number" },
                "observeAfter": { "type": "boolean" }
            },
            "required": ["imagePath"]
        })",
        hooks, allowAbs, false);
}

AgentTool MakeScrollWheelTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"scrollWheel";
    tool.description =
        L"滚动鼠标滚轮以露出更多内容。scrollDirection: 0/up/left 或 1/down/right；"
        L"scrollSteps 默认 3。前台滚轮作用在当前光标下窗口——先确保光标在可滚区域内"
        L"（可传 x/y 先移标，或先 locateAndClick 页面空白）。"
        L"目标不在可视区时优先本工具或 keyClick(PageDown/Space)，禁止反复 openWebpage。"
        L"默认 observeAfter=true。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "scrollDirection": {
                "description": "0|up|left 向上/左；1|down|right 向下/右",
                "oneOf": [
                    { "type": "integer", "minimum": 0, "maximum": 1 },
                    { "type": "string" }
                ]
            },
            "scrollSteps": {
                "type": "integer",
                "minimum": 1,
                "maximum": 20,
                "description": "滚轮步数，默认 3"
            },
            "scrollVertical": { "type": "boolean", "description": "默认 true" },
            "scrollHorizontal": { "type": "boolean", "description": "默认 false" },
            "x": { "type": "number", "description": "可选：滚动前移到此截图/屏幕 X" },
            "y": { "type": "number", "description": "可选：滚动前移到此截图/屏幕 Y" },
            "observeAfter": { "type": "boolean" }
        }
    })";
    tool.execute = [hooks, allowAbs](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();
        params["type"] = "scrollWheel";
        if (!params.contains("scrollSteps")) params["scrollSteps"] = 3;
        if (!params.contains("scrollDirection")) params["scrollDirection"] = 1;
        bool observeAfter = true;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);

        json actions = json::array();
        if (params.contains("x") && params.contains("y")
            && params["x"].is_number() && params["y"].is_number()) {
            json mv = json::object();
            mv["type"] = "moveMouse";
            mv["x"] = params["x"];
            mv["y"] = params["y"];
            mv["coordSpace"] = "screen";
            actions.push_back(std::move(mv));
        }
        json wheel = params;
        wheel.erase("observeAfter");
        actions.push_back(std::move(wheel));
        json wrap = json::object();
        wrap["actions"] = std::move(actions);
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        std::wstring result = FinishExecuteActions(hooks, merged, observeAfter);
        if (result.rfind(L"[错误]", 0) != 0)
            AppendAiTaskMemoLine(L"did: scrollWheel");
        const std::wstring dlg = ProbeForegroundDialogKind(hooks, nullptr);
        if (dlg == L"saveAs" || dlg == L"saveAsMini") {
            ++AiSession().saveAsScrollStreak;
            if (AiSession().saveAsScrollStreak >= 3 && result.rfind(L"[错误]", 0) != 0) {
                return L"[错误] 另存为里已连续 scrollWheel×"
                    + std::to_wstring(AiSession().saveAsScrollStreak)
                    + L"。禁止再空转滚动找「桌面」。"
                      L"正路：resolveSystemPath→点文件名框→quickInput(完整路径或纯文件名, clearFirst=true)"
                      L"→Enter/保存；或 F4 地址栏贴目录。滚动条滑块用 mouseDrag，勿滚轮空转。";
            }
            if (AiSession().saveAsScrollStreak >= 2 && result.rfind(L"[错误]", 0) != 0) {
                result += L"\n[事实] 已连续滚动×"
                    + std::to_wstring(AiSession().saveAsScrollStreak)
                    + L"。未见目标请改路径导航（resolveSystemPath / 文件名框完整路径），"
                      L"或 mouseDrag 拖侧栏滚动条；勿空转滚动。";
            }
        } else {
            AiSession().saveAsScrollStreak = 0;
        }
        return result;
    };
    return tool;
}

AgentTool MakeOpenWebpageTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"openWebpage";
    tool.description =
        L"用系统默认浏览器打开 URL（ShellExecute）。targetPath 必填完整 https://…。"
        L"宿主会本地做「画面有反应→再稳定」二次校验。若本任务已打开过相同 URL，默认跳过重开"
        L"（force=true 才强制）。白屏/骨架只 wait；目标不在可视区用 scrollWheel/PageDown，勿重复开站。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "targetPath": {
                "type": "string",
                "minLength": 1,
                "description": "完整 URL"
            },
            "force": {
                "type": "boolean",
                "description": "true=即使本任务已开过同 URL 也再开；默认 false"
            },
            "observeAfter": {
                "type": "boolean",
                "description": "默认 true：打开后截屏验收"
            }
        },
        "required": ["targetPath"]
    })";
    tool.execute = [hooks, allowAbs](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();
        std::wstring url;
        if (params.contains("targetPath") && params["targetPath"].is_string())
            url = FromUtf8(params["targetPath"].get<std::string>());
        url = Trim(url);
        if (url.empty()) return L"[错误] openWebpage 的 targetPath 不能为空。";

        bool force = false;
        if (params.contains("force")) {
            const auto& f = params["force"];
            if (f.is_boolean()) force = f.get<bool>();
            else if (f.is_number_integer()) force = f.get<int>() != 0;
        }
        const std::wstring& last = AiSession().lastOpenWebpageUrl;
        if (!force && !last.empty() && _wcsicmp(last.c_str(), url.c_str()) == 0) {
            AppendAiTaskMemoLine(L"skip: reopen " + url);
            return std::wstring(kExecutedObserveMarker)
                + L"\n已跳过重复 openWebpage（本任务已打开过该 URL）。"
                L"请观察当前页；内容不在可视区用 scrollWheel 或 keyClick(PageDown)；"
                L"确需刷新用 keyClick(F5) 或 force=true。";
        }

        params["type"] = "openWebpage";
        params.erase("force");
        bool observeAfter = true;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);
        json wrap = json::object();
        wrap["actions"] = json::array({ params });
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        const std::wstring result = FinishExecuteActions(hooks, merged, observeAfter);
        if (result.rfind(L"[错误]", 0) != 0) {
            AiSession().lastOpenWebpageUrl = url;
            AppendAiTaskMemoLine(L"done: openWebpage");
        }
        return result;
    };
    return tool;
}

AgentTool MakeUpdateTaskMemoTool() {
    AgentTool tool;
    tool.name = L"updateTaskMemo";
    tool.description =
        L"写入本任务结构化短备忘（本地缓存）。固定章节："
        L"goal=总目标；facts=已确认事实；todos=待办子目标；windows=窗口标题关键词；"
        L"recipe=已验证的键盘配方要点；notes=其它短记。"
        L"goal/windows/recipe 写入会覆盖该节；facts/todos/notes 默认追加。"
        L"也可在 note 里写「goal: …」或「[facts] …」。replace=true 清空全部再写。"
        L"每行尽量短；上下文变长时靠备忘对照。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "note": {
                "type": "string",
                "minLength": 1,
                "description": "短记正文，或带章节前缀如 goal:抄历史到Excel"
            },
            "section": {
                "type": "string",
                "enum": ["goal", "facts", "todos", "windows", "recipe", "notes"],
                "description": "固定章节；省略时从 note 前缀推断，默认 notes"
            },
            "replace": {
                "type": "boolean",
                "description": "true=清空全部备忘后只留本行；默认 false"
            }
        },
        "required": ["note"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        std::wstring note;
        if (params.contains("note") && params["note"].is_string())
            note = FromUtf8(params["note"].get<std::string>());
        note = NormalizeMemoLine(note);
        if (note.empty()) return L"[错误] note 不能为空。";
        bool replaceAll = false;
        if (params.contains("replace")) {
            const auto& r = params["replace"];
            if (r.is_boolean()) replaceAll = r.get<bool>();
            else if (r.is_number_integer()) replaceAll = r.get<int>() != 0;
        }
        std::wstring section;
        std::wstring body;
        if (params.contains("section") && params["section"].is_string()) {
            section = FromUtf8(params["section"].get<std::string>());
            for (auto& c : section) c = static_cast<wchar_t>(towlower(c));
            body = note;
            // note 若自带同节前缀，剥掉避免 [[goal]] 
            std::wstring parsedSec, parsedBody;
            if (ParseMemoSectionLine(note, parsedSec, parsedBody) && parsedSec == section)
                body = parsedBody;
        } else {
            ParseMemoSectionLine(note, section, body);
            if (body.empty()) body = note;
        }
        if (!IsMemoSectionName(section)) section = L"notes";
        if (body.empty()) return L"[错误] note 不能为空。";
        if (replaceAll) {
            if (!SaveMemoLines({ MemoSectionTag(section) + body }))
                return L"[错误] 备忘写入失败。";
            if (section == L"goal" || section == L"todos")
                AiSession().planUnlocked = true;
            return L"备忘已清空并写入 [" + section + L"]";
        }
        const bool replaceSec = MemoSectionIsSingleton(section);
        if (!UpsertAiTaskMemoSection(section, body, replaceSec))
            return L"[错误] 备忘写入失败。";
        return L"备忘已更新 [" + section + L"]（共 "
            + std::to_wstring(LoadMemoLines().size()) + L" 行）";
    };
    return tool;
}

AgentTool MakeReadTaskMemoTool() {
    AgentTool tool;
    tool.name = L"readTaskMemo";
    tool.description =
        L"读取本任务短备忘。仅当上下文可能遗忘或宿主未注入备忘时调用；"
        L"空备忘时不要每轮开头先调本工具。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {}
    })";
    tool.execute = [](const std::wstring&) -> std::wstring {
        const std::wstring text = ReadAiTaskMemoText();
        if (text.empty()) return L"（备忘为空）";
        return text;
    };
    return tool;
}

AgentTool MakeSaveTaskDataTool() {
    AgentTool tool;
    tool.name = L"saveTaskData";
    tool.description =
        L"把从屏幕上读到/识图得到的一批数据，写入本任务本地缓存（markdown 分块，写进磁盘文件）。"
        L"★长任务专用：数据源（历史记录/列表/表格）只要识图/阅读过一次，"
        L"就立刻用本工具把内容完整存下来，之后填到目标文件时 readTaskData 直接读。"
        L"★★缓存仅本轮有效：新开一轮 AI 动作执行时宿主会清空；"
        L"用户任务要求「打开 Edge/浏览器看历史」时，必须先打开源再 save，禁止臆造或沿用想象中的旧数据。"
        L"name 是数据块名（如 historyRecords、excelRows），同名写多次会追加；"
        L"replace=true 覆盖整块。内容建议一行一条：序号|标题|网址|时间。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "name": {
                "type": "string",
                "minLength": 1,
                "description": "数据块名，短标识（如 historyRecords）"
            },
            "content": {
                "type": "string",
                "minLength": 1,
                "description": "要缓存的数据正文，一行一条"
            },
            "replace": {
                "type": "boolean",
                "description": "true=覆盖整块；默认 false=追加到块尾"
            }
        },
        "required": ["name", "content"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        std::wstring name;
        if (params.contains("name") && params["name"].is_string())
            name = Trim(FromUtf8(params["name"].get<std::string>()));
        std::wstring content;
        if (params.contains("content") && params["content"].is_string())
            content = Trim(FromUtf8(params["content"].get<std::string>()));
        if (name.empty()) return L"[错误] saveTaskData 的 name 不能为空。";
        if (content.empty()) return L"[错误] saveTaskData 的 content 不能为空。";
        bool replace = false;
        if (params.contains("replace")) {
            const auto& r = params["replace"];
            if (r.is_boolean()) replace = r.get<bool>();
            else if (r.is_number_integer()) replace = r.get<int>() != 0;
        }
        const std::wstring saved = UpsertAiTaskDataBlock(name, content, replace);
        if (saved.empty()) return L"[错误] 数据缓存写入失败（路径：" + AiTaskDataPath() + L"）。";
        return L"已缓存数据块 [" + name + L"]（" + (replace ? L"覆盖" : L"追加")
            + L"），当前 " + std::to_wstring(saved.size()) + L" 字。后续直接 readTaskData(name="
            + name + L") 取用，勿再回数据源重新识图。";
    };
    return tool;
}

AgentTool MakeReadTaskDataTool() {
    AgentTool tool;
    tool.name = L"readTaskData";
    tool.description =
        L"读取本轮已缓存的数据块。name 省略时返回全部缓存。"
        L"★仅当本轮已对数据源识图并 saveTaskData 之后才能用来填表；"
        L"若缓存为空或用户任务要求打开 Edge/浏览器获取历史，必须先打开源，禁止编造记录。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "name": {
                "type": "string",
                "description": "数据块名；省略=返回全部缓存"
            }
        }
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        std::wstring name;
        if (params.contains("name") && params["name"].is_string())
            name = Trim(FromUtf8(params["name"].get<std::string>()));
        if (name.empty()) {
            const std::wstring all = ReadAiTaskDataAllText();
            if (all.empty()) return L"（数据缓存为空）";
            return all;
        }
        const std::wstring block = ReadAiTaskDataBlock(name);
        if (block.empty()) {
            return L"[错误] 没有名为「" + name
                + L"」的缓存块。可用 readTaskData() 看全部；或先在数据源识图一次后 saveTaskData 存下。";
        }
        return block;
    };
    return tool;
}

AgentTool MakeSwitchImeTool() {
    AgentTool tool;
    tool.name = L"switchIme";
    tool.description =
        L"切换前台输入法（IME）的中英文模式。英文=字母/数字/ASCII 导航键直接生效；"
        L"中文=拼音组字（仅当你确实需要直接敲拼音时用；quickInput 输中文走 Unicode 事件不依赖 IME）。"
        L"★被中文输入法拉住（按键没反应/数字变拼音/出现残留字母）：先 mode=query 查状态，"
        L"再 mode=english 切英文；或 locateAndClick 点任务栏右下角输入法图标选「英」。"
        L"ASCII 单键/表格导航键（Enter/Tab/Home）前宿主会自动切英文。状态不确定先 mode=query。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "mode": {
                "type": "string",
                "enum": ["english", "chinese", "query"],
                "description": "english=切英文；chinese=切中文；query=只查询不切换"
            }
        },
        "required": ["mode"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        std::wstring mode = L"query";
        if (params.contains("mode") && params["mode"].is_string())
            mode = FromUtf8(params["mode"].get<std::string>());
        for (auto& c : mode) {
            if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        }
        if (mode == L"QUERY") {
            const bool chn = ForegroundImeIsChineseLayout();
            return std::wstring(chn ? L"当前键盘布局为中文 IME（需英文加速键时会自动切换）"
                                    : L"当前键盘布局非中文 IME（无需处理）");
        }
        const bool chinese = (mode == L"CHINESE");
        const bool ok = SetForegroundImeMode(chinese);
        return std::wstring(L"已尝试切换 IME 为")
            + (chinese ? L"中文" : L"英文")
            + (ok ? L"。后续 ASCII 按键宿主仍会保持英文模式。" : L"，但未确认生效（前台无中文 IME 或切换失败）。");
    };
    return tool;
}

AgentTool MakeResolveSystemPathTool() {
    AgentTool tool;
    tool.name = L"resolveSystemPath";
    tool.description =
        L"取本机真实系统目录绝对路径（桌面/文档/下载/图片/用户目录）。"
        L"用途：openFile、核对目录；给了 fileName 则拼完整路径。"
        L"猜的 C:\\Users\\Administrator\\… 会被拒绝；以本工具返回为准。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "folder": {
                "type": "string",
                "enum": ["desktop", "documents", "downloads", "pictures", "profile", "appdata"],
                "description": "系统目录名，默认 desktop"
            },
            "fileName": {
                "type": "string",
                "description": "可选文件名（含扩展名），会拼成 目录\\文件名 返回"
            }
        }
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();

        std::wstring folder = L"desktop";
        if (params.contains("folder") && params["folder"].is_string())
            folder = Trim(FromUtf8(params["folder"].get<std::string>()));
        if (folder.empty()) folder = L"desktop";

        std::wstring dir = KnownFolderPathByName(folder);
        if (dir.empty())
            return L"[错误] 不支持的 folder：" + folder
                + L"。可用：desktop/documents/downloads/pictures/profile/appdata。";

        std::wstring fileName;
        if (params.contains("fileName") && params["fileName"].is_string())
            fileName = Trim(FromUtf8(params["fileName"].get<std::string>()));
        // 文件名里不能带路径分隔符，否则拼出的路径不可用
        if (fileName.find_first_of(L"\\/") != std::wstring::npos)
            return L"[错误] fileName 不能含路径分隔符，只写文件名。";

        std::wstring full = dir;
        if (!fileName.empty()) {
            if (!full.empty() && full.back() != L'\\') full += L'\\';
            full += fileName;
        }
        AiSession().lastResolvedDir = dir;
        AiSession().lastResolvedFullPath = fileName.empty() ? std::wstring() : full;

        std::wstring out = folder + L"=" + dir;
        if (!fileName.empty()) {
            out += L"；完整路径=" + full;
            out += L"\n★另存为快路径（优先，勿 scrollWheel 找侧栏）："
                  L"locateAndClick 文件名框 → quickInput(完整路径=\""
                + full + L"\", clearFirst=true) → Enter/保存。"
                  L"亦可只写「" + fileName + L"」若当前目录已是目标文件夹。"
                  L"侧栏滚不动用 mouseDrag 拖滚动条滑块。";
        } else {
            out += L"\n★另存为：文件名框可贴 目录\\\\文件名 完整路径（clearFirst），"
                  L"比滚动找「桌面」快；侧栏用 mouseDrag。";
        }
        if (!DirectoryExists(dir)) out += L"（注意：该目录当前不存在）";
        return out;
    };
    return tool;
}

// 最长公共子串长度（两个短描述，O(n*m) 足够）
static int LongestCommonSubstrLen(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return 0;
    std::vector<int> prev(b.size() + 1, 0), cur(b.size() + 1, 0);
    int best = 0;
    for (size_t i = 1; i <= a.size(); ++i) {
        for (size_t j = 1; j <= b.size(); ++j) {
            if (a[i - 1] == b[j - 1]) {
                cur[j] = prev[j - 1] + 1;
                if (cur[j] > best) best = cur[j];
            } else {
                cur[j] = 0;
            }
        }
        std::swap(prev, cur);
    }
    return best;
}

AgentTool MakeLocateAndClickTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"locateAndClick";
    tool.description =
        L"点击屏幕上用自然语言描述的目标。★★宿主另开无历史识图会话（不带主 Agent 对话），"
        L"只传短 target + 当前截图——禁止把整段任务原文塞进 target。"
        L"target≤80字：控件文案/颜色/大致位置即可。禁止用 aiActionExecute 外包定位。"
        L"★★选有特征的目标：按钮文字、图标、列标A/行号8、名称框「A1」等；"
        L"禁止以空白单元格/重复网格线为 target（等价于随机点表）。表格起点优先 keyClick(Ctrl+Home)。"
        L"宿主本地：截屏→粗定位→简单目标「紧凑即点」跳过二级（省一轮 API）→难目标自动 Zoom 精点→点击。"
        L"★打开桌面图标/文件必须 doubleClick=true（单击只会选中），或 button=right；"
        L"有路径优先 openFile。默认 refineLevels=1；难目标传 2。有模板优先 findImage。"
        L"未找到：换描述最多 1 次。细则 lookupMacroAction(section=composite)。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "target": {
                "type": "string",
                "minLength": 1,
                "description": "短描述≤80字：按钮文案/颜色/位置，勿粘贴整段任务"
            },
            "refineLevels": {
                "type": "integer",
                "minimum": 1,
                "maximum": 2,
                "description": "识图轮次，默认 1；难目标传 2"
            },
            "doubleClick": {
                "type": "boolean",
                "description": "true=双击。打开桌面图标/文件必须 true；点按钮/菜单项保持 false"
            },
            "button": {
                "type": "string",
                "enum": ["left", "right"],
                "description": "默认 left；right=右键出上下文菜单（随后再 locateAndClick 菜单项）"
            },
            "observeAfter": {
                "type": "boolean",
                "description": "默认 true：点击后截屏验收，避免盲连点。确认无需看图时 false"
            }
        },
        "required": ["target"]
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        std::wstring target;
        if (params.contains("target") && params["target"].is_string())
            target = FromUtf8(params["target"].get<std::string>());
        if (Trim(target).empty())
            return L"[错误] locateAndClick 的 target 不能为空。";
        if (Trim(target).size() > kMaxLocateAndClickTargetChars) {
            return L"[错误] locateAndClick.target 过长（"
                + std::to_wstring(Trim(target).size()) + L"字，上限 "
                + std::to_wstring(kMaxLocateAndClickTargetChars)
                + L"）。请只写要点击的控件短描述（文案/颜色/位置），"
                  L"勿粘贴整段任务；宿主会开无上下文识图，长文只会浪费 token。";
        }
        if (AiActionUiTooBusyForVisionLocate()) {
            return L"[错误] 前台页面动态干扰过大（游戏/广告/视频区在变），"
                L"禁止在页面内容上 locateAndClick（会白烧识图 token）。"
                L"先 keyClick(Ctrl+T) 新标签页再操作，或切到站外空白区。切窗用 activateWindow(标题关键词)。";
        }
        // 连续定位失败硬拦截：同一目标最多试 1 次；第 3 次起禁止再 locate 同一目标，
        // 避免「定位失败→滚动→再定位」原地打转烧轮次。换一个完全不同的目标仍允许。
        const int locateFailCount = AiLocateRetryCount();
        if (locateFailCount >= 2) {
            const std::wstring lastTarget = Trim(AiSession().lastLocateFailTarget);
            const std::wstring curTarget = Trim(target);
            const bool sameTarget = !lastTarget.empty() && !curTarget.empty()
                && (curTarget.find(lastTarget) != std::wstring::npos
                    || lastTarget.find(curTarget) != std::wstring::npos
                    || LongestCommonSubstrLen(curTarget, lastTarget) >= 4);
            if (sameTarget) {
                return L"[错误] 已连续 " + std::to_wstring(locateFailCount)
                    + L" 次定位「" + lastTarget
                    + L"」失败（同一目标最多试 1 次）。禁止继续换词重试同一目标："
                    L"画面没变不会变出目标。请二选一："
                    L"1) listWindows + activateWindow 确认目标窗口确实在前台后重试；"
                    L"2) completeTask(reason=未找到目标) 结束任务。";
            }
        }
        // 已写入完整文件路径时，勿再点换目录（会清掉路径）
        if (!AiSession().pendingSavePath.empty() && IsSaveDialogDetourTarget(target)) {
            return L"[错误] 已在保存框输入完整路径「" + AiSession().pendingSavePath
                + L"」，不要再点「" + Trim(target)
                + L"」。先 Enter/点保存；提交不动则改写纯文件名并点左侧文件夹。";
        }
        if (IsSaveSubmitTarget(target)) AiSession().pendingSavePath.clear();
        if (!hooks || !hooks->onLocateAndClick) {
            return L"[错误] locateAndClick 需要 AI 动作执行宿主（onLocateAndClick）。";
        }
        // 默认 1 级粗定位（省一轮识图 token）；难目标再传 refineLevels=2
        int refineLevels = 1;
        if (params.contains("refineLevels") && params["refineLevels"].is_number_integer()) {
            refineLevels = params["refineLevels"].get<int>();
            if (refineLevels < 1) refineLevels = 1;
            if (refineLevels > 2) refineLevels = 2;
        }
        // 默认观察：乱点后若 SKIP，Agent 会盲连下一目标
        const bool observeAfter = params.contains("observeAfter")
            ? ParseObserveAfterFlag(params) : true;
        bool doubleClick = false;
        if (params.contains("doubleClick")) {
            const auto& dc = params["doubleClick"];
            if (dc.is_boolean()) doubleClick = dc.get<bool>();
            else if (dc.is_number_integer()) doubleClick = dc.get<int>() != 0;
        }
        std::wstring button = L"left";
        if (params.contains("button") && params["button"].is_string()) {
            const std::wstring b = FromUtf8(params["button"].get<std::string>());
            if (b == L"right") button = L"right";
        }
        const std::wstring msg = hooks->onLocateAndClick(
            target, refineLevels, button, doubleClick ? 2 : 1);
        if (msg.rfind(L"[错误]", 0) == 0) {
            AiSession().lastLocateFailTarget = target;
            AiNoteLocateFailed();
            std::wstring out = msg
                + L"\n提示：同一目标最多再试 1 次（可换描述）；仍失败请 completeTask(reason=未找到)。"
                  L"禁止乱按 F 键/hotkeyShortcut 碰运气。";
            // 找任务栏图标/某程序窗口失败：给台账让它改走本地激活，别继续识图碰运气
            if (LooksLikeWindowSwitchTarget(target)) {
                out = WithWindowLedger(hooks, out)
                    + L"\n★要切窗就别识图找任务栏，直接 activateWindow(match=...)。";
            }
            return out;
        }
        AiClearLocateFailKeyBlock();
        ++AiSession().actionSeq;
        const wchar_t* marker = observeAfter ? kExecutedObserveMarker : kExecutedSkipObserveMarker;
        return std::wstring(marker) + L"\n" + msg;
    };
    return tool;
}

AgentTool MakeGetColorTool(AiActionHostHooks* hooks, bool allowAbs) {
    return MakeAtomicMacroTool(
        L"getColor",
        L"获取屏幕坐标 (x,y) 的颜色，保存到 matchVarName（默认 colorRet，值为 #RRGGBB）。x/y 必填。",
        LR"({
            "type": "object",
            "properties": {
                "x": { "type": "number" },
                "y": { "type": "number" },
                "matchVarName": { "type": "string", "description": "默认 colorRet" },
                "observeAfter": { "type": "boolean" }
            },
            "required": ["x", "y"]
        })",
        hooks, allowAbs, false);
}

AgentTool MakeFindColorTool(AiActionHostHooks* hooks, bool allowAbs) {
    return MakeAtomicMacroTool(
        L"findColor",
        L"在搜索区内找颜色。color 必填（#RRGGBB 或 r,g,b）；colorTolerance 为单通道最大差（默认 16）。"
        L"findImageFollowUp: 0点击 1移动 2保存变量。",
        LR"({
            "type": "object",
            "properties": {
                "color": { "type": "string", "minLength": 1, "description": "#RRGGBB 或 r,g,b" },
                "colorTolerance": { "type": "integer", "description": "默认 16" },
                "searchFullScreen": { "type": "boolean" },
                "searchX1": { "type": "number" },
                "searchY1": { "type": "number" },
                "searchX2": { "type": "number" },
                "searchY2": { "type": "number" },
                "findImageFollowUp": { "type": "integer" },
                "matchVarName": { "type": "string" },
                "observeAfter": { "type": "boolean" }
            },
            "required": ["color"]
        })",
        hooks, allowAbs, false);
}

AgentTool MakeColorMatchTool(AiActionHostHooks* hooks, bool allowAbs) {
    return MakeAtomicMacroTool(
        L"colorMatch",
        L"判断 (x,y) 处颜色是否匹配。color 与 x/y 必填；结果写入 matchVarName（found/score）。",
        LR"({
            "type": "object",
            "properties": {
                "x": { "type": "number" },
                "y": { "type": "number" },
                "color": { "type": "string", "minLength": 1 },
                "colorTolerance": { "type": "integer" },
                "matchVarName": { "type": "string" },
                "observeAfter": { "type": "boolean" }
            },
            "required": ["x", "y", "color"]
        })",
        hooks, allowAbs, false);
}

}  // namespace

bool AiQuickInputNeedsCtrlAClear(bool clearFirst, bool spreadsheetForeground,
    const std::wstring& dialogKind) {
    if (!clearFirst) return false;
    // 另存为/打开文件：焦点在文件名框，Ctrl+A 只选框内文字（即使宿主进程是 Excel）
    if (dialogKind == L"saveAs" || dialogKind == L"saveAsMini"
        || dialogKind == L"openFile") return true;
    // 表格前台：Ctrl+A=全选整表，点格后直接输入即可覆盖单元格
    if (spreadsheetForeground) return false;
    return true;
}

void ResetAiActionSessionState(unsigned long long runId) {
    AiSession().runId = runId;
    AiSession().lastOpenWebpageUrl.clear();
    AiSession().pendingSavePath.clear();
    AiSession().lastLaunchTick = 0;
    AiSession().launchSwitchWarned = false;
    AiSession().actionSeq = 0;
    AiSession().lastHideWindowsSeq = -100;
    AiSession().lastResolvedDir.clear();
    AiSession().lastResolvedFullPath.clear();
    AiSession().saveEscapeWarnedSeq = -100;
    AiSession().planGateEnabled = false;
    AiSession().planUnlocked = false;
    AiSession().lastToolSig.clear();
    AiSession().lastToolSigSeq = 0;
    AiSession().recipeVerifiedSig.clear();
    AiSession().recipePreviewGroups = 0;
    AiSession().recipeTemplateTrusted = false;
    AiSession().recipePreviewRowsFp.clear();
    AiSession().lastBusyCoverage = -1.0;
    AiSession().lastSettleStillChanging = false;
    AiSession().lookaheadStarts = 0;
    AiSession().saveAsScrollStreak = 0;
    AiSession().locateFailKeyBlock = false;
    AiSession().locateRetryAfterFail = 0;
    AiSession().bareTabStreak = 0;
    AiSession().submitUiBlocked = false;
    // 备忘 + 数据缓存都必须按本轮 runId 清空，否则会复用上轮的 historyRecords 等，
    // 导致「打开 Edge 抄历史」被跳过、直接拿旧数据填 Excel。
    const std::wstring memoPath = AiTaskMemoPath();
    EnsureParentDir(memoPath);
    {
        std::ofstream out(ToUtf8(memoPath), std::ios::binary | std::ios::trunc);
        (void)out;
    }
    const std::wstring dataPath = AiTaskDataPath();
    EnsureParentDir(dataPath);
    {
        std::ofstream out(ToUtf8(dataPath), std::ios::binary | std::ios::trunc);
        (void)out;
    }
    // 兼容旧版共享文件（runId=0 路径）：每次开新任务都清掉，防串味
    if (runId != 0) {
        const std::wstring legacyData = ImageVarRuntimeDir() + L"\\ai_task_data.md";
        const std::wstring legacyMemo = ImageVarRuntimeDir() + L"\\ai_task_memo.txt";
        DeleteFileW(legacyData.c_str());
        DeleteFileW(legacyMemo.c_str());
    }
}

void AiNoteLocateFailed() {
    AiSession().locateFailKeyBlock = true;
    ++AiSession().locateRetryAfterFail;
}

void AiClearLocateFailKeyBlock() {
    AiSession().locateFailKeyBlock = false;
    AiSession().locateRetryAfterFail = 0;
}

bool AiLocateFailKeyBlockActive() {
    return AiSession().locateFailKeyBlock;
}

int AiLocateRetryCount() {
    return AiSession().locateRetryAfterFail;
}

void SetAiActionPlanGateEnabled(bool enabled) {
    AiSession().planGateEnabled = enabled;
}

int AiNoteToolBatchSignature(const std::wstring& signature) {
    if (signature.empty()) return 0;
    if (signature == AiSession().lastToolSig)
        ++AiSession().lastToolSigSeq;
    else {
        AiSession().lastToolSig = signature;
        AiSession().lastToolSigSeq = 0;
    }
    return AiSession().lastToolSigSeq;
}

bool AiActionPlanGateEnabled() {
    return AiSession().planGateEnabled;
}

namespace {

bool MemoHasGoalOrTodos() {
    for (const auto& line : LoadMemoLines()) {
        const auto parts = SplitMemoStoredLine(line);
        if ((parts.first == L"goal" || parts.first == L"todos") && !parts.second.empty())
            return true;
    }
    return false;
}

bool IsPlanGatedToolName(const std::wstring& name) {
    if (name == L"wait"
        || name == L"listWindows"
        || name == L"resolveSystemPath"
        || name == L"updateTaskMemo"
        || name == L"readTaskMemo"
        || name == L"lookupMacroAction"
        || name == L"completeTask") {
        return false;
    }
    return IsMacroActionRunToolName(name);
}

AgentTool WithPlanGate(AgentTool tool) {
    if (!IsPlanGatedToolName(tool.name)) return tool;
    auto inner = std::move(tool.execute);
    const std::wstring name = tool.name;
    tool.execute = [inner, name](const std::wstring& args) -> std::wstring {
        if (const std::wstring err = CheckAiActionPlanGate(name); !err.empty())
            return err;
        return inner ? inner(args) : L"[错误] 工具未初始化。";
    };
    return tool;
}

}  // namespace

bool AiActionPlanGateIsOpen() {
    if (!AiSession().planGateEnabled) return true;
    // 嵌套 aiActionExecute（depth≥2）不再二次门闩
    if (AiActionExecuteNestDepth() > 1) return true;
    // 逻辑转化 else 回退：继承原任务上下文，勿强制再写 goal
    if (AiLogicConvertSessionIsHeal()) return true;
    if (AiSession().planUnlocked) return true;
    return MemoHasGoalOrTodos();
}

std::wstring CheckAiActionPlanGate(const std::wstring& toolName) {
    if (!IsPlanGatedToolName(toolName)) return {};
    if (AiActionPlanGateIsOpen()) return {};
    return L"[错误] 尚未写下任务骨架。请先 updateTaskMemo(section=goal 或 todos) "
           L"写清总目标与步骤大纲，再调用「" + toolName + L"」。"
           L"只读可用：listWindows / readTaskMemo / resolveSystemPath / lookupMacroAction。";
}

bool LooksLikeLocateOnlyOutsourcePrompt(const std::wstring& aiPrompt) {
    const std::wstring p = Trim(aiPrompt);
    if (p.empty() || p.size() > 160) return false;
    const bool locateish = ContainsAny(p, {
        L"点击", L"点一下", L"定位", L"找按钮", L"找到按钮", L"点按钮",
        L"单击", L"双击", L"click", L"Click", L"locate" });
    if (!locateish) return false;
    // 多步任务留给嵌套 Agent，不拦
    if (ContainsAny(p, {
        L"然后", L"接着", L"并且", L"同时", L"输入", L"填写", L"保存",
        L"打开", L"发送", L"复制", L"粘贴", L"再", L"之后" })) {
        return false;
    }
    return true;
}

void NoteAiActionUiBusy(double busyCoverageRatio, bool settleStillChanging) {
    AiSession().lastBusyCoverage = busyCoverageRatio;
    AiSession().lastSettleStillChanging = settleStillChanging;
}

bool AiActionUiTooBusyForVisionLocate() {
    // 与 observe 跳过上传阈值对齐（10% / 动态仍在变时 4%）
    return AiSession().lastBusyCoverage >= 0.10
        || (AiSession().lastSettleStillChanging && AiSession().lastBusyCoverage >= 0.04);
}

bool AiActionShouldSkipLookahead() {
    if (AiSession().lookaheadStarts >= 4) return true;
    if (AiActionUiTooBusyForVisionLocate()) return true;
    return false;
}

void NoteAiActionLookaheadStarted() {
    ++AiSession().lookaheadStarts;
}

std::wstring ReadAiTaskMemoText() {
    const auto lines = LoadMemoLines();
    if (lines.empty()) return L"";
    // 按固定章节顺序输出，便于压缩后恢复与模型扫读
    std::vector<std::wstring> buckets[kMemoSectionCount];
    for (const auto& line : lines) {
        const auto parts = SplitMemoStoredLine(line);
        size_t idx = kMemoSectionCount - 1;
        for (size_t i = 0; i < kMemoSectionCount; ++i) {
            if (parts.first == kMemoSectionOrder[i]) { idx = i; break; }
        }
        if (!parts.second.empty())
            buckets[idx].push_back(MemoSectionTag(kMemoSectionOrder[idx]) + parts.second);
    }
    std::wstring s;
    for (size_t i = 0; i < kMemoSectionCount; ++i) {
        for (const auto& row : buckets[i]) {
            if (!s.empty()) s += L"\n";
            s += row;
        }
    }
    return s;
}

bool UpsertAiTaskMemoSection(const std::wstring& sectionIn, const std::wstring& bodyIn,
    bool replaceSection) {
    std::wstring section = sectionIn;
    for (auto& c : section) c = static_cast<wchar_t>(towlower(c));
    if (!IsMemoSectionName(section)) section = L"notes";
    const std::wstring body = NormalizeMemoLine(bodyIn);
    if (body.empty()) return false;
    const std::wstring tag = MemoSectionTag(section);
    const std::wstring stored = tag + body;
    auto lines = LoadMemoLines();
    const bool replace = replaceSection || MemoSectionIsSingleton(section);
    if (replace) {
        std::vector<std::wstring> kept;
        for (const auto& line : lines) {
            const auto parts = SplitMemoStoredLine(line);
            if (parts.first == section) continue;
            kept.push_back(MemoSectionTag(parts.first) + parts.second);
        }
        lines = std::move(kept);
        lines.push_back(stored);
    } else {
        if (!lines.empty()) {
            const auto last = SplitMemoStoredLine(lines.back());
            if (last.first == section && last.second == body) {
                if (section == L"goal" || section == L"todos")
                    AiSession().planUnlocked = true;
                return true; // 去重
            }
        }
        lines.push_back(stored);
    }
    while (lines.size() > kAiMemoMaxLines) lines.erase(lines.begin());
    if (!SaveMemoLines(lines)) return false;
    if (section == L"goal" || section == L"todos")
        AiSession().planUnlocked = true;
    return true;
}

void AppendAiTaskMemoLine(const std::wstring& line) {
    const std::wstring n = NormalizeMemoLine(line);
    if (n.empty()) return;
    std::wstring section, body;
    ParseMemoSectionLine(n, section, body);
    if (body.empty()) body = n;
    if (!IsMemoSectionName(section)) section = L"notes";
    UpsertAiTaskMemoSection(section, body, MemoSectionIsSingleton(section));
}

size_t AiToolResultBudgetChars(const std::wstring& toolName) {
    // 分层预算：只读台账略宽，lookup/备忘中等，执行类摘要宜短
    if (toolName == L"listWindows") return 2200;
    if (toolName == L"lookupMacroAction") return 900;
    if (toolName == L"readTaskMemo" || toolName == L"updateTaskMemo") return 1200;
    if (toolName == L"readTaskData" || toolName == L"saveTaskData") return 2400;
    if (toolName == L"resolveSystemPath") return 400;
    if (toolName == L"runActionRecipe") return 1600;
    if (toolName == L"activateWindow") return 1600;
    if (toolName == L"locateAndClick") return 900;
    if (toolName == L"completeTask") return 400;
    // 脚本理解与排查类：readScript 已泛化为精炼中文说明（分页），预算收紧防长输入卡死；
    // 确需原始 JSON 时传 raw=true（内部 32KB 截断 + 分页提示）。
    if (toolName == L"readScript") return 16000;
    if (toolName == L"readAgentFile") return 48000;
    // 参考/技能结果入历史收紧：模型只需关键参数，多份参考不撑爆请求体
    // （几十 KB 历史会显著拖慢预填充，表现为「等待响应体 Ns 已接收 0 KB」）。
    if (toolName == L"readAgentSkill" || toolName == L"readScriptReference") return 8000;
    if (toolName == L"planScriptActions" || toolName == L"buildScriptActions"
        || toolName == L"createMacroScript" || toolName == L"writeScript") return 8000;
    if (toolName == L"getScriptStats" || toolName == L"listSettings"
        || toolName == L"listScheduledTasks") return 8000;
    if (toolName == L"listScripts" || toolName == L"listAgentChanges") return 4000;
    if (toolName == L"listDirectory") return 4000;
    if (toolName == L"searchAgentFiles" || toolName == L"runAgentCommand") return 8000;
    if (toolName == L"listAiModels") return 4000;
    return 1200;
}

std::wstring BuildAndValidateMacroActionJson(const json& params) {
    if (!params.is_object() || !params.contains("type") || !params["type"].is_string())
        return L"[错误] 缺少 type 字段。";

    json copy = params;
    if (!PrepareAiModelFields(copy))
        return L"[错误] 未配置可用 AI 模型。请先在「设置→AI助手」中添加模型。";

    std::wstring error;
    const std::wstring jsonArray = BuildScriptActionsJsonArray({copy}, error, false);
    if (!error.empty()) return L"[错误] " + error;
    return jsonArray;
}

bool IsMacroActionRunToolName(const std::wstring& name) {
    return name == L"submitMacroActions"
        || name == L"quickInput"
        || name == L"keyClick"
        || name == L"mouseClick"
        || name == L"moveMouse"
        || name == L"moveMouseRelative"
        || name == L"mouseDrag"
        || name == L"wait"
        || name == L"hotkeyShortcut"
        || name == L"switchWindow"
        || name == L"activateWindow"
        || name == L"runActionRecipe"
        || name == L"scrollWheel"
        || name == L"mouseDown"
        || name == L"mouseUp"
        || name == L"keyDown"
        || name == L"keyUp"
        || name == L"findImage"
        || name == L"locateAndClick"
        || name == L"openWebpage"
        || name == L"runProgram"
        || name == L"openAppViaSearch"
        || name == L"openFile"
        || name == L"getColor"
        || name == L"findColor"
        || name == L"colorMatch";
}

bool IsMacroExecutionToolName(const std::wstring& name) {
    return IsMacroActionRunToolName(name)
        || name == L"lookupMacroAction"
        || name == L"completeTask"
        || name == L"updateTaskMemo"
        || name == L"readTaskMemo"
        || name == L"saveTaskData"
        || name == L"readTaskData"
        || name == L"switchIme"
        || name == L"listWindows"
        || name == L"resolveSystemPath";
}

std::vector<AgentTool> BuildAiActionExecuteTools(AiActionHostHooks* hooks, AiActionToolOptions opts) {
    const bool allow = opts.allowAbsolutePointer;
    std::vector<AgentTool> tools = {
        MakeQuickInputTool(hooks, allow),
        MakeKeyClickTool(hooks, allow),
        MakeMouseClickTool(hooks, allow),
        MakeMoveMouseTool(hooks, allow),
        MakeMouseDragTool(hooks, allow),
        MakeScrollWheelTool(hooks, allow),
        MakeWaitTool(hooks, allow),
        MakeHotkeyShortcutTool(hooks, allow),
        MakeListWindowsTool(hooks),
        MakeActivateWindowTool(hooks),
        MakeRunActionRecipeTool(hooks, allow),
        MakeSwitchWindowTool(hooks),
        MakeFindImageTool(hooks, allow),
        MakeLocateAndClickTool(hooks),
        MakeResolveSystemPathTool(),
        MakeOpenWebpageTool(hooks, allow),
        MakeRunProgramTool(hooks, allow),
        MakeOpenAppViaSearchTool(hooks, allow),
        MakeOpenFileTool(hooks, allow),
        MakeFetchWebPageTool(),
        MakeGetColorTool(hooks, allow),
        MakeFindColorTool(hooks, allow),
        MakeColorMatchTool(hooks, allow),
        MakeUpdateTaskMemoTool(),
        MakeReadTaskMemoTool(),
        MakeSaveTaskDataTool(),
        MakeReadTaskDataTool(),
        MakeSwitchImeTool(),
        MakeSubmitMacroActionsToolLocal(hooks, allow),
        MakeLookupMacroActionTool(),
        MakeCompleteTaskTool(),
    };
    if (opts.fillTableOnly) {
        static const wchar_t* kDeny[] = {
            L"runProgram", L"openWebpage", L"openAppViaSearch", L"openFile",
            L"hotkeyShortcut", L"runActionRecipe", L"switchWindow",
            L"mouseClick", L"moveMouse", L"mouseDrag", L"findImage", L"findColor", L"getColor",
            L"colorMatch",
        };
        std::vector<AgentTool> filtered;
        filtered.reserve(tools.size());
        for (auto& t : tools) {
            bool deny = false;
            for (const wchar_t* d : kDeny) {
                if (t.name == d) { deny = true; break; }
            }
            if (!deny) filtered.push_back(std::move(t));
        }
        tools = std::move(filtered);
    }
    for (auto& t : tools)
        t = WithPlanGate(std::move(t));
    return tools;
}

std::vector<AgentTool> BuildAiActionExecuteToolsFull(AiActionHostHooks* hooks, AiActionToolOptions opts) {
    return BuildAiActionExecuteTools(hooks, opts);
}

bool IsCompleteTaskToolResult(const std::wstring& result) {
    return result.find(kTaskCompleteMarker) != std::wstring::npos;
}

bool ToolResultNeedsForceObserve(const std::wstring& result) {
    if (result.empty()) return false;
    return result.find(L"[UIA]") != std::wstring::npos
        || result.find(L"[事实] settle无反应") != std::wstring::npos
        || result.find(L"几乎无变化") != std::wstring::npos
        || result.find(L"灰色不可用") != std::wstring::npos
        || (result.find(L"提交钮") != std::wstring::npos
            && result.find(L"不可用") != std::wstring::npos)
        || result.find(L"定位框过大") != std::wstring::npos
        || result.find(L"未找到目标") != std::wstring::npos;
}

bool HistoryNeedsForceObserve(const std::vector<ChatMessage>& history, size_t afterIdx) {
    for (size_t i = afterIdx; i < history.size(); ++i) {
        const auto& m = history[i];
        if (m.role == L"tool" && ToolResultNeedsForceObserve(m.content)) return true;
    }
    return false;
}

bool IsSubmitObserveToolResult(const std::wstring& result) {
    if (result.find(kExecutedSkipObserveMarker) != std::wstring::npos) {
        // SKIP 标记但带不确定事实 → 仍视为要观察（升级）
        if (ToolResultNeedsForceObserve(result)) return true;
        return false;
    }
    if (result.find(kExecutedObserveMarker) != std::wstring::npos) return true;
    if (ToolResultNeedsForceObserve(result)) return true;
    return result.rfind(L"[EXECUTED]", 0) == 0;
}

bool IsSubmitSkipObserveToolResult(const std::wstring& result) {
    return result.find(kExecutedSkipObserveMarker) != std::wstring::npos
        && !ToolResultNeedsForceObserve(result);
}

bool HistoryHasCompleteTask(const std::vector<ChatMessage>& history, size_t afterIdx) {
    for (size_t i = afterIdx; i < history.size(); ++i) {
        const auto& m = history[i];
        if (m.role == L"assistant") {
            for (const auto& tc : m.tool_calls) {
                if (tc.name == L"completeTask") return true;
            }
        }
        if (m.role == L"tool" && IsCompleteTaskToolResult(m.content)) return true;
    }
    return false;
}

std::wstring ExtractCompleteTaskReason(const std::vector<ChatMessage>& history, size_t afterIdx) {
    // 优先从工具返回 `[TASK_COMPLETE] reason` 取；否则解析 assistant 的 args
    for (size_t i = afterIdx; i < history.size(); ++i) {
        const auto& m = history[i];
        if (m.role == L"tool" && IsCompleteTaskToolResult(m.content)) {
            const size_t pos = m.content.find(kTaskCompleteMarker);
            if (pos == std::wstring::npos) continue;
            std::wstring rest = Trim(m.content.substr(pos + wcslen(kTaskCompleteMarker)));
            if (!rest.empty()) return rest;
        }
    }
    for (size_t i = afterIdx; i < history.size(); ++i) {
        const auto& m = history[i];
        if (m.role != L"assistant") continue;
        for (const auto& tc : m.tool_calls) {
            if (tc.name != L"completeTask") continue;
            try {
                const json j = json::parse(ToUtf8(tc.arguments));
                if (j.contains("reason") && j["reason"].is_string())
                    return FromUtf8(j["reason"].get<std::string>());
            } catch (...) {}
        }
    }
    return {};
}

bool HistoryHasSubmitMacroActions(const std::vector<ChatMessage>& history, size_t afterIdx) {
    for (size_t i = afterIdx; i < history.size(); ++i) {
        const auto& m = history[i];
        if (m.role == L"assistant") {
            for (const auto& tc : m.tool_calls) {
                if (IsMacroActionRunToolName(tc.name)) return true;
            }
        }
    }
    return false;
}

bool HistoryHasSubmitRequestingObserve(const std::vector<ChatMessage>& history, size_t afterIdx) {
    for (size_t i = afterIdx; i < history.size(); ++i) {
        const auto& m = history[i];
        if (m.role == L"tool" && IsSubmitObserveToolResult(m.content)) return true;
    }
    return false;
}
