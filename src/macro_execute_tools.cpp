#include "macro_execute_tools.h"

#include "agent_ai_actions.h"
#include "agent_attachment.h"
#include "agent_web.h"
#include "ai_action_router.h"
#include "ai_decide.h"
#include "ai_plan_util.h"
#include "ai_logic_convert.h"
#include "image_var_util.h"
#include "office_doc.h"
#include "sqlite/browser_history.h"
#include "page_snapshot.h"
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

// 前置声明（定义在文件后部，与 LooksLikeUrlInput 同区）：
// 网页控件树相关的守卫要先问「前台到底是不是浏览器」。
bool AiForegroundIsBrowserWindow();
/// 前台是浏览器时返回可用于 runProgram 的启动名（edge/chrome/firefox）；否则空
std::wstring DetectForegroundBrowserLaunchName();
/// 前台窗口是否为表格软件（Excel/WPS 表格/OnlyOffice 等）；定义在文件后部（自检可覆盖）
bool ForegroundIsSpreadsheetApp();
/// 前台窗口的**类名**是否像浏览器（标题读不出来时的兜底判据；#32770 对话框不算）
bool ForegroundWindowIsBrowserClass();

namespace {

using json = nlohmann::json;

/// 路线指针（每任务一次，定义在文件后部）：把「这类活该用 runCommand」按需塞进工具结果
std::wstring AiRouteNudgeOnce();

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
    /// 最近一次成功 openWebpage 的注册域（同站禁止再编 URL）
    std::wstring lastOpenWebpageSite;
    /// 刚用地址栏打开的内置页（edge://…）；下一次 observePage 要核对是否真的到了
    std::wstring internalPagePendingVerify;
    /// 已往保存框输入、等待 Enter/保存 提交的完整文件路径（目录路径不记）
    std::wstring pendingSavePath;
    /// 已执行动作序号；用于判断「显示桌面」是不是在原地连按
    int actionSeq = 0;
    /// 本次 AI 动作里 locateAndClick 的**总次数**（不论成败）：防「换措辞反复定位」空转
    int locateAttemptsThisAction = 0;
    /// 上次 Win+D / Win+Down 等藏窗动作的序号（-100 = 没用过）
    int lastHideWindowsSeq = -100;
    /// resolveSystemPath 最近取到的真实目录 / 完整路径（保存链路的唯一可信路径）
    std::wstring lastResolvedDir;
    std::wstring lastResolvedFullPath;
    /// 已就「Escape 会取消保存」劝阻过的动作序号（-100 = 没劝过）
    int saveEscapeWarnedSeq = -100;
    /// 最近一次工具调用签名（名称+关键参数摘要）；用于连续重复动作劝阻
    std::wstring lastToolSig;
    int lastToolSigSeq = -100;
    /// runActionRecipe：steps 模板签名；试跑组数；模板是否已核对通过可全量复用
    std::wstring recipeVerifiedSig;
    int recipePreviewGroups = 0;
    bool recipeTemplateTrusted = false;
    /// 试跑时实际执行的前几行指纹；确认时若 rows 前缀匹配则跳过，否则视为剩余行全跑
    std::wstring recipePreviewRowsFp;
    /// 本任务里配方已经写进表格/文件的组数——用于「别 Ctrl+A 清表」护栏与重复试跑提醒
    int recipeRowsWritten = 0;
    /// 路线指针是否已提示过（每任务一次；见 AiRouteNudgeOnce）
    bool routeNudgeShown = false;
    /// 自检用：强制「前台是表格软件」的答案（0=按真实前台 / 1=是 / -1=否）
    int spreadsheetFgOverride = 0;
    /// 最近观察的动态覆盖；过高则禁在页面内容上识图
    double lastBusyCoverage = -1.0;
    bool lastSettleStillChanging = false;
    /// 模型明确要过截图（computer(screenshot)）→ 下一次观察必须回传帧
    bool explicitScreenshotRequest = false;
    /// 最近一批动作的 settle 判定：界面到底有没有反应（默认「有反应」= 保守，不乱补点）
    bool lastSettleReactedBatch = true;
    /// 另存为里连续 scrollWheel 次数（软提示，不硬拦）
    int saveAsScrollStreak = 0;
    /// locate 失败重试计数：**只用于如实回执里的计数与提示**（不再用来拦截重试）。
    /// 被 `AiLocateRetryCount()` 读取：每轮如实提示 + 回执里的「本次动作已定位 N 次」。
    /// ⚠ 旧注释还写着「completeTask 的『别宣称已完成』守卫」—— 那个守卫**批 B 已删**，
    ///   批 D 清掉这句陈旧文案（D7②：静态文案 × 动态事实必须同源）。
    int locateRetryAfterFail = 0;
    /// 连续纯 Tab（无修饰）次数；配方失败后盲 Tab 会把表格列对齐打崩
    int bareTabStreak = 0;
    /// UIA 探测到「提交钮是灰的」这件事本身（下一轮 Enter 只**如实标注**它，不再拦）
    bool submitUiBlocked = false;
    /// 本轮已成功 fetchWebPage：随后启动程序要确认
    bool webFetchUntrusted = false;
    /// 最近一次 observePage 的 pageKind（dom|mixed|canvas）
    std::wstring lastPageKind;
    /// 控件树是否与前台标签一致；false 时禁止用 dom 判定剥截图（必须留视觉兜底）
    bool pageTreeTrusted = true;
    std::wstring pageTreeUntrustedReason;
    /// 自检用：强制「前台是浏览器」的答案（0=按真实前台 / 1=是 / -1=否）
    int foregroundBrowserOverride = 0;
    /// 自检用：打开的浏览器启动目标（名或路径）；非空时直接用它，不探测前台进程
    std::wstring browserLaunchTargetOverride;
    std::wstring lastPageUrl;
    /// 已打开/切到浏览器页（扩展优化链路；未装扩展或树上没有仍可识图）。
    bool webBrowseSession = false;
    /// 最近一次 quickInput 实际输入的文本 + 当时的动作序号。
    /// 用途：区分「在地址栏输入 URL 后回车」与「在站内搜索框回车」（后者只**如实标注**）。
    std::wstring lastQuickInputText;
    int lastQuickInputSeq = -100;
    std::wstring lastClickRef;
    std::wstring lastTypeSearchText;
    struct SnapshotHref {
        std::wstring ref;
        std::wstring href;
        std::wstring name;
        std::wstring role;
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
        int screenX = 0;
        int screenY = 0;
        /// DOM 优先点击要判「可视区」，必须随树一起留档
        bool inView = true;
    };
    std::vector<SnapshotHref> lastSnapshotHrefs;
    int lastSnapshotInViewContent = 0;
    int samePageClickStreak = 0;
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
    if (AiSession().spreadsheetFgOverride != 0)
        return AiSession().spreadsheetFgOverride > 0;
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

bool MemoTextUnlocksPlan(const std::wstring& text) {
    if (text.find(L"goal:") != std::wstring::npos
        || text.find(L"Goal:") != std::wstring::npos
        || text.find(L"[goal]") != std::wstring::npos
        || text.find(L"[GOAL]") != std::wstring::npos)
        return true;
    if (text.find(L"todos:") != std::wstring::npos
        || text.find(L"Todos:") != std::wstring::npos
        || text.find(L"[todos]") != std::wstring::npos)
        return true;
    return false;
}

std::vector<std::pair<std::wstring, std::wstring>> ParseMemoNoteChunks(const std::wstring& raw) {
    std::vector<std::pair<std::wstring, std::wstring>> chunks;
    size_t start = 0;
    while (start <= raw.size()) {
        size_t nl = raw.find(L'\n', start);
        if (nl == std::wstring::npos) nl = raw.size();
        std::wstring piece = raw.substr(start, nl - start);
        if (!piece.empty() && piece.back() == L'\r') piece.pop_back();
        piece = NormalizeMemoLine(piece);
        if (!piece.empty()) {
            std::wstring section, body;
            ParseMemoSectionLine(piece, section, body);
            if (body.empty()) body = piece;
            if (!IsMemoSectionName(section)) section = L"notes";
            if (!body.empty())
                chunks.emplace_back(std::move(section), std::move(body));
        }
        if (nl >= raw.size()) break;
        start = nl + 1;
    }
    return chunks;
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

std::wstring ValidateAndMergeActions(const json& params, bool allowAbsolutePointer,
    std::wstring* outNote = nullptr) {
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
            // ★「禁止用 aiActionExecute 外包定位/点击」那道判据**已删（批 D，docs §47）**：
            //   它靠**对提示词做文本匹配**（ContainsAny 一堆动词 + 长度 ≤160）——
            //   §36.1 明令禁止的判据形态（措辞一改就静默失效），而且拦的是一个模型
            //   可能比引擎更清楚的选择。嵌套深度熔断（上面那条）是**真限制**，照留。
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
                    // ⚠ 提示必须**贴着这个动作**说：旧文案对 `mouseDown`/`mouseUp` 也讲
                    //   「点击输入框须给截图坐标…」——一次拖拽失败因此被模型读成
                    //   「我点输入框的方式不对」，改去别处瞎试（实测 `mouseDrag` 从未成功过）。
                    const std::wstring tip = (type == "mouseClick")
                        ? L"点击输入框须给截图坐标；若输入框已聚焦可直接 quickInput，勿乱点。"
                        : L"坐标是 upload 截图像素（与元素索引/文字索引同一套）。";
                    return L"[错误] 第 " + std::to_wstring(i + 1)
                        + L" 个 " + FromUtf8(type) + L" 缺少 x/y。" + tip;
                }
            }
        }
        if (!allowAbsolutePointer
            && (type == "moveMouse" || type == "mouseClick"
                || type == "mouseDown" || type == "mouseUp")
            && !isTruthyMoveFromVar(one)
            && one.contains("x") && one.contains("y")) {
            // ★这里原先是 `return L"[错误] 无截图时禁止用绝对坐标点击/移动。"`（拦下不执行）。
            //   批 D 改成**执行 + 如实标注**（docs §47 / D-a 判据）：
            //   宿主**本来就有**截图映射（见 ai_action_service 里 liveOpts 的注释），
            //   模型可能知道得比守卫多（坐标可以来自元素索引或上一帧），
            //   「禁止」这个词还承诺了一个引擎并不实施的限制（§46.2 的「谎」）。
            if (outNote && outNote->empty()) {
                *outNote = L"\n[事实] 本轮没有把截图回传给模型：这一下按 upload 截图像素坐标"
                           L"执行（基准取自宿主最近一次截屏）。";
            }
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

namespace {

/// 坐标动作的回执后缀：`(x,y)`。口径 = **upload 截图像素**（与 `mouseClick` 入参、
/// 元素索引给的坐标同一套），所以直接打印原值，**不做任何换算**（docs §33.1/§50）。
std::wstring CoordSuffix(const json& step) {
    if (!step.contains("x") || !step.contains("y")) return L"(未给坐标)";
    if (!step["x"].is_number() || !step["y"].is_number()) return L"(未给坐标)";
    return L"(" + std::to_wstring(step["x"].get<int>()) + L","
        + std::to_wstring(step["y"].get<int>()) + L")";
}

/// 拖拽的回执后缀：`(起点)→(终点)`。
/// ⚠ 字段名以**脚本 schema 为准**（`script_action_builder.cpp` / `script_io.cpp`）：
///   `mouseDrag` 动作的起点是 `x/y`、终点是 `endX/endY`。工具入参用的那套
///   `fromX/fromY/toX/toY` 只是**工具层**的叫法（`computer(left_click_drag)` 也照它写），
///   读进 `ScriptAction` 时没有任何一处读 `fromX/toX` ⇒ 两套都要认，
///   否则回执会印出「(未给坐标)」而实际动作里坐标是有的（自检抓到的就是这个）。
std::wstring DragSuffix(const json& step) {
    auto num = [&step](const char* k, int* out) {
        if (!step.contains(k) || !step[k].is_number()) return false;
        *out = step[k].get<int>();
        return true;
    };
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    const bool haveStart = (num("x", &x1) && num("y", &y1))
        || (num("fromX", &x1) && num("fromY", &y1));
    const bool haveEnd = (num("endX", &x2) && num("endY", &y2))
        || (num("toX", &x2) && num("toY", &y2));
    if (!haveStart || !haveEnd) return L"(未给坐标)";
    return L"(" + std::to_wstring(x1) + L"," + std::to_wstring(y1) + L")→("
        + std::to_wstring(x2) + L"," + std::to_wstring(y2) + L")";
}

}  // namespace

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
            } else if (t == "mouseClick" || t == "moveMouse"
                || t == "mouseDown" || t == "mouseUp") {
                // ★★坐标动作必须**回显落点**（实测事故）：原先这里没有分支，模型发来
                //   `mouseClick(243,50)` 只拿回「已执行:mouseClick」——**连点在哪儿都没说**。
                //   在游戏画面上（没有元素索引可查、卡片没有文字）模型只能靠手算坐标点，
                //   而回执不给坐标 ⇒ 它无法判断这一下到底落在哪、更无法判断有没有生效，
                //   于是反复点同一张卡、反复开合同一个面板（用户主诉：「光选卡，不会放僵尸」）。
                //   口径与元素索引/mouseClick 入参**同一套**：upload 截图像素（见
                //   DescribeClickPointForModel 的注释），所以这里直接打印原值，不做换算。
                s += CoordSuffix(step);
            } else if (t == "mouseDrag") {
                s += DragSuffix(step);
            } else if (t == "scrollWheel" && step.contains("x") && step.contains("y")) {
                s += CoordSuffix(step);
                if (step.contains("delta") && step["delta"].is_number())
                    s += L" Δ" + std::to_wstring(step["delta"].get<int>());
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
    const bool firstActionOfTask = (AiSession().actionSeq == 0);
    ++AiSession().actionSeq;
    const std::wstring summary = SummarizeExecutedActionsJson(merged);
    std::wstring execMsg;
    if (hooks && hooks->onExecuteActions) {
        execMsg = hooks->onExecuteActions(merged);
        // 宿主拒绝全部步骤时勿包一层 EXECUTED，否则 Agent 会误判已执行成功
        if (execMsg.rfind(L"[错误]", 0) == 0) return execMsg;
    }
    const bool needObserve = AiDecideNeedObserveAfterExec(
        observeAfter, firstActionOfTask, execMsg, summary);
    const wchar_t* marker = needObserve ? kExecutedObserveMarker : kExecutedSkipObserveMarker;
    std::wstring out = std::wstring(marker) + L"\n" + summary;
    if (!execMsg.empty()) out += L"\n" + execMsg;
    return out;
}

AgentTool MakePlanSpendTool() {
    AgentTool tool;
    tool.name = L"planSpend";
    tool.description =
        L"★先算账：给定预算与可选项，算出「怎么花最划算」的确定性方案。"
        L"**资源分配不要靠心算**——实测 AI 会点「一键全选」让弱卡占满卡槽、或阳光够却只放 2~3 只。"
        L"用法：items 每项给 name/cost（value 可选=强度，缺省按 cost；maxCount 可选=最多几次）。"
        L"⚠⚠ **name 必须是屏幕上真实存在的短标签**（索引里查得到的字，如「铁桶僵尸」）——"
        L"它就是后面 locateAndClick 的 target。**拿价格当目标一定点空**（同屏多卡同价）。"
        L"distinctOnly=true 且 slots=卡槽数 → 选卡名单（按性价比，别一键全选）；"
        L"distinctOnly=false + budget → 这一波放哪些、各放几个。"
        L"拿到 summary 后照它执行（逐个 clickRef/locateAndClick；批量落点用 locateAndClick(targets=[…])）。"
        L"**只给了 costs 的名单只能算账，不能拿去点**。";
    tool.parameters_json = LR"({
            "type": "object",
            "properties": {
                "budget": { "type": "number", "description": "总预算（阳光/金币/费用）" },
                "slots": { "type": "integer", "description": "最多挑几种（卡槽数）；0=不限" },
                "distinctOnly": { "type": "boolean", "description": "true=每种最多一个（选卡场景）；false=可按预算重复放（放单位场景）" },
                "items": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "properties": {
                            "name": { "type": "string" },
                            "cost": { "type": "number" },
                            "value": { "type": "number", "description": "强度/价值；缺省按 cost" },
                            "maxCount": { "type": "integer", "description": "最多用几次；缺省不限" },
                            "locked": { "type": "boolean", "description": "已锁定/不可更换" }
                        },
                        "required": ["name", "cost"]
                    }
                },
                "costs": {
                    "type": "array",
                    "items": { "type": "number" },
                    "description": "★简写：只知道价格、不想编名字时用它（如 [600,400,175,75,50,25]）。与 items 二选一。"
                }
            },
            "required": ["budget"]
        })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        double budget = 0.0;
        if (params.contains("budget") && params["budget"].is_number())
            budget = params["budget"].get<double>();
        if (!(budget > 0.0)) return L"[错误] planSpend.budget 必须 > 0（写清楚总预算）。";
        int slots = 0;
        if (params.contains("slots") && params["slots"].is_number_integer())
            slots = params["slots"].get<int>();
        bool distinctOnly = false;
        if (params.contains("distinctOnly") && params["distinctOnly"].is_boolean())
            distinctOnly = params["distinctOnly"].get<bool>();
        std::vector<PlanSpendItem> items;
        bool costsOnly = false;
        if (params.contains("items") && params["items"].is_array()) {
            for (const auto& it : params["items"]) {
                if (!it.is_object()) continue;
                PlanSpendItem x;
                if (it.contains("name") && it["name"].is_string())
                    x.name = Trim(FromUtf8(it["name"].get<std::string>()));
                if (it.contains("cost") && it["cost"].is_number()) x.cost = it["cost"].get<double>();
                if (it.contains("value") && it["value"].is_number()) x.value = it["value"].get<double>();
                if (it.contains("maxCount") && it["maxCount"].is_number_integer())
                    x.maxCount = it["maxCount"].get<int>();
                if (it.contains("locked") && it["locked"].is_boolean())
                    x.locked = it["locked"].get<bool>();
                if (!x.name.empty()) items.push_back(std::move(x));
            }
        }
        // ★简写：只知道价格、不知道名字时也能算（实测模型因为「要编 item 名字」干脆跳过本工具，
        // 直接点了一键全选）。costs=[600,400,75,…] 等价于按价格建候选。
        if (items.empty() && params.contains("costs") && params["costs"].is_array()) {
            int i = 0;
            for (const auto& c : params["costs"]) {
                if (!c.is_number()) continue;
                PlanSpendItem x;
                // ⚠ 占位符必须**明显不是目标**：旧写法 `选项3(花费500)` 看起来像个可点的名字，
                //   而描述又让模型「照名单逐个点卡」⇒ 它真的去 locateAndClick("500")。
                x.name = L"#" + std::to_wstring(++i) + L"(cost"
                    + std::to_wstring(static_cast<long long>(c.get<double>()))
                    + L",noname)";
                x.cost = c.get<double>();
                x.value = x.cost;
                items.push_back(std::move(x));
                if (items.size() >= 60) break;
            }
            costsOnly = !items.empty();
        }
        if (items.empty()) return L"[错误] planSpend.items 为空（至少给一项 name+cost）。";
        if (items.size() > 80) items.resize(80);
        AiNotePlanSpendCalled();   // 放行标记：本次动作算过账，之后才允许批量选择
        const PlanSpendPlan plan = PlanSpendBudget(budget, slots, distinctOnly, items);
        std::wstring out = plan.summary;
        out += L"\n（明细：";
        for (size_t i = 0; i < plan.picks.size(); ++i) {
            if (i) out += L"；";
            out += plan.picks[i].name + L" 花费 "
                + std::to_wstring(static_cast<long long>(plan.picks[i].cost))
                + L"，共 " + std::to_wstring(plan.picks[i].count) + L" 个";
        }
        out += L"）";
        // ★★「清单要能被执行」是产清单的工具的责任（docs §44）：
        //   只给 costs 时上面那些名字是**占位符**，价格在同屏多张卡上会重复、
        //   也未必进得了 OCR 索引。旧版把它们写成「选项1(花费9999)」这种**像名字的东西**，
        //   而描述又写着「照它执行、逐个点名单里的卡」⇒ 模型真的去 locateAndClick("9999")
        //   ⇒ 索引里没有 → 拿裸数字问 VLM（每次给的框都不一样）→ 点空 → 死点表/近点重复
        //   连环触发 → 改用不带目标语义的手算坐标 mouseClick → 在选卡界面上把**已选中的卡
        //   又点掉**（用户：「把选了的卡收回去又重新选」）。所以这里必须当场说清。
        if (costsOnly) {
            out += L"\n⚠ 这份名单**只有价格、没有名字**（上面是占位符，不是屏幕上的字）——"
                L"价格在同屏多张卡上会重复，**不能用来定位点击**。它只回答「这波该花多少、选几种」；"
                L"要逐张点卡请用 items 带 name 重调，name 写**卡面上真实存在的短标签**"
                L"（元素索引/OCR 里查得到的字），价格只在回执里用于核对。";
        }
        return out;
    };
    return tool;
}

AgentTool MakeLookupMacroActionTool() {
    AgentTool tool;
    tool.name = L"lookupMacroAction";
    tool.description =
        L"按需查阅参数。优先直接调规范工具；勿每轮开头先查 section=agent/usage（费 token）。"
        L"仅缺某 type 字段时查 type=该名；组合定位 section=composite。"
        L"**产品自有 Skill**：section=game（游戏/动态画面）、command（命令行）、office（办公文档）。"
        L"参数只填**裸值**（type=\"game\"），不要写成 type=\"section=game\"。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "type": {
                "type": "string",
                "description": "动作 type，或 section 的**裸名**：agent|usage|composite|game|command|office|mouse|keyboard|flow|findImage|ocr|system|ai|all|catalog（勿写 section= 前缀）"
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
        // 兼容模型写整段键值对（section=game）的情况 —— 见 NormalizeMacroLookupQuery 注释
        query = NormalizeMacroLookupQuery(query);
        if (Trim(query).empty()) query = L"usage";
        std::wstring body = LookupMacroActionSchema(query);
        // 入历史截断，避免 Skill 全文在后续每轮重复计费
        constexpr size_t kMax = 900;
        if (body.size() > kMax) {
            body.resize(kMax);
            body += L"\n…(已截断；勿再 lookup 同节，缺字段再查具体 type=)";
        }
        // ⚠ 「文案提到的工具必须在本轮工具表里」那套服务点（docs §43）**已随批 C 撤销**：
        //   引擎不再裁工具表，静态文案与工具表之间不再有「两处事实」的可能。
        return body;
    };
    return tool;
}

AgentTool MakeCompleteTaskTool() {
    AgentTool tool;
    tool.name = L"completeTask";
    tool.description =
        // ★★★ 关键指令**必须排在描述最前面**（2026-10-02 A2 实测，见 LESSONS §79）：
        //   网页 AI 路径下，模型看到的描述是被**截断**的 ——
        //     目录 `RenderToolCatalog` 只取 `catalogDescChars = 48` **字节**（≈16 个汉字）；
        //     扁平清单 `RenderToolList` 只取 160 字节。
        //   ⇒ 排在后面的文字**根本不会发给模型**。
        //   ⚠ 前科：原来把「收尾判据」加在**末尾** ⇒ 加了等于没加（用户看不出任何变化）。
        L"★做完就收尾：目标一旦达成（回执说已执行 / 界面已变化 / 截图·树里能看到结果）"
        L"就**立刻调用本工具**结束本轮 —— 不要「再确认一下」、不要重复点同一处、"
        L"不要因为不确定而继续试。确实做不到（识图 NOT_FOUND / 元素不存在）也用本工具收尾，"
        L"并把原因写进 reason。"
        // ↓ 以下是次要信息，排在后面（可能被目录截断，但 loadTools 的完整定义里仍在）
        L"任务验收通过或确认无需再操作时调用；reason 可选（如「已发送回复」或「对方未发言」）。"
        L"调用后本轮 Agent 闭环结束。若上一工具返回 [错误]，禁止调用本工具，须先修正再执行。"
        L"识图 NOT_FOUND 后禁止宣称已完成（须 clickRef 树上按钮，或 reason 写未找到）。";
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
        // ★模型要收工就如实收工：引擎**不**拿「上一轮识图没找到」去否决它。
        // （这条原来是一道内联存活下来的否决闸，把 completeTask —— 模型唯一诚实的
        //   出口 —— 按文本匹配锁死；判据是文本匹配本身也违反 §36.1。）
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
        std::wstring note;
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbsolutePointer, &note);
        return FinishExecuteActions(hooks, merged, observeAfter) + note;
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

/// 读一个「真假不限」的 JSON 字段（true / 1 / "1" 都算真）
bool JsonFlagLike(const json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key)) return false;
    const auto& v = obj[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<long long>() != 0;
    if (v.is_number_float()) return v.get<double>() != 0.0;
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        return s == "1" || s == "true" || s == "True";
    }
    return false;
}

std::wstring UpperKeyText(const json& step) {
    std::wstring kt;
    if (step.is_object() && step.contains("keyText") && step["keyText"].is_string())
        kt = Trim(FromUtf8(step["keyText"].get<std::string>()));
    for (auto& c : kt)
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    return kt;
}

/// 模板里是否含「回到 A1」类导航（Ctrl+Home）。
/// ★重复模板里出现它 = 每一组都写回第一行 → 反复覆盖表头/前几行（实测症状：
/// 「复用逻辑时反复输入，最后前两行被复用内容全被删/覆盖」）。模板里只能有行内导航
/// （Tab / Enter / Home），起点 Ctrl+Home 只在开跑前按一次。
bool RecipeStepsGoBackToOrigin(const json& steps) {
    if (!steps.is_array()) return false;
    for (const auto& st : steps) {
        if (!st.is_object()) continue;
        const std::string type = st.contains("type") && st["type"].is_string()
            ? st["type"].get<std::string>() : std::string();
        const std::wstring kt = UpperKeyText(st);
        const bool ctrl = JsonFlagLike(st, "holdLeftCtrl") || kt.rfind(L"CTRL+", 0) == 0;
        if (type == "keyClick" && ctrl && (kt == L"HOME" || kt == L"CTRL+HOME")) return true;
        if (type == "hotkeyShortcut" && ctrl
            && (JsonFlagLike(st, "home") || kt == L"HOME" || kt == L"CTRL+HOME")) {
            return true;
        }
    }
    return false;
}

/// 模板是否只是「逐格录入」（quickInput / Tab / Enter / Home 之类），
/// 用于提示「这种活本来 runCommand 一条命令就能做完」。
bool RecipeStepsLookLikePlainDataEntry(const json& steps) {
    if (!steps.is_array() || steps.empty()) return false;
    bool hasInput = false;
    for (const auto& st : steps) {
        if (!st.is_object()) return false;
        const std::string type = st.contains("type") && st["type"].is_string()
            ? st["type"].get<std::string>() : std::string();
        if (type == "quickInput") { hasInput = true; continue; }
        if (type == "keyClick") {
            const std::wstring kt = UpperKeyText(st);
            if (kt == L"TAB" || kt == L"ENTER" || kt == L"RETURN" || kt == L"HOME"
                || kt == L"DOWN" || kt == L"RIGHT" || kt == L"CTRL+HOME") {
                continue;
            }
            return false;
        }
        return false;
    }
    return hasInput;
}

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
        // 本次调用**之前**已经写进去的组数（用于「别覆盖已写内容」的提醒口径）
        const int rowsWrittenBefore = AiSession().recipeRowsWritten;
        if (params.contains("verifiedGroups") && params["verifiedGroups"].is_number())
            verifiedGroups = params["verifiedGroups"].get<int>();
        const std::wstring stepsSig = RecipeBatchSignature(steps, rows);
        // ★模板里含 Ctrl+Home 时**不拦**（有些复用就是「每次回到固定区域覆盖填写」，拦了会抓瞎），
        // 只在结果里把后果说清楚，让模型自己判断：逐行追加数据时它会导致每组都覆盖表头/第一行。
        const bool stepsGoHome = (nRows >= 2) && RecipeStepsGoBackToOrigin(steps);
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
        std::wstring note;
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs, &note);
        std::wstring out = FinishExecuteActions(hooks, merged, observeAfter) + note;
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
            AiSession().recipeRowsWritten += static_cast<int>(rowsToRun.size());
            UpsertAiTaskMemoSection(L"recipe",
                L"rows=" + std::to_wstring(nRows) + L" steps=" + std::to_wstring(steps.size())
                    + L" preview=" + (previewMode ? L"1" : L"0")
                    + L" trusted=" + (AiSession().recipeTemplateTrusted ? L"1" : L"0")
                    + L" written=" + std::to_wstring(AiSession().recipeRowsWritten),
                true);
        }
        if (out.rfind(L"[错误]", 0) != 0 && stepsGoHome) {
            out += L"\n★注意：steps 模板里有 Ctrl+Home —— 若你的用法是**逐行追加数据**，"
                L"它会让每一组都跳回 A1，把表头/前几行反复覆盖（实测就是这么把已写好的前两行弄没的）；"
                L"那种用法应该把起点 Ctrl+Home 单独发一次，模板里只用 Enter(下行)+Home(回 A 列)。"
                L"若你本来就要「每组回到固定区域覆盖填写」（例如同一张表反复填新值），"
                L"那这条忽略即可。判断依据：看截图确认表头是否被覆盖。";
        }
        // ★「别把已写内容弄丢」的两条提醒（都基于宿主的真实写入计数）：
        //  · 已经写过 N 行又来试跑/换模板 → 明确告知会从当前光标继续写，别 Ctrl+A 清表；
        //  · 纯逐格录入的活 → 提醒本来可以用 runCommand 一条命令做完。
        if (out.rfind(L"[错误]", 0) != 0 && previewMode && rowsWrittenBefore > 0) {
            out += L"\n★本表已经用配方写入约 " + std::to_wstring(rowsWrittenBefore)
                + L" 组数据。这次「试跑」是从**当前光标位置**继续写的，不是从 A1 —— "
                  L"若你换了模板或先按了 Ctrl+Home，就会重复写/覆盖已有行。"
                  L"禁止用 Ctrl+A + Delete 清表重来（会丢掉已写好的内容）："
                  L"先看截图确认光标在哪一行，再决定继续填还是只修错误格。";
        }
        if (out.rfind(L"[错误]", 0) != 0 && skipGroups > 0) {
            out += L"\n（已跳过试跑前缀 " + std::to_wstring(skipGroups)
                + L" 组；本表累计写入约 "
                + std::to_wstring(AiSession().recipeRowsWritten) + L" 组）";
        }
        if (out.rfind(L"[错误]", 0) != 0 && previewMode && rowsWrittenBefore == 0
            && RecipeStepsLookLikePlainDataEntry(steps)) {
            out += AiRouteNudgeOnce();
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
        std::wstring note;
        const std::wstring merged = ValidateAndMergeActions(params, allowAbsolutePointer, &note);
        return FinishExecuteActions(hooks, merged, observeAfter) + note;
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
                "description": "true=把文本和变量里的 \\n \\t \\\\ 转成换行/Tab/反斜杠；默认 false（变量内换行/Tab 丢掉）"
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
            AiResetLocateMiss();
            AiSession().bareTabStreak = 0;
            // 记下刚输入的正文：地址栏导航（Ctrl+L → 输入 URL → Enter）要据此放行 Enter
            AiSession().lastQuickInputText = text;
            AiSession().lastQuickInputSeq = AiSession().actionSeq;
            // 在表格软件里逐格输入 = 典型的「本该 runCommand 一条命令做完」
            if (ForegroundIsSpreadsheetApp()) result += AiRouteNudgeOnce();
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

/// 是否为「表格/文档类」程序名或路径（Excel/WPS/Calc/Word…）
bool LooksLikeSpreadsheetOrDocTarget(const std::wstring& target) {
    std::wstring t = target;
    for (auto& c : t)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    static const wchar_t* kKeys[] = {
        L"excel", L"winword", L"word", L"wps", L"et.exe", L"soffice", L"calc",
        L"numbers", L"spreadsheet", L"onlyoffice",
    };
    for (auto* k : kKeys) {
        if (t.find(k) != std::wstring::npos) return true;
    }
    return false;
}

/// 路线指针（每个任务最多一次）：只在**真的在做 GUI 数据录入**的工具结果里追加，
/// 指向 Skill（`lookupMacroAction(section=command)`）——按需付费。
/// 为什么不写进系统提示词：那是每一轮都要付费的固定成本，而「该用命令行还是点界面」
/// 只在一部分任务里相关（用户明确要求：不产生额外开销，整理到 Skill + 工具函数配合）。
/// 本次动作里是否调用过 planSpend（选卡/批量选择前的硬门槛；每次动作开始由服务层复位）

std::wstring AiRouteNudgeOnce() {
    if (AiSession().routeNudgeShown) return {};
    AiSession().routeNudgeShown = true;
    // ★自带可照抄的命令，不让模型再花一轮 lookup（实测「只给指针」时模型根本不去查，
    // 直接照着原计划继续逐格点）。这里只花一次、且只在相关任务里花。
    return L"\n★路线提示（本任务只提示一次，照抄即可）：这批数据**不需要逐格点界面**。"
           L"直接用 runCommand 一条命令落盘：\n"
           L"  {\"command\":\"$rows=@('序号,标题,网址,时间','1,标题A,example.com,23:54');"
           L"$p=Join-Path ([Environment]::GetFolderPath('Desktop')) '记录.csv';"
           L"$rows | Out-File -Encoding utf8 $p\"}\n"
           L"要真 .xlsx 就把每行写成 $ws.Cells.Item($r,$c)=… 再 $wb.SaveAs(桌面路径)（需装 Office）。"
           L"比逐格输入快几十倍，也不会错行/覆盖。细则 lookupMacroAction(section=command)。"
           L"只有必须看着界面填（改既有表格的某个单元格等）才继续用界面操作。";
}

/// 本地窗口台账文本（EnumWindows Z 序，不烧图）；无宿主钩子时返回空串
std::wstring WindowLedgerText(AiActionHostHooks* hooks) {    if (!hooks || !hooks->onListWindows) return {};    return hooks->onListWindows();
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
        // 网页任务里 Ctrl+T/N 会新开空白标签，离开正在操作的空间/播放页。
        // ★原先这里**直接拒绝**（「已在网页标签内：禁止开新标签」，连 confirmShortcut 也不放行）。
        //   批 D 改成**执行 + 如实标注**（docs §47 / D-a）：模型要开标签就开，
        //   引擎只把它会造成的事实说清楚，要不要重发由模型自己判断。
        std::wstring extra;
        {
            const auto kind = AiLastPageKind();
            const bool onWeb = ((kind == L"dom" || kind == L"mixed")
                || AiWebBrowseSessionActive()) && AiForegroundIsBrowserWindow();
            const bool newBrowserTab = holdCtl && !holdAlt && !holdWin
                && (keyText == L"T" || keyText == L"N");
            if (onWeb && newBrowserTab) {
                extra += L"\n[事实] 前台是浏览器：Ctrl+"
                    + std::wstring(keyText == L"T" ? L"T" : L"N")
                    + L" 会新开一个标签页（当前页面留在原标签，不会丢）。";
            }
        }
        // ★Ctrl+A 清表护栏：宿主知道「本次任务已经用配方写进去多少组数据」。
        // 实测模型在配方写了几行后按 Ctrl+A + Delete「重来」，把已写好的表头/前两行整片删掉。
        // 想修单个错格不该全选；确实要整表重来才放行（confirmShortcut=true）。
        if (holdCtl && !holdAlt && !holdWin && keyText == L"A"
            && AiSession().recipeRowsWritten >= 2 && !confirmShortcut) {
            return L"[提示] 本表已用配方写入约 "
                + std::to_wstring(AiSession().recipeRowsWritten)
                + L" 组数据，Ctrl+A 会**全选整表**（紧接着的 Delete/输入就会把已写内容全部清掉，"
                  L"刚才那两行就是这么没的）。只修错格请不要全选：用方向键/名称框定位到那个单元格"
                  L"再 Delete。确实要清空整表重来，请重传并加 confirmShortcut=true。";
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
        if (const std::wstring tabGuard = GuardBareTabStreak(keyText,
                holdCtl || holdAlt || holdWin || hasShift); !tabGuard.empty()) {
            return tabGuard;
        }

        const bool isF2 = (keyText == L"F2");
        // 非 F2 清掉「保留选区」提示，避免误伤后续搜索框输入
        if (!isF2) g_keepInputSelectionAfterF2 = false;
        // 地址栏导航例外：Ctrl+L → quickInput(URL) → Enter 是打开网页的正常手法，
        // 不能当成「在站内搜索框里按 Enter」。判据是「刚输入的内容长得像 URL」
        // 且就发生在最近几步内（否则可能是模型乱按 Enter）。
        // 另外：只有前台确实是浏览器时这条网页守卫才生效——切到 Excel 另存为对话框后
        // 仍拿旧 pageKind 拦 Enter 是错的（实测被拦两次）。
        const bool urlNavPending = LooksLikeUrlInput(AiSession().lastQuickInputText)
            && AiSession().actionSeq - AiSession().lastQuickInputSeq <= 3;
        if ((AiPageKindIsDom() || AiSession().lastPageKind == L"mixed")
            && AiForegroundIsBrowserWindow()
            && !holdCtl && !holdAlt && !holdWin && !hasShift
            && (keyText == L"ENTER" || keyText == L"RETURN")
            && !urlNavPending) {
            // ★原先这里**直接拒绝**（「已有网页控件树：搜人/搜词用 searchOnPage(query)，
            //   勿 typeRef+Enter / keyClick(Enter)」）。批 D 改成执行 + 如实标注（docs §47）：
            //   焦点在不在搜索框是**运行时事实**，引擎的推断代替不了模型眼前看到的画面。
            extra += L"\n[事实] 前台是网页（pageKind=dom/mixed）：这一下 Enter 打给当前焦点控件"
                     L"（焦点常在搜索框或地址栏，取决于上一次点击）。";
        }
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
        // Midscene：提交钮探测为灰 —— ★原先这里**不执行** Enter（只回一个观察标记）。
        //   批 D 改成**执行 + 如实标注**（docs §47）：探测是上一步的事，模型可能知道得更多
        //   （按钮刚变可用／焦点其实在别处）。标注里保留「提交钮…不可用」这两个词，
        //   因为 `AiExecResultLooksUncertain` 靠它把这一击判成「结果不确定 → 必须看一帧」。
        if (!holdCtl && !holdAlt && !holdWin && !hasShift
            && (keyText == L"ENTER" || keyText == L"RETURN")
            && AiSession().submitUiBlocked) {
            extra += L"\n[事实] 上一步探测到提交钮不可用（灰）；这一下 Enter 已照发，"
                     L"能否生效取决于它现在是否变可用。";
        }
        // Enter/Escape 后若对话框已关，清 pending
        if (keyText == L"ENTER" || keyText == L"RETURN" || keyText == L"ESCAPE") {
            // 先执行，再按结果清（见下方）
        }
        params.erase("confirmShortcut");

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
                AiResetLocateMiss();
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
        return out + extra;
    };
    return tool;
}

/// 指针点击的上下文守卫（鼠标点击类工具共用；computer 别名层也必须走同一套，
/// 否则「网页禁止猜坐标 / 表格禁止点网格」这两条踩过坑的拦截会被绕过）。
/// 返回空串=放行。
std::wstring GuardPointerClickContext() {
    if ((AiPageKindIsDom() || AiLastPageKind() == L"mixed") && AiForegroundIsBrowserWindow()) {
        return L"[错误] 网页禁止用坐标猜点击。请 clickRef(ref)；"
               L"树上没有：observePage(query=短词) 或 locateAndClick。";
    }
    if (ForegroundIsSpreadsheetApp()) {
        return L"[错误] 前台是表格软件，禁止用坐标点网格（网格边线重复，极易点到错误行/中间空行）。"
               L"定位起点：keyClick(Ctrl+Home) 到 A1；需要视觉点选时 locateAndClick「列标A」/"
               L"「行号1」等有文字特征的目标，勿点空白单元格。"
               L"表头写完 Enter+Home 后直接填数据。确需点功能区非网格控件可改用 locateAndClick。";
    }
    return {};
}

AgentTool MakeMouseClickTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"mouseClick";
    tool.description =
        L"在当前 upload 截图像素坐标 (x,y) 点击。须落在截图宽高内；越界会被拒绝。"
        L"pageKind=dom/mixed 时禁止本工具，改 clickRef。按钮/图标请用 locateAndClick，禁止猜坐标。"
        L"输入框已聚焦时直接 quickInput。"
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
        // 共用守卫（computer 别名层走同一套）；表格场景可用 confirmSpreadsheetClick 显式放行
        std::wstring guard = GuardPointerClickContext();
        if (!guard.empty()) {
            if (ForegroundIsSpreadsheetApp() && confirmSheet) guard.clear();
            else return guard;
        }
        params["type"] = "mouseClick";
        bool observeAfter = false;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);
        json wrap = json::object();
        wrap["actions"] = json::array({ params });
        std::wstring note;
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs, &note);
        return FinishExecuteActions(hooks, merged, observeAfter) + note;
    };
    return tool;
}

AgentTool MakeComputerTool(AiActionHostHooks* hooks, bool allowAbs) {
    // computer-use 兼容层：模型侧（Anthropic/OpenAI/Gemini 以及挂了 computer-use provider 的
    // agent）越来越习惯单一 `computer` 工具 + action 枚举的调用形态。这里把它映射到我们既有
    // 动作上（mouseClick/moveMouse/quickInput/keyClick/scrollWheel/wait），
    // 于是：① 带 computer-use 先验的模型少犯错；② 动作**仍然落到可回放的宏动作**，
    // 录制/逻辑转化/脚本回放全部照旧（这是与直接接第三方 driver 的关键区别）。
    AgentTool tool;
    tool.name = L"computer";
    tool.description =
        L"【computer-use 兼容入口】action 描述一次桌面操作，内部映射到既有动作"
        L"（mouseClick/moveMouse/quickInput/keyClick/scrollWheel/wait），录制与脚本回放照旧。"
        // ★★ **坐标口径必须与清单/其它工具说同一句话**（2026-10-01 实测：同一轮里两种口径
        //   并存 ⇒ 模型在两种空间之间自选 ⇒ 系统性点偏、连点十几轮"没反应"）。
        //   统一为：**upload 截图像素**（与元素索引/OCR 清单、`mouseClick` 同一套）；
        //   0~1000 归一化**也收**（历史习惯），但要求整批统一，别在同一批里混用。
        L"★coordinate 与**清单/鼠标工具同一套**：**upload 截图像素**（见每帧清单标题的「图像尺寸 W×H」）。"
        L"给 0~1000 的归一化值也能收，但**整批统一一种口径**，别混用。"
        L"★screenshot 刷新观察帧（图片下一轮给你）；cursor_position 返回光标屏幕坐标。"
        L"★key 支持组合键（\"ctrl+s\"/\"Return\"）。type 在光标处输入，不替换；要替换先 key=ctrl+a。"
        L"网页按钮优先 clickRef；网页树可信时**不要**用 screenshot（纯浪费），"
        L"找不到坐标用 locateAndClick 而不是猜。本工具与 mouseClick/keyClick 等价，勿混用。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "action": {
                "type": "string",
                "enum": ["screenshot", "cursor_position", "mouse_move", "left_click",
                         "right_click", "middle_click", "double_click", "left_click_drag",
                         "type", "key", "hold_key", "scroll", "wait"],
                "description": "要执行的操作"
            },
            "coordinate": {
                "type": "array",
                "items": { "type": "number" },
                "minItems": 2,
                "maxItems": 2,
                "description": "[x,y]：0~1000 归一化（也接受像素）"
            },
            "coordinate2": {
                "type": "array",
                "items": { "type": "number" },
                "minItems": 2,
                "maxItems": 2,
                "description": "left_click_drag 的终点 [x,y]"
            },
            "text": { "type": "string", "description": "action=type 时要输入的正文" },
            "key": {
                "type": "string",
                "description": "action=key/hold_key 的按键或组合键，如 \"Return\" / \"ctrl+s\""
            },
            "scroll_direction": {
                "type": "string",
                "enum": ["up", "down", "left", "right"],
                "description": "action=scroll 的方向（默认 down）"
            },
            "scroll_amount": {
                "type": "integer",
                "minimum": 1,
                "maximum": 50,
                "description": "action=scroll 的步数（默认 3）"
            },
            "duration": {
                "type": "number",
                "description": "hold_key/wait 的秒数（默认 0.5 / 1.0）"
            },
            "observeAfter": { "type": "boolean" }
        },
        "required": ["action"]
    })";
    tool.execute = [hooks, allowAbs](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();
        std::wstring action;
        if (params.contains("action") && params["action"].is_string())
            action = Trim(FromUtf8(params["action"].get<std::string>()));
        for (auto& c : action)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (action.empty()) return L"[错误] computer 需要 action。";

        auto coord = [&](const char* keyName, double* outX, double* outY) -> bool {
            if (!params.contains(keyName)) return false;
            const auto& c = params[keyName];
            if (!c.is_array() || c.size() < 2) return false;
            auto one = [](const json& v, double* out) -> bool {
                if (v.is_number()) { *out = v.get<double>(); return true; }
                if (v.is_number_integer()) { *out = static_cast<double>(v.get<int>()); return true; }
                return false;
            };
            return one(c[0], outX) && one(c[1], outY);
        };
        auto numField = [&](const char* keyName, double fallback) -> double {
            if (!params.contains(keyName)) return fallback;
            const auto& v = params[keyName];
            if (v.is_number()) return v.get<double>();
            if (v.is_number_integer()) return static_cast<double>(v.get<int>());
            return fallback;
        };
        bool observeAfter = params.contains("observeAfter")
            ? ParseObserveAfterFlag(params) : false;

        auto runOne = [&](json act, bool observe) -> std::wstring {
            const std::wstring builtFull = BuildAndValidateMacroActionJson(act);
            if (builtFull.rfind(L"[错误]", 0) == 0) return builtFull;
            const std::wstring built = ExtractLeadingJsonArray(builtFull);
            if (built.empty()) return L"[错误] computer 动作构建异常。";
            std::wstring execMsg;
            if (hooks && hooks->onExecuteActions) {
                execMsg = hooks->onExecuteActions(built);
                if (execMsg.rfind(L"[错误]", 0) == 0) return execMsg;
            } else {
                return L"[错误] computer 需要 AI 动作执行宿主。";
            }
            ++AiSession().actionSeq;
            const wchar_t* marker = observe ? kExecutedObserveMarker : kExecutedSkipObserveMarker;
            return std::wstring(marker) + L"\n" + execMsg;
        };

        if (action == L"screenshot") {
            // ★★ 「手上已经有可信的 DOM 树，还要一张截图」= 纯浪费，必须挡住。
            //    实测（超星 50 题作业页那一局）：`computer(screenshot)` 调了 **21 次**，
            //    而 `observePage` 也调了 20 次 —— 同一时刻 DOM 树是可信的、树里就有目标
            //    （日志里 `[诊断] DOM 优先：树上命中… → clickRef，省整屏截图+识图` 反复出现）。
            //    单张图 22~96 KB，21 张 ≈ 1.7 MB 的上传量；更要紧的是它**每轮都在花时间**
            //    （截图 → 编码 → OCR 7000ms 级 → 上传），而模型拿到图后干的事和看树一样。
            //    所以：树可信 + 本轮没别的理由要图 ⇒ 拒绝截图，并**明确告诉它该用树里的什么**。
            //    ⚠ 只在 `dom`/`mixed` 且树可信时拦；canvas 页树是空的，截图是唯一手段，
            //      那种情况必须放行（否则模型真全盲）。
            if (AiPageKindIsDom() && AiPageTreeTrusted()) {
                return L"[拒绝] 当前页是网页且控件树可信（pageKind="
                    + (AiLastPageKind() == L"mixed" ? std::wstring(L"mixed") : std::wstring(L"dom"))
                    + L"），**不需要截图**：树里已有可点的 ref 与文字，截图只是重复信息。\n"
                    L"请改用：① 树上有目标 → clickRef/typeRef；② 知道文字但树里没有 → "
                    L"locateAndClick(target=\"该文字\")（本地识图，不额外上传整屏）；"
                    L"③ 要读正文/题目 → observePage（树上有 name）或 searchOnPage(query)。\n"
                    L"确实需要看画面（树为空/页面是画布/树与前台不符）时，先 observePage 重新抓树；"
                    L"若回执说明树不可信，再调 computer(screenshot)。";
            }
            // 刷新观察帧：返回 OBSERVE 标记，宿主会把新截图随下一轮给模型。
            // ★同时标记「模型明确要图」：宿主观察时**不许**用「界面未变」把这帧省掉。
            //   历史里的旧图会被剥成「(历史截图已省略)」，省掉这一帧 = 模型全盲，
            //   实测它会反复自问「我看不到图」并空转好几轮。
            NoteAiExplicitScreenshotRequest();
            std::wstring out = std::wstring(kExecutedObserveMarker)
                + L"\ncomputer(screenshot)：已刷新观察帧（图片随本轮观察给出）。"
                  L"坐标用 0~1000 归一化描述目标。";
            return out;
        }
        if (action == L"cursor_position") {
            POINT pt{};
            if (!GetCursorPos(&pt)) return L"[错误] 取光标位置失败。";
            return L"computer(cursor_position)：当前光标屏幕坐标 (" + std::to_wstring(pt.x)
                + L"," + std::to_wstring(pt.y) + L")。";
        }
        if (action == L"wait") {
            const double sec = std::clamp(numField("duration", 1.0), 0.05, 30.0);
            json act = json::object();
            act["type"] = "wait";
            act["duration"] = sec;
            return runOne(act, observeAfter);
        }
        if (action == L"mouse_move" || action == L"left_click" || action == L"right_click"
            || action == L"middle_click" || action == L"double_click") {
            double x = 0, y = 0;
            if (!coord("coordinate", &x, &y))
                return L"[错误] computer(" + action + L") 需要 coordinate=[x,y]（0~1000 归一化）。";
            // 复用 mouseClick 的上下文守卫（网页猜坐标 / 表格网格）——这条是踩过坑的
            if (const std::wstring guard = GuardPointerClickContext(); !guard.empty()) return guard;
            json act = json::object();
            if (action == L"mouse_move") {
                act["type"] = "moveMouse";
            } else {
                act["type"] = "mouseClick";
                if (action == L"right_click") act["button"] = "right";
                else if (action == L"middle_click") act["button"] = "middle";
                if (action == L"double_click") act["clickCount"] = 2;
            }
            act["x"] = static_cast<int>(std::lround(x));
            act["y"] = static_cast<int>(std::lround(y));
            return runOne(act, observeAfter);
        }
        if (action == L"left_click_drag") {
            double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            if (!coord("coordinate", &x1, &y1) || !coord("coordinate2", &x2, &y2)) {
                return L"[错误] computer(left_click_drag) 需要 coordinate=[起点] 与 "
                       L"coordinate2=[终点]（0~1000 归一化）。";
            }
            json act = json::object();
            act["type"] = "mouseDrag";
            // ★★字段名必须用**脚本 schema 的那一套**：`mouseDrag` 的起点是 `x/y`、
            //   终点是 `endX/endY`（`script_action_builder.cpp` 的字段表 / `script_io.cpp`
            //   只读这两个）。这里原先是 `fromX/fromY/toX/toY` —— 那是 `MakeMouseDragTool`
            //   的**工具入参**叫法，读进 `ScriptAction` 时**没有任何一处读它**
            //   ⇒ 拖动退化成 (0,0)→(0,0)，和上一轮 `mouseDown/mouseUp` 缺坐标是**同一个事故**，
            //   只是长在另一条入口上（`mouseDrag` 工具修了，`computer` 别名没修）。
            act["x"] = static_cast<int>(std::lround(x1));
            act["y"] = static_cast<int>(std::lround(y1));
            act["endX"] = static_cast<int>(std::lround(x2));
            act["endY"] = static_cast<int>(std::lround(y2));
            return runOne(act, observeAfter);
        }
        if (action == L"type") {
            std::wstring text;
            if (params.contains("text") && params["text"].is_string())
                text = FromUtf8(params["text"].get<std::string>());
            if (text.empty()) return L"[错误] computer(type) 需要 text。";
            json act = json::object();
            act["type"] = "quickInput";
            act["inputText"] = ToUtf8(text);
            // computer-use 语义是「在光标处输入」，不替换已有内容
            act["clearFirst"] = false;
            return runOne(act, observeAfter);
        }
        if (action == L"key" || action == L"hold_key") {
            std::wstring key;
            if (params.contains("key") && params["key"].is_string())
                key = Trim(FromUtf8(params["key"].get<std::string>()));
            if (key.empty()) return L"[错误] computer(" + action + L") 需要 key。";
            // 解析 "ctrl+shift+t" / "Return" 这类组合键
            bool ctrl = false, alt = false, shift = false, win = false;
            std::wstring mainKey = key;
            size_t pos = 0;
            std::vector<std::wstring> parts;
            while (pos <= key.size()) {
                const size_t plus = key.find(L'+', pos);
                parts.push_back(Trim(key.substr(pos,
                    plus == std::wstring::npos ? std::wstring::npos : plus - pos)));
                if (plus == std::wstring::npos) break;
                pos = plus + 1;
            }
            if (parts.size() > 1) {
                mainKey = parts.back();
                for (size_t i = 0; i + 1 < parts.size(); ++i) {
                    std::wstring m = parts[i];
                    for (auto& c : m)
                        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
                    if (m == L"ctrl" || m == L"control") ctrl = true;
                    else if (m == L"alt") alt = true;
                    else if (m == L"shift") shift = true;
                    else if (m == L"win" || m == L"meta" || m == L"cmd") win = true;
                }
            }
            if (mainKey.empty()) return L"[错误] computer(" + action + L") 的 key 无效。";
            if (action == L"hold_key") {
                const double sec = std::clamp(numField("duration", 0.5), 0.05, 10.0);
                json down = json::object();
                down["type"] = "keyDown";
                down["keyText"] = ToUtf8(mainKey);
                if (ctrl) down["holdLeftCtrl"] = true;
                if (alt) down["holdLeftAlt"] = true;
                if (shift) down["holdLeftShift"] = true;
                if (win) down["holdLeftWin"] = true;
                json waitAct = json::object();
                waitAct["type"] = "wait";
                waitAct["duration"] = sec;
                json up = json::object();
                up["type"] = "keyUp";
                up["keyText"] = ToUtf8(mainKey);
                json wrap = json::object();
                wrap["actions"] = json::array({ down, waitAct, up });
                const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
                if (merged.rfind(L"[错误]", 0) == 0) return merged;
                std::wstring execMsg;
                if (hooks && hooks->onExecuteActions) {
                    execMsg = hooks->onExecuteActions(merged);
                    if (execMsg.rfind(L"[错误]", 0) == 0) return execMsg;
                    ++AiSession().actionSeq;
                } else {
                    return L"[错误] computer 需要 AI 动作执行宿主。";
                }
                // ★这条分支原本直接返回，**绕过了 FinishExecuteActions 的三条守卫**
                //   （不确定结果必须看 / 本任务第一次动手必须看）。computer 是模型
                //   最常用的别名入口（hold_key、组合动作都走这里），绕过守卫等于
                //   让最常用的一条路没有 act→observe 保护。
                const bool needObserve = AiDecideNeedObserveAfterExec(
                    observeAfter, false, execMsg, std::wstring());
                return std::wstring(needObserve ? kExecutedObserveMarker
                                                : kExecutedSkipObserveMarker)
                    + L"\ncomputer(hold_key " + key + L" " + std::to_wstring(sec)
                    + L"s)：已按住并松开。" + execMsg;
            }
            json act = json::object();
            act["type"] = "keyClick";
            act["keyText"] = ToUtf8(mainKey);
            if (ctrl) act["holdLeftCtrl"] = true;
            if (alt) act["holdLeftAlt"] = true;
            if (shift) act["holdLeftShift"] = true;
            if (win) act["holdLeftWin"] = true;
            return runOne(act, observeAfter);
        }
        if (action == L"scroll") {
            std::wstring dir = L"down";
            if (params.contains("scroll_direction") && params["scroll_direction"].is_string()) {
                dir = FromUtf8(params["scroll_direction"].get<std::string>());
                for (auto& c : dir)
                    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
            }
            const int steps = static_cast<int>(std::clamp(numField("scroll_amount", 3.0), 1.0, 50.0));
            json act = json::object();
            act["type"] = "scrollWheel";
            const bool vertical = (dir == L"up" || dir == L"down");
            act["scrollVertical"] = vertical;
            act["scrollHorizontal"] = !vertical;
            act["scrollDirection"] = (dir == L"up" || dir == L"left") ? 0 : 1;
            act["scrollSteps"] = steps;
            double x = 0, y = 0;
            if (coord("coordinate", &x, &y)) {
                act["x"] = static_cast<int>(std::lround(x));
                act["y"] = static_cast<int>(std::lround(y));
            }
            return runOne(act, observeAfter);
        }
        return L"[错误] computer 不支持的 action：" + action
            + L"（可用：screenshot/cursor_position/mouse_move/left_click/right_click/"
              L"middle_click/double_click/left_click_drag/type/key/hold_key/scroll/wait）。";
    };
    return tool;
}

AgentTool MakeMoveMouseTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool = MakeAtomicMacroTool(
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
    return tool;
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
            // ★★这里**必须带 x/y**（实测事故）：`mouseDown` 的校验要求 x/y，
            //   不带的话整条拖拽会被拒，而且报的是一句**完全不搭界**的话
            //   （「第 2 个 mouseDown 缺少 x/y。点击输入框须给截图坐标…」）——
            //   模型于是以为自己「点输入框」的方式错了，改去别的地方瞎试。
            //   实测：`mouseDrag` 因此**从来没成功过一次**。
            down["x"] = fx;
            down["y"] = fy;
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
            up["x"] = tx;
            up["y"] = ty;
            // 终点另写一份 endX/endY：宿主把「移动+按下+移动+松开」合并成**单个**
            // `mouseDrag` 动作时，终点取自这两个字段；缺了就会合并成 (0,0)→(0,0)
            // （实测日志里那行 `鼠标拖拽左键 (0,0)→(0,0)` 就是这么来的）。
            up["endX"] = tx;
            up["endY"] = ty;
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
            return std::wstring(kExecutedSkipObserveMarker)
                + L"\n已跳过短 wait：宿主打开/点击后已自动等界面稳定。"
                  L"请直接 searchOnPage 或 observePage。画面仍在转圈/白屏时加 confirmWait=true。";
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

        std::wstring extra;
        if (preset == 9) {
            // ★原先这里**直接拒绝**（「禁止 hotkeyShortcut(9) 一按即松 Alt+Tab」）。
            //   批 D 改成执行 + 如实标注（docs §47）：模型要按就按 ——
            //   「不知道当前选中项」是**事实**，照实说清，由模型自己决定要不要改用别的手段。
            extra += L"\n[事实] hotkeyShortcut(9) = Alt+Tab 一按即松：系统会切到**最近使用过的**"
                     L"上一个窗口（引擎不指定目标）。要看预览再落地用 switchWindow。";
        }
        if (preset == 6 || preset == 10) {
            const std::wstring what = (preset == 6) ? L"Win+D 显示桌面" : L"Win+Down 最小化";
            if (const std::wstring guard = GuardHideWindowsAction(what); !guard.empty())
                return guard;
            // ★原先 preset==6 时还有一条**读任务备忘做文本匹配**的拒绝
            //   （备忘里同时出现「新建/创建」与「Excel/表格/文档/Word/PPT」⇒ 回
            //   「新建办公文档请用 runProgram(应用名)，勿 Win+D + 桌面右键」）。
            //   已删（批 D，docs §47）：§36.1 明令禁止的判据形态（措辞一改就静默失效），
            //   而且它是引擎替模型否决一个已经发出的按键。
        }
        params["type"] = "hotkeyShortcut";
        bool observeAfter = false;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);
        json wrap = json::object();
        wrap["actions"] = json::array({ params });
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        return FinishExecuteActions(hooks, merged, observeAfter) + extra;
    };
    return tool;
}

AgentTool MakeSwitchWindowTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"switchWindow";
    tool.description =
        L"【兜底】Alt+Tab 预览切窗。★首选 activateWindow（本地激活，一步到位不烧图）；"
        L"本工具只在 activateWindow 反复失败时用，要 force=true。"
        L"流程：openPreview（Alt按住+Tab，保持Alt）→ 按蓝框与目标窗的格数差 "
        L"move(steps,direction,confirm=true) 一步收尾。"
        L"★不松开 Alt 不会真的切过去；confirm=true 就是移完立刻松手，省一轮也避免预览挡屏。"
        L"找不到：move 多翻几格再看；仍无则 cancel，改 activateWindow(match=关键词)。"
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
        // ★原先这里有一条「刚 runProgram 启动的窗口会自己到前台，此时不许开预览」的**拒绝**
        //   （判据 = 跨帧的 `lastLaunchTick` 时间戳 + `launchSwitchWarned` 只劝一次）。
        //   已删（批 D，docs §47）：程序起没起来、在不在前台，模型看一眼画面就知道，
        //   引擎拿一个 8 秒窗口去否决它要的动作属于 §38② 的「用上一次的值做判据」。
        //   `lastLaunchTick` / `launchSwitchWarned` 两个字段一并删除。
        // ★模态对话框挡着时 Alt+Tab 也切不走（系统不让别的窗口盖过模态框），
        // 直接说清「先处理这个框」，别让模型在「切窗 → 失败 → 再切」上打转
        //（实测：另存为框弹出来后 activateWindow 连续失败 → Alt+Tab → 又 F12 乱试）。
        {
            const std::wstring dlgKind = ProbeForegroundDialogKind(hooks, nullptr);
            if (!dlgKind.empty() && dlgKind != L"none"
                && (opensPreview || dlgKind == L"saveAs" || dlgKind == L"saveAsMini"
                    || dlgKind == L"openFile" || dlgKind == L"confirmOverwrite")) {
                std::wstring advice = L"[错误] 前台正被一个模态对话框挡着（kind=" + dlgKind
                    + L"），模态框存在时切窗系统会拒绝（Alt+Tab 也切不走）。"
                      L"先把它处理掉：";
                if (dlgKind == L"saveAs" || dlgKind == L"saveAsMini") {
                    advice += L"quickInput 文件名/完整路径 → keyClick(Enter)（或 locateAndClick(保存)）；"
                              L"放弃才 Escape。处理完再继续任务。";
                } else if (dlgKind == L"openFile") {
                    advice += L"quickInput 文件名 → Enter；或 Escape 取消后改用 openFile(路径)。";
                } else if (dlgKind == L"confirmOverwrite") {
                    advice += L"看图选「是/替换」或「否/取消」（locateAndClick）。";
                } else {
                    advice += L"看图点确定/取消，或 Escape 取消。";
                }
                advice += L"\n不要 activateWindow / switchWindow / F12 碰运气。";
                return advice;
            }
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

AgentTool MakeReadDocumentToolImpl() {
    AgentTool tool;
    tool.name = L"readDocument";
    tool.description =
        L"读取本地办公文档正文（Excel/Word/PPT/PDF/CSV/文本），像 Codex 一样「直接读懂文件」。"
        L"★支持：xlsx/xlsm/xls/xlsb、csv/tsv、docx/doc、pptx/ppt、pdf、txt/md/json/log/xml/ini/sql。"
        L"★★优先用它**读**，不要为了读内容去 openFile + 截图 + 识图抄（那是几十倍 token 且会抄错）："
        L"表格返回「制表符分隔、一行一记录」（可直接配 runActionRecipe/CSV 用），文档返回段落文本。"
        L"★PDF：有文本层就返回文字；扫描件/纯图 PDF 会自动把前几页**渲染成图片**返回，"
        L"宿主会把图片发给你看（多模态），你直接读图即可，不要再让用户截图。"
        L"★装了 Office 走 COM（值最准），没装走 OOXML 直读；都不行时报错会给出替代路线。"
        L"写文件/生成表格用 runCommand（见 lookupMacroAction(section=office)）。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "path": {
                "type": "string",
                "minLength": 1,
                "description": "文档完整路径（桌面用 resolveSystemPath(folder=desktop) 拼）"
            },
            "maxChars": {
                "type": "integer",
                "minimum": 200,
                "maximum": 60000,
                "description": "正文上限，默认 6000；超出会截断并标注"
            },
            "pages": {
                "type": "integer",
                "minimum": 1,
                "maximum": 20,
                "description": "多页文档（PPT/PDF）最多读/渲染多少页，默认 3"
            }
        },
        "required": ["path"]
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();
        std::wstring path;
        if (params.contains("path") && params["path"].is_string())
            path = Trim(FromUtf8(params["path"].get<std::string>()));
        if (path.empty()) return L"[错误] readDocument 需要 path。";
        int maxChars = 6000;
        if (params.contains("maxChars") && params["maxChars"].is_number_integer())
            maxChars = std::clamp(params["maxChars"].get<int>(), 200, 60000);
        int pages = 3;
        if (params.contains("pages") && params["pages"].is_number_integer())
            pages = std::clamp(params["pages"].get<int>(), 1, 20);

        const OfficeDocResult r = ReadOfficeDocument(path, maxChars, pages);
        if (!r.ok) return L"[错误] " + r.error;
        std::wstring leaf = path;
        if (const size_t slash = leaf.find_last_of(L"\\/"); slash != std::wstring::npos)
            leaf = leaf.substr(slash + 1);
        std::wstring out = L"已读取 " + OfficeDocFormatLabel(r.format) + L"「" + leaf
            + L"」（引擎 " + r.engine + L"）";
        if (r.truncated) out += L"；正文已截断（maxChars=" + std::to_wstring(maxChars) + L"）";
        if (!r.note.empty()) out += L"\n" + r.note;
        if (!r.text.empty()) out += L"\n---- 正文 ----\n" + r.text;
        if (!r.images.empty()) {
            out += L"\n---- 页面图片（宿主已附图，直接读图）----";
            for (const auto& img : r.images) out += L"\n[[AGENT_IMG:" + img + L"]]";
        }
        if (r.text.empty() && r.images.empty()) {
            out += L"\n（没读到内容：文件可能是空白的，或内容全是图片。可 openFile 打开后用视觉读取。）";
        }
        AppendAiTaskMemoLine(L"did: readDocument " + r.format);
        return out;
    };
    return tool;
}

// ★★★ `readBrowserHistory` —— 「统计最近 N 条浏览记录」这类办公任务的**正路**。
//
// 为什么必须有这个工具（实测数据，不是推测）：
//   走 UI 路线（observePage → 失败 → listUiControls → zoom 读被省略号截断的标题 → 抄）
//   实测要 **8+ 轮往返、多次截图上传、23.6 秒**，而且 `observePage` 在 `edge://history`
//   上**必然失败** —— Chrome/Edge 的 `edge://` / `chrome://` 内置页不允许扩展注入
//   content script，重试没有意义。模型撞墙后会转去 zoom 烧 token，最后还是抄错。
//   本工具 **1 次调用**就返回完整标题 + 完整 URL + 精确时间（本地时间字符串），没有截断。
//
// 为什么自己做解析而不调 sqlite3.exe / Python：
//   用户的纪律 —— **产品不依赖别人的软件运行**（LESSONS.md §18）。
//   格式解析在 `src/sqlite/sqlite_read.*`，只读、零依赖。
AgentTool MakeReadBrowserHistoryTool() {
    AgentTool tool;
    tool.name = L"readBrowserHistory";
    tool.description =
        L"直接读 Edge / Chrome 的**本地历史记录与书签**（不用开浏览器、不用截图、不用识图）。"
        L"★★★ 凡是「统计/整理/导出浏览记录、最近访问过什么、查某网址访问时间、看书签」"
        L"这类任务，**第一个就该用它**，绝对不要去 edge://history 页面上截图抄："
        L"① 那条路 Chrome/Edge 禁止扩展注入（observePage 必然失败，重试无用）；"
        L"② 内置页上的列表内容截图必然被截断，标题会变成省略号，抄出来是错的；"
        L"③ 实测 UI 路线要 8+ 轮、23.6 秒，本工具 1 轮、亚秒级。"
        L"★返回格式：制表符分隔、一行一条（可直接喂给 runCommand 生成 xlsx/csv）。"
        L"★返回的是**完整**标题与完整 URL，没有省略号 —— 不要再 zoom / 截图「核对」。"
        L"★浏览器开着也能读（按只读共享方式打开，不需要先关浏览器）。"
        L"不确定有哪些浏览器/profile 时，先 kind=\"browsers\" 列一下。"
        L"★只读，绝不写浏览器库。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "kind": {
                "type": "string",
                "enum": ["history", "bookmarks", "browsers"],
                "description": "history=浏览记录（默认）；bookmarks=书签；browsers=只列出探测到的浏览器与 profile（不读数据）"
            },
            "browser": {
                "type": "string",
                "description": "edge / chrome；留空=自动挑第一个有数据的"
            },
            "profile": {
                "type": "string",
                "description": "如 Default / Profile 1；留空=Default，没有就用第一个"
            },
            "limit": {
                "type": "integer",
                "minimum": 1,
                "maximum": 500,
                "description": "最多返回多少条（按访问时间倒序），默认 20。★用户要「最近 10 条」就填 10，别填大再自己截"
            }
        },
        "required": []
    })";
    tool.execute = [](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();

        std::wstring kind = L"history";
        if (params.contains("kind") && params["kind"].is_string())
            kind = Trim(FromUtf8(params["kind"].get<std::string>()));
        std::wstring browser;
        if (params.contains("browser") && params["browser"].is_string())
            browser = Trim(FromUtf8(params["browser"].get<std::string>()));
        std::wstring profile;
        if (params.contains("profile") && params["profile"].is_string())
            profile = Trim(FromUtf8(params["profile"].get<std::string>()));
        int limit = 20;
        if (params.contains("limit") && params["limit"].is_number_integer())
            limit = std::clamp(params["limit"].get<int>(), 1, 500);

        namespace bh = browser_history;

        if (kind == L"browsers") {
            const std::vector<bh::BrowserInfo> list = bh::ListBrowsers();
            if (list.empty()) {
                return
                    L"[结果] 没找到 Edge / Chrome 的数据目录。\n"
                    L"我找过：%LOCALAPPDATA%\\Microsoft\\Edge\\User Data 与 "
                    L"%LOCALAPPDATA%\\Google\\Chrome\\User Data。\n"
                    L"如果用户用的是 Firefox / 360 / QQ 浏览器，本工具不支持（库格式不同）。\n"
                    L"替代路线：浏览器已打开的话，用 listUiControls(typeFilter=ListItem) "
                    L"读当前页面上的列表项名字（别截图，列表截图会被截断）。";
            }
            std::wstring out = L"[结果] 探测到 " + std::to_wstring(list.size()) + L" 个浏览器 profile：\n";
            out += L"浏览器\tProfile\t有历史\t有书签\t路径\n";
            for (const auto& bi : list) {
                out += bi.name + L"\t" + bi.profile + L"\t"
                    + (bi.hasHistory ? L"是" : L"否") + L"\t"
                    + (bi.hasBookmarks ? L"是" : L"否") + L"\t"
                    + (bi.hasHistory ? bi.historyDb : bi.bookmarkDb) + L"\n";
            }
            return out;
        }

        bh::ReadResult r;
        const bool ok = (kind == L"bookmarks")
            ? bh::ReadBookmarks(browser, profile, limit, r)
            : bh::ReadHistory(browser, profile, limit, r);
        if (!ok) return L"[错误] " + r.error;

        std::wstring out = bh::FormatReadResult(r);
        AppendAiTaskMemoLine(kind == L"bookmarks" ? L"did: readBrowserHistory(bookmarks)"
                                                  : L"did: readBrowserHistory(history)");
        return out;
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
        L"★切窗首选：按标题/进程名子串、**或按进程号(pid)**把已打开的窗口切到前台。宿主本地激活，"
        L"不识图、不 Alt+Tab、不点任务栏。最小化会自动还原。"
        L"★多候选时拒绝自动切换并列出清单——**这时别反复改 match 硬猜**："
        L"照 listWindows 台账里那一行的 `pid=…` 再调一次 activateWindow(pid=12345)，"
        L"pid 是精确的；双开同名窗口、多标签 Edge 只有它能唯一锁定。"
        L"只用 match 时必须写到能唯一锁定标题关键词（如「历史记录」「哔哩哔哩」），"
        L"禁止只写 edge/excel/msedge。"
        L"默认 observeAfter=false（跨窗抄录不烧图）；需要验收时再传 true。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "match": {
                "type": "string",
                "description": "标题关键词优先，如 浏览记录 / 历史记录；避免仅进程名"
            },
            "pid": {
                "type": "integer",
                "description": "进程号，用于双开/同名窗口消歧；取 listWindows 台账里的 pid=…。给了 pid 时 match 可省略"
            },
            "observeAfter": {
                "type": "boolean",
                "description": "默认 false；true=切窗后截屏验收"
            }
        },
        "anyOf": [{"required": ["match"]}, {"required": ["pid"]}]
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        if (!params.is_object()) params = json::object();
        std::wstring match;
        if (params.contains("match") && params["match"].is_string())
            match = Trim(FromUtf8(params["match"].get<std::string>()));
        // ★ pid（2026-10-02 加）：台账 `FormatWindowList` **本来就打印 `pid=…`**，
        //   双开同名窗口时还明确写着「⚠同类名同标题共N个（按 pid/客户区区分，别只按标题选）」
        //   ⇒ 模型照台账的指示用 pid 消歧，而这里只认 match ⇒ 被回一句「缺少 match」
        //   ⇒ 模型在「match 命中 2 个 / pid 不被接受」之间**反复绕圈**（真机日志：
        //     `activateWindow(pid=29404)` 连续被拒）。⇒ **产品叫模型用 pid，就必须真的收 pid。**
        //   容错：整数/无符号/浮点/字符串（模型偶尔写成 "29404"）都收；非法值按「没给」处理。
        unsigned long pid = 0;
        if (params.contains("pid")) {
            const auto& pv = params["pid"];
            long long v = -1;
            if (pv.is_number_integer()) v = pv.get<long long>();
            else if (pv.is_number_unsigned()) v = static_cast<long long>(pv.get<unsigned long long>());
            else if (pv.is_number_float()) v = static_cast<long long>(pv.get<double>());
            else if (pv.is_string()) {
                const std::wstring s = Trim(FromUtf8(pv.get<std::string>()));
                try { v = s.empty() ? -1 : std::stoll(s); } catch (...) { v = -1; }
            }
            if (v > 0 && v <= 0xFFFFFFFFLL) pid = static_cast<unsigned long>(v);
        }
        // ★★ **切窗口这件事软件已经替你做**（2026-09-30 实测：模型连续 11 轮发空参数的
        //   `activateWindow`，每轮只得到"match 不能为空"，白烧一轮里的一个工具位）。
        //   观察帧采集前会自动把任务窗口切到前台（`EnsureTaskWindowForeground()`）
        //   ⇒ 这里要**说清它不必做**，并给出"真要指定窗口时"怎么填，否则它会一直空转在这上面。
        if (match.empty() && pid == 0) {
            // ⚠ 仍以 `[错误]` 开头：空 match 就是一次**非法调用**（用例 `window_ledger_tools`
            //   钉住这个标记），而紧接着的指引才是真正要模型读到的东西（见上一段注释）。
            return L"[错误] activateWindow 缺少 match/pid（这次调用**没有切任何窗口**）。"
                   L"**不需要你切窗口**：软件在每次观察/截图前会自动把"
                   L"**任务所在的窗口**切到前台（网页桥接时就是扩展贴着的那个浏览器窗口）。"
                   L"你直接发动作即可。\n若确实要切到**别**的窗口，请给 match="
                   L"\"标题或进程名的一段\"（如 activateWindow(match=\"Edge\")），"
                   L"或给 pid（台账里的 `pid=…`，双开同名窗口时只有它靠得住）；"
                   L"不确定开着什么就先 listWindows。";
        }
        if (!hooks || !hooks->onActivateWindow) {
            return L"[错误] activateWindow 需要宿主钩子 onActivateWindow。";
        }
        bool observeAfter = false;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);
        // 只给 pid 时 match 为空 ⇒ 任务备忘用 pid 当标签（别记成空键）
        const std::wstring label =
            match.empty() ? (L"pid=" + std::to_wstring(pid)) : match;
        const std::wstring msg = hooks->onActivateWindow(match, pid);
        if (msg.rfind(L"[错误]", 0) == 0) return msg;
        ++AiSession().actionSeq;
        UpsertAiTaskMemoSection(L"windows", label, true);
        if (LooksLikeBrowserWindowTitle(msg)
            || msg.find(L"msedge.exe") != std::wstring::npos
            || msg.find(L"chrome.exe") != std::wstring::npos
            || msg.find(L"firefox.exe") != std::wstring::npos)
            AiNoteWebBrowseSession(true);
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
        L"★找不到路径时勿瞎猜：改用 openAppViaSearch(query=应用显示名) —— "
        L"它按显示名**纯本地解析**桌面/开始菜单快捷方式与 App Paths（不走 Win+S 搜索，"
        L"那条路会在浏览器里搜网页）。"
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

        // ★⏸ 这条**有意保留**（批 D 复核后的裁定，docs §47.7）：**拿不到用户同意就不做**。
        //   它不是「引擎替模型定任务策略」，而是**安全联锁**：
        //   ① `macro_execute_tools.h` 明写「RPA 直接 runProgram 不经过此钩子」⇒ 这道闸只影响
        //      「AI 刚读过网页之后要启动程序」这一条路径，**普通脚本一点不受影响**，
        //      所以不存在「把模型锁在外面」（它另有不受限的路径）；
        //   ② 网页正文是**不可信输入**，「读了网页 → 启动程序」正是提示注入的提权路径；
        //      **没有确认框可弹 ≠ 用户同意**，而是「问不到人」⇒ 安全默认必须 fail-safe。
        //      fail-open 会让「有确认框的环境」比「没有确认框的环境」更安全 —— 正好反了。
        if (AiWebFetchUntrustedActive()) {
            const std::wstring detail = L"目标：" + target;
            if (!hooks || !hooks->onConfirmWebLaunch) {
                return L"[错误] 刚抓取过网页：启动程序需用户在确认框中允许。"
                    L"直接 runProgram 做 RPA 不受影响（未抓网页时不会弹出）。";
            }
            if (!hooks->onConfirmWebLaunch(detail)) {
                return L"[错误] 用户拒绝从网页建议启动程序。";
            }
        }

        std::wstring resolved = ResolveProgramLaunchPath(target);
        // ★exe 名解析不出来时，按**显示名**再解析一次（桌面/开始菜单快捷方式、App Paths）。
        // 这条是通用能力：游戏/绿色软件的入口基本只有快捷方式，没有 exe 名可查
        //（真实事故：`runProgram("PlantsVsZombiesRH")` 找不到 → 退化成 Win+S 搜索 → 打开 Bing）。
        std::wstring launchSource;
        const bool resolvedIsFile = !resolved.empty()
            && GetFileAttributesW(resolved.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!resolvedIsFile || LooksLikeShellLaunchTarget(resolved)) {
            const AppLaunchTarget byName = ResolveAppLaunchTarget(target);
            if (byName.ok() && !resolvedIsFile) {
                resolved = byName.path;
                launchSource = byName.source;
            }
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
            return L"[错误] 找不到程序「" + target + L"」(解析为「" + resolved
                + L"」)。请改用 openAppViaSearch(query=应用显示名)（纯本地解析桌面/开始菜单"
                  L"快捷方式，不走搜索）；或 listWindows 后 activateWindow 复用已开窗口。";
        }

        const std::wstring rewritten = FromUtf8(params.dump());
        std::wstring out = inner ? inner(rewritten) : L"[错误] runProgram 未初始化。";
        if (out.rfind(L"[错误]", 0) != 0 && !LastLaunchProgramError().empty()) {
            // 宿主偶发未透传失败时，仍按真实 ShellExecute 结果纠偏
            out = L"[错误] " + LastLaunchProgramError();
        }
        if (out.rfind(L"[错误]", 0) != 0) {
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
                if (!launchSource.empty()) out += L"（来源 " + launchSource + L"）";
            }
            if (!list.empty()) {
                out += L"\n当前窗口（Z 序，#1 最靠前）：\n" + list
                    + L"\n切到其中任意一个用 activateWindow(match=...)；已经开着的东西别重复新建。";
            }
            // 打开了表格/文档软件：接下来多半是要往里写数据 → 按需给一次路线指针
            if (LooksLikeSpreadsheetOrDocTarget(resolved)) out += AiRouteNudgeOnce();
        } else if (out.find(L"openAppViaSearch") == std::wstring::npos) {
            const std::wstring q = NormalizeAppSearchQuery(target);
            out += L"\n兜底：openAppViaSearch(query=\"" + q
                + L"\") 按显示名做本地解析启动（桌面/开始菜单快捷方式）。";
        }
        return out;
    };
    return tool;
}

AgentTool MakeOpenAppViaSearchTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"openAppViaSearch";
    tool.description =
        L"★按**应用显示名**启动程序（如「植物大战僵尸融合版」「Microsoft Edge」「Excel」「微信」）。"
        L"实现是**纯本地解析**：桌面快捷方式 → 开始菜单快捷方式 → App Paths → PATH，"
        L"命中就用 ShellExecute 打开（.lnk 由 shell 解释）。"
        L"★不再使用 Win+S 搜索：Win10/11 的搜索默认带网页结果，输入中文名回车会**打开浏览器"
        L"搜网页**而不是启动程序（真实事故，白烧十几轮）。"
        L"不知道 exe 路径、runProgram 找不到文件时优先用本工具；找到就用 openFile/runProgram 直接开。"
        L"★已开同类窗口时默认拒绝——先 activateWindow 复用；确需新开 forceNew=true。"
        L"解析不到会**明确报未找到**并列出可用的近似快捷方式名，不会静默退化。默认 observeAfter=true。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "query": {
                "type": "string",
                "minLength": 1,
                "description": "应用显示名（开始菜单/桌面快捷方式上的名字），如 植物大战僵尸融合版 / Microsoft Edge / Excel / 微信"
            },
            "forceNew": {
                "type": "boolean",
                "description": "true=即使台账已有同类窗口也仍启动新实例；默认 false"
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
                L"有路径请用 runProgram/openFile。";
        }

        // ★⏸ 同 runProgram：**拿不到用户同意就不做**（安全联锁，批 D 有意保留，docs §47.7）
        if (AiWebFetchUntrustedActive()) {
            const std::wstring detail = L"启动程序：" + query;
            if (!hooks || !hooks->onConfirmWebLaunch) {
                return L"[错误] 刚抓取过网页：启动程序需用户在确认框中允许。"
                    L"直接 openAppViaSearch 做 RPA 不受影响（未抓网页时不会弹出）。";
            }
            if (!hooks->onConfirmWebLaunch(detail)) {
                return L"[错误] 用户拒绝从网页建议启动程序。";
            }
        }

        bool forceNew = false;
        if (params.contains("forceNew")) {
            const auto& f = params["forceNew"];
            if (f.is_boolean()) forceNew = f.get<bool>();
            else if (f.is_number_integer()) forceNew = f.get<int>() != 0;
        }

        const AppLaunchTarget target = ResolveAppLaunchTarget(query);
        if (!target.ok()) {
            // 明确失败：把「本机有哪些近似快捷方式」告诉模型，让它换名字重试，
            // 而不是退回搜索（搜索正是把游戏变成 Bing 网页的那条路）。
            std::wstring hint = L"[错误] 在本机桌面/开始菜单/App Paths 里找不到与「" + query
                + L"」匹配的应用。请换**快捷方式上的准确名字**再试，或用 listWindows "
                  L"activateWindow 复用已开窗口；也可 runCommand 列出桌面快捷方式名。";
            if (hooks) {
                const std::wstring list = WindowLedgerText(hooks);
                if (!list.empty()) hint += L"\n当前窗口（Z 序）：\n" + list;
            }
            return hint;
        }

        if (!forceNew) {
            const std::wstring list = WindowLedgerText(hooks);
            if (WindowLedgerLooksLikeApp(list, target.matchedName)
                || WindowLedgerLooksLikeApp(list, query)) {
                return WithWindowLedger(hooks,
                    L"[错误] 台账里已有与「" + target.matchedName
                    + L"」相关的窗口。请 activateWindow(match=标题关键词) 复用，"
                      L"勿再重复启动（会新开实例）。确需新开：forceNew=true。");
            }
        }

        bool observeAfter = true;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);

        // 解析到 .lnk / .exe → 走既有「打开文件」动作（ShellExecute），零键盘、零焦点依赖
        const std::wstring leaf = [&]() {
            const size_t p = target.path.find_last_of(L"\\/");
            return p == std::wstring::npos ? target.path : target.path.substr(p + 1);
        }();
        json actions = json::array();
        actions.push_back(json{
            {"type", "openFile"},
            {"targetPath", ToUtf8(target.path)}
        });
        json wrap = json::object();
        wrap["actions"] = actions;
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        std::wstring out = FinishExecuteActions(hooks, merged, observeAfter);
        if (out.rfind(L"[错误]", 0) != 0) {
            out += L"\n已启动「" + target.matchedName + L"」（解析来源 " + target.source
                + L"）：" + target.path;
            out += L"\n若未起窗：确认这就是你要的程序（同名快捷方式可能指向别的东西），"
                   L"或用 listWindows 看是否已起窗但标题不同。";
            // 进程名按 exe 名猜（.lnk 的目标名未知，不做二次猜测，交给下轮观察）
            if (hooks && hooks->onActivateByProcess && !LooksLikeShellLaunchTarget(target.path)) {
                const std::wstring act = hooks->onActivateByProcess(leaf);
                if (act.rfind(L"[错误]", 0) != 0) out += L"\n" + act;
            }
            const std::wstring list = WindowLedgerText(hooks);
            if (!list.empty()) {
                out += L"\n当前窗口（Z 序，#1 最靠前）：\n" + list;
            }
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
        L"followUp click/move：未匹配到则不点不移（与编辑器「找到后点击」相同）。"
        L"findTimeExpr 对 click/move/saveVar 以及有模板的 saveImage 生效（0=只找一次；-1=直到找到）。"
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
                "findTimeExpr": {
                    "type": "string",
                    "description": "等图时限秒；0=只找一次；-1=直到找到；saveImage 无模板时忽略"
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
        L"滚动鼠标滚轮以露出更多内容。"
        L"scrollDirection: 0/up/left 或 1/down/right；scrollSteps 默认 3。"
        L"pageKind=dom 时默认不截屏，滚完 observePage 刷新树。无截图时不要传 x/y。"
        L"默认 observeAfter=true（非网页）。";
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
        bool observeAfter = !AiPageKindIsDom();
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);
        // ★两条「别滚」的静默拒绝**已删（批 D，docs §47）**：
        //   ① `列表第1项已在树=eN。禁止滚动`（拿上一次快照推出来的「第1项」挡滚动）；
        //   ② `当前树可视区已有 N 项主内容…确认目标不在树里才传 confirmScroll=true`。
        //   两条都是拿**跨帧的「上一次」状态**替模型决定「该不该滚」，属 §38② 同类；
        //   滚动本身不破坏任何用户状态，模型自己看得见树里有什么。`confirmScroll` 入参一并删除。

        json actions = json::array();
        if (allowAbs && params.contains("x") && params.contains("y")
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
        wheel.erase("x");
        wheel.erase("y");
        actions.push_back(std::move(wheel));
        json wrap = json::object();
        wrap["actions"] = std::move(actions);
        std::wstring note;
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs, &note);
        std::wstring result = FinishExecuteActions(hooks, merged, observeAfter) + note;
        if (result.rfind(L"[错误]", 0) != 0) {
            AppendAiTaskMemoLine(L"did: scrollWheel");
            if (AiPageKindIsDom()) {
                result += L"\n[事实] 网页已滚动，控件树已过期。下一步 observePage 再 clickRef，"
                    L"勿 locateAndClick/mouseClick。";
            }
        }
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
        L"用系统默认浏览器打开 https 地址（ShellExecute）。"
        L"本任务已打开过**同一个 URL** 则跳过（回执里会说明）；force=true 强制重开。"
        L"地址看起来像站点接口（api./graphql. 等）时回执会如实标注，但仍照开。"
        L"白屏只 wait。打开后默认不截屏，下一轮 searchOnPage/observePage。"
        L"★targetPath 可以是完整网址、**常见站点名**（哔哩哔哩/B站、百度、知乎…）或裸域名"
        L"（www.bilibili.com 会自动补 https://）——**不要**因为不知道该填什么就省略参数。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "targetPath": {
                "type": "string",
                "minLength": 1,
                "description": "要打开的目标：完整网址（https://…）、**常见站点名**（哔哩哔哩/B站、百度、知乎、淘宝…）、或浏览器内置页（edge://history）。裸域名会自动补 https://"
            },
            "force": {
                "type": "boolean",
                "description": "true=即使本任务已开过同 URL 也再开；默认 false"
            },
            "observeAfter": {
                "type": "boolean",
                "description": "默认 false：打开后不截屏，省 token；需要画面再 true"
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
        // ★★ 通用解析（2026-09-28）：模型常把任务里的**自然表达**直接当参数用
        //   （「打开哔哩哔哩」⇒ 参数空着，或写「哔哩哔哩」）⇒ 这里统一解析成可打开的 URL。
        //   ⚠ 解析不出来就**如实报**并给出可用写法；**绝不**猜一个相近的站点打开
        //     （打开错的网站比不打开更糟）。
        if (url.empty()) {
            return L"[错误] openWebpage 需要 targetPath（要打开的目标）" + OpenWebpageTargetHint();
        }
        {
            const std::wstring resolved = ResolveOpenWebpageTargetUrl(url);
            if (resolved.empty()) {
                return L"[错误] 不认识要打开的目标「" + url + L"」" + OpenWebpageTargetHint();
            }
            url = resolved;
        }
        // ── 浏览器内置页（edge://history、chrome://downloads、about:…）──
        // 扩展的 tabs.update 不能导航 edge://chrome://（浏览器不允许），宏动作层也只放行
        // http/https。
        //
        // ★首选：让浏览器自己带 URL 打开（runProgram <browser> <url> 一条动作）。
        // 早期实现用地址栏合成键（Ctrl+L → quickInput → Enter），实测**首字母会被吞**：
        // "edge://history" 打成了 "dge://history" → 浏览器当搜索词 → 进了必应搜索页，
        // 模型却以为历史记录已打开（这是「本应输到地址栏却输到搜索栏」的直接原因）。
        // 合成键还依赖焦点/IME/时序；带 URL 启动没有这些变量，而且仍是可回放的 runProgram 动作。
        // 浏览器不在前台 / 识别不出浏览器时才退回地址栏合成键（并插入短等待缓解首字母吞字）。
        {
            const bool internalPage = IsBrowserInternalUrl(url);
            if (internalPage) {
                if (!hooks || !hooks->onExecuteActions) {
                    return L"[错误] openWebpage 打开内置页需要 AI 动作执行宿主。";
                }
                const std::wstring browser = DetectForegroundBrowserLaunchName();
                const std::wstring browserPath = browser.empty()
                    ? std::wstring() : ResolveProgramLaunchPath(browser);
                if (!browserPath.empty() && ProgramLaunchPathExists(browserPath)) {
                    json act = json::object();
                    act["type"] = "runProgram";
                    act["shortcutPreset"] = 0;
                    act["targetPath"] = ToUtf8(browserPath);
                    act["inputText"] = ToUtf8(url);
                    const std::wstring builtFull = BuildAndValidateMacroActionJson(act);
                    if (builtFull.rfind(L"[错误]", 0) == 0) return builtFull;
                    const std::wstring built = ExtractLeadingJsonArray(builtFull);
                    if (built.empty()) return L"[错误] 内置页启动动作构建异常。";
                    const std::wstring result = hooks->onExecuteActions(built);
                    if (result.rfind(L"[错误]", 0) == 0) return result;
                    AiNoteWebBrowseSession(true);
                    AiSession().lastOpenWebpageUrl = url;
                    AiSession().lastOpenWebpageSite.clear();
                    AiNoteInternalPagePendingVerify(url);
                    AppendAiTaskMemoLine(L"done: internal-page " + url);
                    return std::wstring(kExecutedSkipObserveMarker)
                        + L"\n已让浏览器打开内置页 " + url + L"（runProgram "
                        + browserPath + L" " + url
                        + L"，新开一个标签；不用合成键，不会把 URL 打成搜索词）\n" + result
                        + L"\n下一步：observePage 看该页控件树（内置页扩展看不到 DOM 时，"
                          L"用 listUiControls 或对截图 locateAndClick）。";
                }
                json sel = json::object();
                sel["type"] = "keyClick";
                sel["keyText"] = "L";
                sel["holdLeftCtrl"] = 1;
                sel["clickCount"] = 1;
                // ★地址栏刚聚焦就打字，首个字符极易被吞（实测 edge→dge）：
                // 插一个真等待（动作层 wait，不是工具层被跳过的短 wait）
                json pause = json::object();
                pause["type"] = "wait";
                pause["duration"] = 250;
                pause["clickCount"] = 1;
                json type = json::object();
                type["type"] = "quickInput";
                type["inputText"] = ToUtf8(url);
                type["clearFirst"] = true;
                type["clickCount"] = 1;
                json pause2 = json::object();
                pause2["type"] = "wait";
                pause2["duration"] = 180;
                pause2["clickCount"] = 1;
                json enter = json::object();
                enter["type"] = "keyClick";
                enter["keyText"] = "Enter";
                enter["clickCount"] = 1;
                json wrap = json::object();
                wrap["actions"] = json::array({ sel, pause, type, pause2, enter });
                const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
                const std::wstring result = FinishExecuteActions(hooks, merged, false);
                if (result.rfind(L"[错误]", 0) != 0) {
                    AiNoteWebBrowseSession(true);
                    AiSession().lastOpenWebpageUrl = url;
                    AiSession().lastOpenWebpageSite.clear();
                    // 地址栏路线依赖焦点/IME，可能静默失败 → 下一次 observePage 核对
                    AiNoteInternalPagePendingVerify(url);
                    AppendAiTaskMemoLine(L"done: internal-page " + url);
                }
                return std::wstring(kExecutedSkipObserveMarker)
                    + L"\n已用地址栏打开内置页 " + url
                    + L"（Ctrl+L → 等待 → 输入 → 等待 → Enter；扩展无法导航 "
                      L"edge://chrome://）\n" + result
                    + L"\n★务必核对是否真的到了该页：observePage 看 url/标题。"
                      L"若还停在旧页面（或标题变成「xxx - 搜索」，说明 URL 被打进搜索栏了），"
                      L"不要重复 Ctrl+L/Ctrl+H，改让浏览器自己打开："
                      L"runProgram(targetPath=\"edge\", inputText=\"" + url
                    + L"\")。";
            }
        }
        // ★这里原先有两条**站点专属**的静默拒绝（docs §47 / D1 点名的两处：
        //   「这是站点接口不是给人看的页面」与「不要打开 space.bilibili.com 空根路径」），
        //   第三条「不要猜用户数字 UID」的判据同样是「某站空间 URL 的形状」。
        //   前两条的判据写死了具体域名/URL 形状 ⇒ 换个网站就坏（D-c）。
        //   处置 = **放行 + 如实标注**（D-a：引擎可以说出它观察到什么，但不许拒绝执行）；
        //   地址像不像接口，是本机可观测的**事实**，照实说给模型听。
        //   （三条判据的函数名与详细叙述见 docs §47，源码注释里不留已删符号名。）
        std::wstring urlShapeNote;
        if (LooksLikeNonUserFacingWebUrl(url)) {
            urlShapeNote = L"\n[事实] 这个地址看起来是站点接口（api.*/graphql./gateway./.json 之类），"
                           L"不是给人看的页面；是否仍要打开由你决定（已照常打开）。";
        }

        bool force = false;
        if (params.contains("force")) {
            const auto& f = params["force"];
            if (f.is_boolean()) force = f.get<bool>();
            else if (f.is_number_integer()) force = f.get<int>() != 0;
        }
        const std::wstring& last = AiSession().lastOpenWebpageUrl;
        if (!force && !last.empty() && _wcsicmp(last.c_str(), url.c_str()) == 0) {
            AppendAiTaskMemoLine(L"skip: reopen " + url);
            return std::wstring(kExecutedSkipObserveMarker)
                + L"\n已跳过重复 openWebpage（本任务已打开过该 URL）。"
                L"请 searchOnPage 或 observePage 看当前页；确需刷新用 force=true。";
        }
        // ★「已在同一站点 ⇒ 禁止再编地址」的静默拒绝**已删（批 D，docs §47）**：
        //   它的两个豁免判据正是那两个**站点专属**启发式（搜索结果页 URL / 某站空间 URL）。
        //   D1 的口径是「没它就不知道该拦还是放行 ⇒ **放行**」（引擎不决策），
        //   所以整块删除，模型要开第二个地址就开（同 URL 去重仍然照旧保留，在上一段）。
        const std::wstring site = ExtractUrlSiteKey(url);

        params["type"] = "openWebpage";
        params.erase("force");
        bool observeAfter = false;
        if (params.contains("observeAfter"))
            observeAfter = ParseObserveAfterFlag(params);
        json wrap = json::object();
        wrap["actions"] = json::array({ params });
        const std::wstring merged = ValidateAndMergeActions(wrap, allowAbs);
        const std::wstring result = FinishExecuteActions(hooks, merged, observeAfter);
        if (result.rfind(L"[错误]", 0) != 0) {
            AiSession().lastOpenWebpageUrl = url;
            AiSession().lastOpenWebpageSite = site.empty() ? ExtractUrlSiteKey(url) : site;
            AiNoteWebBrowseSession(true);
            AppendAiTaskMemoLine(L"done: openWebpage");
            if (hooks && hooks->onObservePage) {
                const std::wstring tree = hooks->onObservePage(false, L"", L"", 0);
                if (!tree.empty() && tree.rfind(L"[错误]", 0) != 0
                    && tree.find(L"[canvas]") == std::wstring::npos) {
                    return result + urlShapeNote + L"\n" + tree
                        + L"\n请直接 searchOnPage(query)，勿 wait/observePage。";
                }
            }
        }
        return result + urlShapeNote;
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
        // ★★ **章节名当参数的别名**（2026-09-29，用户实测报障）。
        //
        //   实测：模型按 description 里的「goal=总目标；todos=待办子目标」直接写成
        //   `updateTaskMemo(goal="…", todos=["…"])` ⇒ 我们只认 `note` ⇒ 回
        //   `[错误] note 不能为空。` ⇒ **白烧一轮 API**，而它想表达的内容完全合法。
        //   修法：把每个固定章节名都当**别名**接住（`goal`/`facts`/`todos`/`windows`/
        //   `recipe`/`notes`），拼成带前缀的 note —— 复用既有的 `ParseMemoNoteChunks`
        //   路径，不另写一套写入逻辑（同一事实一份实现）。
        //   ⚠ 数组（`todos=[...]`）要展开成多行，模型常常一次给整张待办表。
        if (note.empty()) {
            static const wchar_t* kSections[] = {
                L"goal", L"facts", L"todos", L"windows", L"recipe", L"notes",
            };
            auto appendChunk = [&note](const wchar_t* sec, const std::wstring& v) {
                if (Trim(v).empty()) return;
                if (!note.empty()) note += L"\n";
                note += sec;
                note += L": ";
                note += v;
            };
            for (const wchar_t* sec : kSections) {
                // ⚠ `contains`/`operator[]` 收的是 `const char*`/`std::string`：wchar_t* 得自己转
                const std::string secKey = ToUtf8(sec);
                if (!params.contains(secKey)) continue;
                const json& v = params[secKey];
                if (v.is_string()) {
                    appendChunk(sec, FromUtf8(v.get<std::string>()));
                } else if (v.is_array()) {
                    for (const json& it : v) {
                        if (!it.is_string()) continue;
                        // 数组元素逐条展开：`todos: …` 每行一条，便于后续按行读
                        appendChunk(sec, FromUtf8(it.get<std::string>()));
                    }
                }
            }
        }
        while (!note.empty() && (note.front() == L' ' || note.front() == L'\t'
            || note.front() == L'\r' || note.front() == L'\n'))
            note.erase(note.begin());
        while (!note.empty() && (note.back() == L' ' || note.back() == L'\t'
            || note.back() == L'\r' || note.back() == L'\n'))
            note.pop_back();
        if (note.empty()) {
            return L"[错误] note 不能为空。也可以按章节直接给："
                   L"goal=\"总目标\"、todos=[\"步骤1\",\"步骤2\"]、facts=\"…\"、"
                   L"windows=\"…\"、recipe=\"…\"、notes=\"…\"。";
        }
        bool replaceAll = false;
        if (params.contains("replace")) {
            const auto& r = params["replace"];
            if (r.is_boolean()) replaceAll = r.get<bool>();
            else if (r.is_number_integer()) replaceAll = r.get<int>() != 0;
        }
        std::wstring sectionParam;
        if (params.contains("section") && params["section"].is_string()) {
            sectionParam = FromUtf8(params["section"].get<std::string>());
            for (auto& c : sectionParam) c = static_cast<wchar_t>(towlower(c));
            if (!IsMemoSectionName(sectionParam)) sectionParam.clear();
        }
        std::vector<std::pair<std::wstring, std::wstring>> chunks;
        if (sectionParam.empty()) {
            chunks = ParseMemoNoteChunks(note);
        } else {
            std::wstring body = note;
            std::wstring parsedSec, parsedBody;
            if (ParseMemoSectionLine(note, parsedSec, parsedBody) && parsedSec == sectionParam)
                body = parsedBody;
            if (body.empty()) body = note;
            chunks.emplace_back(sectionParam, body);
        }
        if (chunks.empty()) return L"[错误] note 不能为空。";
        if (replaceAll) {
            std::vector<std::wstring> stored;
            stored.reserve(chunks.size());
            for (const auto& c : chunks) {
                stored.push_back(MemoSectionTag(c.first) + c.second);
            }
            if (!SaveMemoLines(stored))
                return L"[错误] 备忘写入失败。";
            return L"备忘已清空并写入 [" + chunks.front().first + L"]";
        }
        for (const auto& c : chunks) {
            const bool replaceSec = MemoSectionIsSingleton(c.first);
            if (!UpsertAiTaskMemoSection(c.first, c.second, replaceSec))
                return L"[错误] 备忘写入失败。";
        }
        return L"备忘已更新 [" + chunks.front().first + L"]（共 "
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
        std::wstring out = L"已缓存数据块 [" + name + L"]（" + (replace ? L"覆盖" : L"追加")
            + L"），当前 " + std::to_wstring(saved.size()) + L" 字。后续直接 readTaskData(name="
            + name + L") 取用，勿再回数据源重新识图。";
        // ★最早、最强的「该走命令行」信号：模型刚把**成批表格数据**落到缓存，
        // 下一步必然是「写进 Excel/CSV」。此刻给一次自带命令的路线提示，
        // 就不必再走「开 Excel → 逐格 quickInput → Ctrl+S 另存为」那条又慢又容易卡的路。
        if (content.find(L'\n') != std::wstring::npos && content.find(L'|') != std::wstring::npos) {
            out += AiRouteNudgeOnce();
        }
        return out;
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

AgentTool MakeObservePageTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"observePage";
    tool.description =
        L"抓当前 Edge 标签的压缩可访问性树（Playwright 式 ref=eN）。"
        L"返回 pageKind=dom|mixed|canvas。canvas：禁止抓 HTML，改 locateAndClick。"
        L"dom/mixed：clickRef 点、typeRef 填。query=关键字只保留匹配项（大页用）。"
        L"★长列表页（作业/题库/搜索结果/表格）**默认一次给全**；回执开头会写"
        L"「本页共N个…已全部列出」或「本次给的是第a~b条…用 offset= 接着取」。"
        L"见到后者就用 observePage(offset=该值) 继续枚举，**不要靠反复滚动 + 截图去猜**"
        L"（滚动会换新树、旧 ref 作废，而且截图会截断文字）。"
        L"树上已有目标则直接 clickRef；树上没有或未装扩展则 locateAndClick 识图兜底。"
        L"勿每步都重抓。需配套扩展。force=true 从游戏壳回到平台页时重抓。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "force": {
                "type": "boolean",
                "description": "true=强制重抓（离开 canvas 回到网页时）"
            },
            "query": {
                "type": "string",
                "description": "可选，按控件名/栏目标题过滤（如 进入游戏、登录）"
            },
            "offset": {
                "type": "integer",
                "description": "可选，分页起点（0 起）。上一轮回执写了「用 offset=N 接着取」时填该 N。"
            }
        }
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        bool force = false;
        if (params.contains("force")) {
            const auto& f = params["force"];
            if (f.is_boolean()) force = f.get<bool>();
            else if (f.is_number_integer()) force = f.get<int>() != 0;
        }
        std::wstring query;
        if (params.contains("query") && params["query"].is_string())
            query = FromUtf8(params["query"].get<std::string>());
        query = SanitizeObservePageQuery(query);
        int offset = 0;
        if (params.contains("offset")) {
            const auto& o = params["offset"];
            if (o.is_number_integer()) offset = o.get<int>();
            else if (o.is_number_float()) offset = static_cast<int>(o.get<double>());
            else if (o.is_string()) {
                try {
                    offset = std::stoi(FromUtf8(o.get<std::string>()));
                } catch (...) {
                    offset = 0;
                }
            }
            if (offset < 0) offset = 0;
        }
        if (AiPageKindIsCanvas() && !force) {
            return L"[canvas] 当前页是画布（网页游戏/绘图/播放器），禁止再抓 HTML。"
                L"用视觉推进：locateAndClick(短目标) 点画面、keyClick/keyDown+keyUp 操作，"
                L"看不清就 screenshot。玩法细则 lookupMacroAction(section=game)。"
                L"若已回到登录/平台页，传 force=true。";
        }
        if (!hooks || !hooks->onObservePage) {
            return L"[错误] observePage 需要配套扩展（onObservePage）。"
                L"请在 Edge 加载 extension/edge 后重试；未安装则 locateAndClick 识图兜底。";
        }
        return hooks->onObservePage(force, L"", query, offset);
    };
    return tool;
}

AgentTool MakeClickRefTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"clickRef";
    tool.description =
        L"按 observePage 返回的 ref 点击（如 e1）。成功后返回新树，旧 ref 作废。"
        L"画布页（canvas）没有控件树，改用 locateAndClick。";
    // ★批 D（docs §47）删掉的两句（**能力已不存在，文案不能照旧教**）：
    //   ① 「用户主页等身份卡 href 会直接打开」—— 引擎不再按站点形状替模型直接导航（D2）；
    //   ② 「树上已标列表第1项时只能点该项内容卡，勿点后面的卡」—— 「列表第1项」这个概念
    //      本身已随 D1 删除，那条限制也不存在了。留着它就是**静态文案教模型用一个
    //      不存在的能力/限制**（§43 同形，也是本仓反复踩的「删了能力、留下文案」）。
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "ref": {
                "type": "string",
                "minLength": 2,
                "description": "observePage 的 ref，如 e1"
            },
            "doubleClick": {
                "type": "boolean",
                "description": "true=双击"
            }
        },
        "required": ["ref"]
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        std::wstring ref;
        if (params.contains("ref") && params["ref"].is_string())
            ref = FromUtf8(params["ref"].get<std::string>());
        ref = Trim(ref);
        if (!LooksLikePageSnapshotRef(ref))
            return L"[错误] clickRef 的 ref 无效（须为 e1、e2 …，来自最近一次 observePage）。";
        if (AiPageKindIsCanvas()) {
            // ★由「禁止」改成**事实句**（批 D，docs §47 / D6②）：画布页**没有控件树** ——
            //   这是客观不可用，不是引擎在禁止。措辞决定模型会不会以为「这个工具被禁了」。
            return L"[错误] 当前页是 canvas（画布/自绘画面）：observePage 拿不到控件树，"
                L"clickRef 没有可解析的 ref。可用 locateAndClick(target=短描述)/findImage/键鼠。";
        }
        bool doubleClick = false;
        if (params.contains("doubleClick")) {
            const auto& dc = params["doubleClick"];
            if (dc.is_boolean()) doubleClick = dc.get<bool>();
            else if (dc.is_number_integer()) doubleClick = dc.get<int>() != 0;
        }
        // ★两条「拿上一次状态拒绝点击」的闸**已删（批 D，docs §47）**：
        //   ① `刚点过 eN 且页面未变。不要连点同一 ref`（判据 = 跨帧的「刚点过的 ref」）；
        //   ② `列表第1项是 eN。请 clickRef(eN)，不要点 eX`（判据 = 上次快照推出来的「第1项」
        //      + 一个按 URL 形状判「这是不是播放页」的站点启发式）。
        //   两条都是 §38② 同类：用「上一次」的值替模型决定这一击该不该发。
        if (!hooks || !hooks->onClickRef) {
            return L"[错误] clickRef 需要配套扩展（onClickRef）。请先 observePage。";
        }
        const std::wstring urlBefore = AiLastPageUrl();
        const std::wstring msg = hooks->onClickRef(ref, doubleClick);
        if (msg.rfind(L"[错误]", 0) == 0) return msg;
        AiResetLocateMiss();
        ++AiSession().actionSeq;
        const std::wstring urlAfter = AiLastPageUrl();
        std::wstring out = std::wstring(
            AiPageKindIsDom() ? kExecutedSkipObserveMarker : kExecutedObserveMarker)
            + L"\n" + msg;
        if (!urlBefore.empty()
            && (PageUrlsSameDocument(urlBefore, urlAfter) || urlAfter == urlBefore)) {
            ++AiSession().samePageClickStreak;
            // ★原先 samePageClickStreak>=2 时这里既**拒绝**、又（命中站点启发式时）
            //   **从快照里按名字模糊匹配一个链接自己 onNavigatePage 过去** ——
            //   那是引擎替模型决定去哪（D2 最严重的一类），整块删除（批 D，docs §47）。
            //   保留的是**事实**：这一击之后 URL 没变 + 本次动作里连续几次没变。
            out += L"\n[事实] 这一击之后页面 URL 没有变化（仍在同一页）；本次动作里已连续 "
                + std::to_wstring(AiSession().samePageClickStreak)
                + L" 次点击后 URL 未变。页内按钮本来就不会换页。";
        } else {
            AiSession().samePageClickStreak = 0;
        }
        return out;
    };
    return tool;
}

AgentTool MakeTypeRefTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"typeRef";
    tool.description =
        L"按 observePage 的 ref 向输入框/下拉框填文本（登录框/评论框）。"
        L"默认 clearFirst=true。搜人/搜词请用 searchOnPage(query)，勿在搜索框 typeRef+Enter。"
        L"成功后返回新树，勿再 observePage。canvas 禁用。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "ref": {
                "type": "string",
                "minLength": 2,
                "description": "observePage 的 ref，如 e1"
            },
            "text": {
                "type": "string",
                "description": "要填入的文本"
            },
            "clearFirst": {
                "type": "boolean",
                "description": "true=先清空（默认 true）"
            },
            "submit": {
                "type": "boolean",
                "description": "true=填完后回车"
            }
        },
        "required": ["ref", "text"]
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        std::wstring ref;
        if (params.contains("ref") && params["ref"].is_string())
            ref = FromUtf8(params["ref"].get<std::string>());
        ref = Trim(ref);
        std::wstring text;
        if (params.contains("text") && params["text"].is_string())
            text = FromUtf8(params["text"].get<std::string>());
        if (!LooksLikePageSnapshotRef(ref))
            return L"[错误] typeRef 的 ref 无效（须为 e1、e2 …，来自最近一次 observePage）。";
        if (text.empty())
            return L"[错误] typeRef 的 text 不能为空。";
        if (AiPageKindIsCanvas()) {
            // 事实句（批 D，docs §47 / D6②）：画布页没有控件树 ⇒ 客观不可用，不是「禁止」
            return L"[错误] 当前页是 canvas（画布/自绘画面）：observePage 拿不到控件树，"
                L"typeRef 没有可解析的 ref。可用 locateAndClick + quickInput/键鼠。";
        }
        // ★两条站点专属的静默拒绝**已删（批 D，docs §47 / D1）**：
        //   ① `已在搜索结果页。禁止再 typeRef 搜索`（判据 = 搜索结果页 URL 形状）；
        //   ② `已在用户空间里搜过「X」（那是站内搜）`（判据 = 某站空间 URL + 跨帧的
        //      上一次搜索词）。判据里写死站点/页型 ⇒ 换个网站就坏（D-c）。
        //   （函数名与详细叙述见 docs §47，源码注释不留已删符号名。）
        if (!hooks || !hooks->onTypeRef) {
            return L"[错误] typeRef 需要配套扩展（onTypeRef）。请先 observePage。";
        }
        bool clearFirst = true;
        if (params.contains("clearFirst")) {
            const auto& c = params["clearFirst"];
            if (c.is_boolean()) clearFirst = c.get<bool>();
            else if (c.is_number_integer()) clearFirst = c.get<int>() != 0;
        }
        bool submit = false;
        if (params.contains("submit")) {
            const auto& s = params["submit"];
            if (s.is_boolean()) submit = s.get<bool>();
            else if (s.is_number_integer()) submit = s.get<int>() != 0;
        }
        const std::wstring urlBefore = AiLastPageUrl();
        const std::wstring msg = hooks->onTypeRef(ref, text, clearFirst, submit);
        if (msg.rfind(L"[错误]", 0) == 0) return msg;
        ++AiSession().actionSeq;
        AiSession().lastTypeSearchText = text;
        if (submit && hooks->onNavigatePage
            && LooksLikeSiteHomepageUrl(urlBefore)
            && !text.empty() && text.size() <= 40
            && text.find(L'@') == std::wstring::npos
            && text.find(L"http") == std::wstring::npos) {
            const std::wstring urlAfter = AiLastPageUrl();
            if (urlAfter.empty() || urlAfter == urlBefore) {
                const std::wstring searchUrl = BuildSiteSearchUrl(text,
                    urlAfter.empty() ? urlBefore : urlAfter);
                if (!searchUrl.empty()) {
                    const std::wstring nav = hooks->onNavigatePage(searchUrl, text);
                    if (nav.rfind(L"[错误]", 0) != 0) {
                        const std::wstring site = ExtractUrlSiteKey(searchUrl);
                        AiSession().lastOpenWebpageUrl = searchUrl;
                        AiSession().lastOpenWebpageSite = site.empty()
                            ? ExtractUrlSiteKey(searchUrl) : site;
                        const wchar_t* marker = AiPageKindIsDom()
                            ? kExecutedSkipObserveMarker : kExecutedObserveMarker;
                        return std::wstring(marker)
                            + L"\n已 typeRef 提交未跳转，已改用 searchOnPage 打开 "
                            + searchUrl + L"\n" + nav;
                    }
                }
            }
        }
        const wchar_t* marker = AiPageKindIsDom()
            ? kExecutedSkipObserveMarker : kExecutedObserveMarker;
        return std::wstring(marker) + L"\n" + msg;
    };
    return tool;
}

// ── 按控件名一次性填写（observePage(query) + typeRef 合成一步）────────
// 为什么需要它：typeRef 必须先 observePage 拿 ref，登录框/表单每填一格就要多烧一轮
// API。扩展侧 observePage(query) 已按「控件名命中」排序，宿主在这里直接挑输入类 ref
// 并 typeRef，把「观察→读树→填写」压成一次工具调用。树不可信时照旧报错让模型
// 回退 observePage + typeRef，不改变既有语义。
AgentTool MakeTypeByLabelTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"typeByLabel";
    tool.description =
        L"按控件名/标签一次性填入网页输入框（宿主内部 observePage(query)+typeRef，省一轮）。"
        L"适合登录框、表单字段（label 写「用户名」「密码」「手机号」等可见文字）。"
        L"宿主会挑输入类控件（textbox/searchbox），链接/按钮不会被误填。"
        L"搜人/搜词仍用 searchOnPage(query)，不要用本工具。"
        L"报错说树上没有时，改用 observePage(query=短词) 后 typeRef(ref)。需配套扩展。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "label": {
                "type": "string",
                "minLength": 1,
                "description": "输入框的名字/占位/标签短词，如「用户名」「密码」"
            },
            "text": {
                "type": "string",
                "description": "要填入的文本"
            },
            "clearFirst": {
                "type": "boolean",
                "description": "true=先清空（默认 true）"
            },
            "submit": {
                "type": "boolean",
                "description": "true=填完后回车"
            }
        },
        "required": ["label", "text"]
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        std::wstring label;
        if (params.contains("label") && params["label"].is_string())
            label = Trim(FromUtf8(params["label"].get<std::string>()));
        std::wstring text;
        if (params.contains("text") && params["text"].is_string())
            text = FromUtf8(params["text"].get<std::string>());
        if (label.empty()) return L"[错误] typeByLabel 的 label 不能为空。";
        if (text.empty()) return L"[错误] typeByLabel 的 text 不能为空。";
        if (AiPageKindIsCanvas()) {
            // 事实句（批 D，docs §47 / D6②）
            return L"[错误] 当前页是 canvas（画布/自绘画面）：observePage 拿不到控件树，"
                L"typeByLabel 没有可查的输入框。可用 locateAndClick + quickInput。";
        }
        // ★站点专属拒绝 `已在搜索结果页。请 clickRef 结果卡片（用户或稿件），不要再填搜索框`
        //   已删（批 D，docs §47 / D1）：判据 = 搜索结果页 URL 形状启发式。
        if (!hooks || !hooks->onObservePage || !hooks->onTypeRef) {
            return L"[错误] typeByLabel 需要配套扩展（onObservePage/onTypeRef）。"
                L"请先用 observePage(query=短词) 后 typeRef(ref)。";
        }
        const std::wstring tree = hooks->onObservePage(
            false, L"", PageSnapshotTargetKeyword(label), 0);
        if (tree.rfind(L"[错误]", 0) == 0) return tree;
        const PageSnapshot snap = AiLastPageSnapshot();
        const PageSnapshotPick pick = PickPageSnapshotRefForText(
            snap, label, PageSnapshotPickKind::Input);
        if (pick.ref.empty()) {
            return L"[错误] 树上没有像「" + label + L"」的输入框（候选 "
                + std::to_wstring(snap.nodes.size()) + L" 项）。"
                L"请 observePage(query=短词) 看控件名后 typeRef(ref)；"
                L"或先 locateAndClick 点中输入框再 quickInput。";
        }
        if (pick.ambiguous) {
            return L"[错误] 树上像「" + label + L"」的输入框有多个（候选 "
                + std::to_wstring(pick.candidates) + L" 项），"
                L"不敢替你选。请 observePage(query=" + label + L") 看 ref 后 typeRef(ref)。";
        }
        bool clearFirst = true;
        if (params.contains("clearFirst")) {
            const auto& c = params["clearFirst"];
            if (c.is_boolean()) clearFirst = c.get<bool>();
            else if (c.is_number_integer()) clearFirst = c.get<int>() != 0;
        }
        bool submit = false;
        if (params.contains("submit")) {
            const auto& s = params["submit"];
            if (s.is_boolean()) submit = s.get<bool>();
            else if (s.is_number_integer()) submit = s.get<int>() != 0;
        }
        const std::wstring msg = hooks->onTypeRef(pick.ref, text, clearFirst, submit);
        if (msg.rfind(L"[错误]", 0) == 0) {
            return msg + L"\n[提示] ref 可能已过期：改用 observePage(query=短词) 后 typeRef。";
        }
        ++AiSession().actionSeq;
        AiSession().lastTypeSearchText = text;
        const wchar_t* marker = AiPageKindIsDom()
            ? kExecutedSkipObserveMarker : kExecutedObserveMarker;
        const PageSnapshot after = AiLastPageSnapshot();
        std::wstring head = L"typeByLabel 已按控件树填入「" + Trim(pick.name) + L"」("
            + pick.ref + L"，DOM 精确填写，未截屏/未识图)";
        if (!after.url.empty() || !after.title.empty()) {
            head += L"\n新页面：" + (after.title.empty() ? L"(无标题)" : after.title);
            if (!after.url.empty()) head += L" | " + after.url;
        }
        return std::wstring(marker) + L"\n" + head;
    };
    return tool;
}

// ── UIA 控件台账 / 按编号触发（桌面侧「枚举 → 回 id → 校验」）──────────
// 对齐微软 UFO：把 UIA 控件编号给模型（纯文本，不烧图），模型回 (id, name)，
// 宿主按 name 重新定位并以 id 交叉校验——不一致时照样按 name 执行并明确警告。
// 这条路径完全不截屏、不调 VLM，桌面软件上比 locateAndClick 又快又准。
AgentTool MakeListUiControlsTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    (void)hooks;
    tool.name = L"listUiControls";
    tool.description =
        L"列出前台窗口的可交互控件（UIA 枚举，纯文本不烧图）：[编号] 类型 \"名字\" @坐标。"
        L"桌面软件先用它看清有哪些按钮/输入框/菜单项，再用 invokeUiControl(id, name) 精确触发，"
        L"比 locateAndClick 识图更快更准。网页请用 observePage/clickRef"
        L"（⚠ 但 edge:// / chrome:// 内置页扩展无权注入，只能走本工具的 UIA 路线）。"
        L"列表型内容（历史记录/书签/文件列表/消息列表）**优先用本工具** —— "
        L"界面上的标题常被省略号截断，UIA 名字才是完整的，别去 zoom 读截图。"
        L"长列表看不全时用 typeFilter/nameFilter 缩小，或先 scrollWheel 再重列。"
        L"枚举不到控件（游戏/自绘界面）时改用 locateAndClick。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "maxCount": {
                "type": "integer",
                "minimum": 1,
                "maximum": 120,
                "description": "最多列出多少个控件，默认 40"
            },
            "typeFilter": {
                "type": "string",
                "description": "可选：只列类型名包含该串的控件，如「列表项」「按钮」"
            },
            "nameFilter": {
                "type": "string",
                "description": "可选：只列名字包含该串的控件（大小写不敏感）"
            }
        }
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        int maxCount = 40;
        if (params.contains("maxCount") && params["maxCount"].is_number_integer())
            maxCount = std::clamp(params["maxCount"].get<int>(), 1, 120);
        std::wstring typeFilter, nameFilter;
        if (params.contains("typeFilter") && params["typeFilter"].is_string())
            typeFilter = FromUtf8(params["typeFilter"].get<std::string>());
        if (params.contains("nameFilter") && params["nameFilter"].is_string())
            nameFilter = FromUtf8(params["nameFilter"].get<std::string>());
        if (!hooks || !hooks->onListUiControls) {
            return L"[错误] listUiControls 需要 AI 动作执行宿主（UIA 枚举）。"
                L"请改用 locateAndClick 识图点击。";
        }
        return hooks->onListUiControls(maxCount, typeFilter, nameFilter);
    };
    return tool;
}

AgentTool MakeInvokeUiControlTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"invokeUiControl";
    tool.description =
        L"按 UIA 名字触发控件（需先 listUiControls）。宿主用 name 重新定位："
        L"有 InvokePattern 时直接触发（不打像素），否则点控件中心。"
        L"id 可省略（界面变化会让编号错位，不确定就别传；传了也只用于交叉校验，"
        L"不一致会按 name 执行并警告）。比 locateAndClick 省一轮识图。"
        L"网页里的按钮请用 clickRef。目标灰掉会被拦下。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "name": {
                "type": "string",
                "minLength": 1,
                "description": "listUiControls 里该控件的名字（必须与台账一致）"
            },
            "id": {
                "type": "integer",
                "minimum": 1,
                "description": "可选：listUiControls 里的编号，仅用于交叉校验"
            },
            "observeAfter": {
                "type": "boolean",
                "description": "默认 true：触发后截屏验收"
            }
        },
        "required": ["name"]
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        int id = 0;
        if (params.contains("id") && params["id"].is_number_integer())
            id = params["id"].get<int>();
        std::wstring name;
        if (params.contains("name") && params["name"].is_string())
            name = Trim(FromUtf8(params["name"].get<std::string>()));
        if (name.empty()) return L"[错误] invokeUiControl 的 name 不能为空。";
        if (!hooks || !hooks->onInvokeUiControl) {
            return L"[错误] invokeUiControl 需要 AI 动作执行宿主（UIA 触发）。"
                L"请改用 locateAndClick 识图点击。";
        }
        ++AiSession().actionSeq;
        const bool observeAfter = params.contains("observeAfter")
            ? ParseObserveAfterFlag(params) : true;
        const wchar_t* marker = observeAfter ? kExecutedObserveMarker : kExecutedSkipObserveMarker;
        const std::wstring body = hooks->onInvokeUiControl(id, name, observeAfter);
        if (body.rfind(L"[错误]", 0) == 0) return body;
        return std::wstring(marker) + L"\n" + body;
    };
    return tool;
}

// ── 命令行工具：一步到位的「非 GUI」路线 ─────────────────────────────
// 为什么需要：实测同一个「统计浏览器历史 → 写 Excel」任务，GUI 路线要
// 17 轮 API + 约 99 个合成按键（逐格 Tab/Enter 输入），而写文件/查数据本身
// 一条命令就够。这里把命令包装成既有的「运行程序」动作（RunProgram + args），
// 所以它天然可被逻辑转化/脚本回放复用，不引入新的回放语义。
AgentTool MakeRunCommandTool(AiActionHostHooks* hooks, bool allowAbs) {
    AgentTool tool;
    tool.name = L"runCommand";
    tool.description =
        L"执行一条命令行（内部走「运行程序」动作：程序路径 + 运行参数，可被逻辑转化固化回放）。"
        L"★适用范围：读写/生成文件（Excel/CSV/JSON/文本）、批量改名、统计与转码、查注册表"
        L"——凡是不需要「看见界面」的**本机**活，都用它一步做完，不要用 locateAndClick 逐格点。"
        L"shell=powershell(默认)/cmd。命令里写文件用绝对路径（桌面用 resolveSystemPath(\"desktop\")）。"
        L"★GUI 任务仍用定位/点击工具；本工具不弹窗、**不回显输出**。"
        L"⚠ 查资料请用 **webSearch → fetchWebPage**（两步都是宿主 HTTP）；"
        L"要**点击/登录**的交互网页才用 openWebpage+observePage（会抢前台）。"
        L"**不要**用本工具开浏览器或搜网页 —— 输出不回显，结果只会丢掉；"
        L"确实要用本工具取输出时，必须自己重定向到文件再用 readAgentFile 读。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "shell": {
                "type": "string",
                "enum": ["powershell", "cmd"],
                "description": "默认 powershell；cmd 用于老式批处理命令"
            },
            "command": {
                "type": "string",
                "minLength": 1,
                "description": "要执行的命令/脚本正文（不含 -Command 前缀）"
            },
            "waitMs": {
                "type": "integer",
                "minimum": 0,
                "maximum": 60000,
                "description": "启动后等待毫秒（默认 1500），给命令一点执行时间"
            },
            "hidden": {
                "type": "boolean",
                "description": "默认 true：后台隐藏窗口执行，不抢前台焦点"
            }
        },
        "required": ["command"]
    })";
    tool.execute = [hooks, allowAbs](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        std::wstring command;
        std::wstring shell = L"powershell";
        int waitMs = 1500;
        bool hidden = true;
        if (!parseError.empty()) {
            // ★容错：命令正文常含换行/引号/反斜杠，模型很容易给出非法 JSON
            //（实测连续两轮「JSON 解析失败」直接卡死任务）。这里退化成「自由文本」：
            // 从原始参数里把 command 抠出来，抠不到就把整段当命令。
            std::wstring raw = Trim(paramsJson);
            std::wstring salvaged;
            const size_t key = raw.find(L"\"command\"");
            if (key != std::wstring::npos) {
                size_t colon = raw.find(L':', key);
                if (colon != std::wstring::npos) {
                    std::wstring rest = Trim(raw.substr(colon + 1));
                    // 去掉包裹的引号/花括号；把转义还原成真字符
                    if (!rest.empty() && (rest.front() == L'"' || rest.front() == L'\''))
                        rest.erase(rest.begin());
                    while (!rest.empty() && (rest.back() == L'}' || rest.back() == L'"'
                        || rest.back() == L'\'' || rest.back() == L'\n' || rest.back() == L' '))
                        rest.pop_back();
                    std::wstring un;
                    un.reserve(rest.size());
                    for (size_t i = 0; i < rest.size(); ++i) {
                        if (rest[i] == L'\\' && i + 1 < rest.size()) {
                            const wchar_t n = rest[i + 1];
                            if (n == L'n') { un.push_back(L'\n'); ++i; continue; }
                            if (n == L't') { un.push_back(L'\t'); ++i; continue; }
                            if (n == L'r') { ++i; continue; }
                            if (n == L'"' || n == L'\\' || n == L'\'') {
                                un.push_back(n);
                                ++i;
                                continue;
                            }
                        }
                        un.push_back(rest[i]);
                    }
                    salvaged = Trim(un);
                }
            }
            if (salvaged.empty()) salvaged = raw;
            // 整段都不像命令（例如模型给了空对象）就还是报错
            if (salvaged.size() < 2 || salvaged == L"{}" || salvaged == L"null") {
                return L"[错误] runCommand 参数解析失败，且无法从正文中恢复命令。"
                    L"请重发：{\"command\":\"...\"}（命令里有换行/引号时务必转义）。";
            }
            command = salvaged;
            if (params.contains("shell")) params.erase("shell");
            params = json::object();
        } else {
            if (params.contains("command") && params["command"].is_string())
                command = FromUtf8(params["command"].get<std::string>());
            if (params.contains("shell") && params["shell"].is_string()) {
                const std::wstring s = FromUtf8(params["shell"].get<std::string>());
                if (s == L"cmd") shell = L"cmd";
            }
            if (params.contains("waitMs") && params["waitMs"].is_number_integer())
                waitMs = std::clamp(params["waitMs"].get<int>(), 0, 60000);
            if (params.contains("hidden")) {
                const auto& h = params["hidden"];
                if (h.is_boolean()) hidden = h.get<bool>();
                else if (h.is_number_integer()) hidden = h.get<int>() != 0;
            }
        }
        if (Trim(command).empty()) return L"[错误] runCommand 的 command 不能为空。";
        // 动作层：RunProgram + args。隐藏执行用 powershell 的 -WindowStyle Hidden /
        // cmd 的 start /min，避免命令窗口抢走前台焦点（AI 后续还要点界面）。
        const std::wstring program = (shell == L"cmd") ? L"cmd" : L"powershell";
        std::wstring args;
        if (shell == L"cmd") {
            args = L"/c " + command;
        } else {
            args = (hidden ? L"-NoProfile -NonInteractive -WindowStyle Hidden -Command "
                           : L"-NoProfile -Command ")
                + command;
        }
        json act = json::object();
        act["type"] = "runProgram";
        act["shortcutPreset"] = 0;    // 0=自选文件（targetPath 为准）
        // 注意：nlohmann 只认 UTF-8 std::string，赋 wstring 会变成整数数组
        act["targetPath"] = ToUtf8(program);
        // 运行参数在动作层是 inputText（见 action_reference：runProgram = targetPath + inputText）
        act["inputText"] = ToUtf8(args);
        const std::wstring builtFull = BuildAndValidateMacroActionJson(act);
        if (builtFull.rfind(L"[错误]", 0) == 0) return builtFull;
        // ★构建器会在数组后追加「[提示] 已自动…stopMacro…」，而提示自带 [ ]：
        // 引擎用裸 rfind(']') 取数组结尾会把提示里的 ] 一起吞掉 → 整段不是合法 JSON
        // →「JSON 解析失败」（实测：AI 最省事的命令行路线第一次调用就死在这里，
        // 然后被迫回去瞎点界面）。这里先只把数组正文交给宿主。
        const std::wstring built = ExtractLeadingJsonArray(builtFull);
        if (built.empty()) return L"[错误] 动作 JSON 构建异常（没有数组正文）。";
        std::wstring execMsg;
        if (hooks && hooks->onExecuteActions) {
            execMsg = hooks->onExecuteActions(built);
            if (execMsg.rfind(L"[错误]", 0) == 0) return execMsg;
        } else {
            return L"[错误] runCommand 需要 AI 动作执行宿主。";
        }
        if (waitMs > 0) Sleep(static_cast<DWORD>(waitMs));
        ++AiSession().actionSeq;
        AppendAiTaskMemoLine(L"did: runCommand(" + shell + L")");
        const wchar_t* marker = kExecutedObserveMarker;
        return std::wstring(marker) + L"\nrunCommand 已执行[" + shell + L"]：" + command
            + L"\n（宿主运行完即返回，不回显输出；需要结果请用命令自己重定向到文件，"
              L"再用 readAgentFile/readTaskData 读取）\n" + execMsg;
    };
    return tool;
}

AgentTool MakeSearchOnPageTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"searchOnPage";
    tool.description =
        L"当前站点全站搜索：宿主按当前页站点构造搜索 URL 并打开，返回新控件树。"
        L"站点不在内置模板表里时**明确失败**（不会退回搜索引擎）；"
        L"此时可 observePage 后 typeRef 搜索框。"
        L"搜索后若树上第一个名字含该词的结果能解析出地址，宿主会直接打开它（回执里写明）。"
        L"需配套扩展（onNavigatePage）。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "query": {
                "type": "string",
                "minLength": 1,
                "description": "搜索词，如 UP 主名字"
            }
        },
        "required": ["query"]
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        std::wstring query;
        if (params.contains("query") && params["query"].is_string())
            query = FromUtf8(params["query"].get<std::string>());
        query = Trim(query);
        if (query.empty())
            return L"[错误] searchOnPage 的 query 不能为空。";
        if (AiPageKindIsCanvas()) {
            // 事实句（批 D，docs §47 / D6②）
            return L"[错误] 当前页是 canvas（画布/自绘画面）：没有站点/控件树可用，"
                L"searchOnPage 构造不出搜索地址。可用 locateAndClick + quickInput/键鼠。";
        }
        const std::wstring current = !AiLastPageUrl().empty()
            ? AiLastPageUrl() : AiLastOpenWebpageUrl();
        if (current.empty()) {
            return L"[错误] 还不知道当前站点。请先 openWebpage 打开目标站首页，再 searchOnPage。";
        }
        // ★站点专属拒绝 `已在「X」的搜索结果页。请 clickRef 用户/视频，勿再搜索`
        //   已删（批 D，docs §47 / D1）：判据 = 搜索结果页 URL 形状 + 跨帧的上一次搜索词。
        //   要不要用同一个词再搜一次，模型自己判断。
        const std::wstring url = BuildSiteSearchUrl(query, current);
        if (url.empty()) {
            return L"[错误] 当前站点没有内置搜索模板。请 observePage 后 typeRef 搜索框；"
                L"已知站点（B站/百度/YouTube 等）会自动构造 search URL。";
        }
        if (!hooks || !hooks->onNavigatePage) {
            return L"[错误] searchOnPage 需要配套扩展（onNavigatePage）。"
                L"请在 Edge 加载 extension/edge 后重试；未安装则 locateAndClick 点搜索框后 quickInput。";
        }
        const std::wstring msg = hooks->onNavigatePage(url, query);
        if (msg.rfind(L"[错误]", 0) == 0) return msg;
        ++AiSession().actionSeq;
        AiSession().lastTypeSearchText = query;
        AiSession().lastOpenWebpageUrl = url;
        const std::wstring site = ExtractUrlSiteKey(url);
        AiSession().lastOpenWebpageSite = site.empty() ? ExtractUrlSiteKey(current) : site;
        const wchar_t* marker = AiPageKindIsDom()
            ? kExecutedSkipObserveMarker : kExecutedObserveMarker;
        const std::wstring match = AiQueryMatchingNavigationUrl(query);
        if (!match.empty() && !PageUrlsSameDocument(match, AiLastPageUrl())
            && hooks->onNavigatePage) {
            const std::wstring opened = hooks->onNavigatePage(match, L"");
            if (opened.rfind(L"[错误]", 0) != 0) {
                AiSession().lastOpenWebpageUrl = match;
                const std::wstring matchSite = ExtractUrlSiteKey(match);
                AiSession().lastOpenWebpageSite = matchSite.empty()
                    ? ExtractUrlSiteKey(match) : matchSite;
                AppendAiTaskMemoLine(L"did: searchOnPage-open " + match);
                AiLogicConvertNoteOpenWebpage(match);
                return std::wstring(marker)
                    + L"\n已 searchOnPage「" + query + L"」并打开匹配结果 " + match
                    + L"\n" + opened;
            }
        }
        AiLogicConvertNoteOpenWebpage(AiLastPageUrl().empty() ? url : AiLastPageUrl());
        return std::wstring(marker) + L"\n已 searchOnPage 打开 " + url + L"\n" + msg;
    };
    return tool;
}

AgentTool MakeLocateAndClickTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"locateAndClick";
    tool.description =
        L"点击屏幕上用自然语言描述的目标。★★宿主另开无历史识图会话（不带主 Agent 对话），"
        L"只传短 target + 当前截图——禁止把整段任务原文塞进 target。"
        L"target 上限 80 字，但**最佳长度是 2~10 字**：写屏幕上真实存在的文字/控件名"
        L"（「历史记录」「保存」「更多」），或「图标名+最短限定」（「右上角更多」）。"
        L"★把方位/颜色/形状/用途堆成一句话（如「历史记录面板右上角的三个点更多选项按钮」）"
        L"会同时降低两种命中率：VLM 被长句带偏、UIA 名称匹配变歧义——长描述不是更准，是更不准。"
        L"target≤80字：控件文案/颜色/大致位置即可。"
        L"★★选有特征的目标：按钮文字、图标、列标A/行号8、名称框「A1」等；"
        L"禁止以空白单元格/重复网格线为 target（等价于随机点表）。表格起点优先 keyClick(Ctrl+Home)。"
        L"宿主本地：截屏→粗定位→简单目标「紧凑即点」跳过二级（省一轮 API）→难目标自动 Zoom 精点→点击。"
        L"★打开桌面图标/文件必须 doubleClick=true（单击只会选中），或 button=right；"
        L"有路径优先 openFile。默认 refineLevels=1；难目标传 2。有模板优先 findImage。"
        L"网页有树时优先 clickRef；树上没有或未装扩展时用本工具识图兜底。"
        L"未找到：可换更短的描述再试（引擎不限次数）。细则 lookupMacroAction(section=composite)。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "target": {
                "type": "string",
                "minLength": 1,
                "description": "短描述：写屏幕上的文字/控件名（最准是 2~10 字，如「历史记录」「保存」）；方位/颜色只作最短限定，勿堆成长句"
            },
            "elementId": {
                "type": "integer",
                "minimum": 1,
                "description": "★直接用**本帧元素索引**里的编号点（清单每行开头的 [N]），比写 target 更确定：同屏多个同名条目时不会判歧义。编号只在当前这一帧有效，界面变了就用清单里新的编号；填了它 target 可以只写该条目的名字（供回执显示）。"
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
            },
            "targets": {
                "type": "array",
                "items": { "type": "string" },
                "minItems": 2,
                "maxItems": 6,
                "description": "★多个不同目标一次完成（推荐用于「先点A再点B」类操作：拿卡→放卡、开局选多张卡、多个单位同时进攻）。宿主会逐个识图定位并**立即连续点击**，中间不插观察/验收——省掉每个动作一次主模型轮次。任一目标定位失败即停。给了 targets 就不要再单独发 target。"
            }
        },
        "required": []
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;

        // ★多目标一次定位+连点（省轮次的关键路径）
        std::vector<std::wstring> targets;
        if (params.contains("targets") && params["targets"].is_array()) {
            for (const auto& t : params["targets"]) {
                if (!t.is_string()) continue;
                const std::wstring s = Trim(FromUtf8(t.get<std::string>()));
                if (!s.empty()) targets.push_back(s);
            }
        }
        if (targets.size() >= 2) {
            if (targets.size() > 6) targets.resize(6);
            for (const auto& t : targets) {
                if (t.size() > kMaxLocateAndClickTargetChars) {
                    return L"[错误] targets 里有超长目标（上限 "
                        + std::to_wstring(kMaxLocateAndClickTargetChars) + L" 字）：" + t.substr(0, 32);
                }
            }
            if (const std::wstring guard = GuardPointerClickContext(); !guard.empty()) return guard;
            // ★视觉闸**已删（批 D，docs §47 / D6④）**：原先这里
            //   `if (AiActionUiTooBusyForVisionLocate(&gateRec)) return L"[错误] 前台浏览器页面
            //   动态干扰过大，先 observePage 拿控件树再用 clickRef。";` —— 引擎决定模型
            //   **不许用哪种感知手段**（而且这条历史就是一部错指引史：游戏逐帧重绘必然超标
            //   ⇒ locateAndClick 被 100% 硬拦 ⇒「游戏没反应、一直在思考」）。
            //   现在只保留**如实留痕**（下面这行调用会把「vision_gate 判决 + 输入信号」写进
            //   判断日志，供离线影子测试复算），不再拦、不再劝。
            AiActionUiTooBusyForVisionLocate();
            if (!hooks || !hooks->onLocateMulti) {
                return L"[错误] 多目标定位需要 AI 动作执行宿主（onLocateMulti）。";
            }
            // ★「本次动作定位次数将超上限（24）」的拒绝**已删（批 D，docs §47）**：
            //   写死的次数上限属策略；如实回执里的计数（`本次动作已定位 N 次`）照留。
            //   这里的 `AiNoteLocateAttempt()` 继续记账（计数是事实，模型看不到）。
            AiNoteLocateAttempt();
            const std::wstring button = (params.contains("button") && params["button"].is_string())
                ? FromUtf8(params["button"].get<std::string>()) : L"left";
            const int clickCount = (params.contains("doubleClick") && params["doubleClick"].is_boolean()
                && params["doubleClick"].get<bool>()) ? 2 : 1;
            for (size_t i = 1; i < targets.size(); ++i) AiNoteLocateAttempt();
            const std::wstring out = hooks->onLocateMulti(targets, button, clickCount);
            return out;
        }

        std::wstring target;
        if (params.contains("target") && params["target"].is_string())
            target = FromUtf8(params["target"].get<std::string>());
        // ★★本帧元素索引编号（可选）：给了它就**按编号直查**，不再靠名字解析 ——
        //   同屏多个同名条目（「确定」「600」）按名字会判歧义，按编号不会。
        int elementId = 0;
        if (params.contains("elementId") && params["elementId"].is_number_integer())
            elementId = params["elementId"].get<int>();
        if (elementId < 0) elementId = 0;
        // 编号路径允许 target 省略（回执用索引里的名字）；两条都没有才报错。
        if (Trim(target).empty() && elementId <= 0)
            return L"[错误] locateAndClick 的 target 不能为空（或给 elementId 指向元素索引的编号）。";
        // ★这里曾经有一条「批量选择」关键词门槛（全选/批量选/select all → 要求先 planSpend）。
        //  已删除，理由有两条，都是实测教训：
        //  ① **针对性**：拿关键词去认某个界面的按钮，本质是给个别游戏打的补丁
        //     （用户明确要求「不要做针对性的优化，要做通用的」）。
        //  ② **挡路又无替代**：门槛要的是模型拿不到的数据（各卡价格/卡槽数），
        //     被拦后它当场放弃整个选卡面板、转去 Escape 开暂停菜单，白烧 4 轮，
        //     最后只在种卡栏塞进一张卡。
        //  通用做法：让模型自己判断（Skill 里讲策略），宿主只做**能力**，不做**领域规则**。
        if (Trim(target).size() > kMaxLocateAndClickTargetChars) {
            return L"[错误] locateAndClick.target 过长（"
                + std::to_wstring(Trim(target).size()) + L"字，上限 "
                + std::to_wstring(kMaxLocateAndClickTargetChars)
                + L"）。请只写要点击的控件短描述（文案/颜色/位置），"
                  L"勿粘贴整段任务；宿主会开无上下文识图，长文只会浪费 token。";
        }
        // ★编号路径先解析成该条目的**名字**再继续：后续链路（保存对话拦截、DOM 参考等）
        //   都建立在 target 是「屏幕上的短名字」之上 —— 先解析就等于让它们照常工作。
        //   ⚠ 解析失败（编号不在本帧表里）**不在这里报错**：索引表在宿主侧，这里看不到，
        //     由宿主给可读回执（它才知道表里有什么）。
        // ⚠ 下面这两条只对**按名字**的路径成立：`elementId` 路径的 target 只是显示名，
        //   既不需要长度闸（不是拿去识图的短描述），也不该被「树上有更精确的 ref」改道
        //   —— 模型已经指名了索引里的某一条，改道就等于不听它说的。
        const std::wstring treeRef =
            (elementId > 0) ? std::wstring() : AiLastSnapshotRefMatchingTarget(target);
        // 有宿主的 DOM 钩子时不在这里报错：宿主 onLocateAndClick 会先走扩展控件树精确点击
        // （省一整轮「报错→模型改调 clickRef」）。只有无宿主（离线规划）才保留指路错误。
        const bool hostCanDomClick = hooks && hooks->onLocateAndClick;
        if (elementId <= 0 && !treeRef.empty() && !hostCanDomClick) {
            return L"[错误] 树上已有「"
                + Trim(target).substr(0, 24) + L"」匹配项=" + treeRef
                + L"。请 clickRef(" + treeRef + L")，勿 locateAndClick 识图。";
        }
        // ★视觉闸**已删（批 D，docs §47 / D6④）**：原先这里
        //   `if (AiActionUiTooBusyForVisionLocate(&gateRec)) { … return msg; }`
        //   —— 单目标路径上的同一条硬否决（含按页型分流的两句「改走控件树」文案）。
        //   删它的三条理由：① 引擎决定模型**不许用哪种感知手段**（正是「守卫把模型锁在
        //   外面」）；② 它的历史就是一部错指引史（游戏逐帧重绘必然超标 ⇒ 100% 硬拦 ⇒
        //   「游戏没反应、一直在思考」）；③ 不注入就不会注入错。
        //   只保留**如实留痕**：这一行把「vision_gate 判决 + why/conf + 输入信号」写进判断
        //   日志（离线影子测试靠它复算），不再拦、不再劝。
        AiActionUiTooBusyForVisionLocate();
        // ★模型发来的定位一律如实执行（引擎不做「同一目标试了几次」的决策）。
        // 已写入完整文件路径时，勿再点换目录（会清掉路径）
        // ⏸ 这一条属**破坏性保护族**（保住用户已经输入的文件路径），用户裁定推迟评估
        //   （见 rollback_audit.ps1 的 `keep: deferred guards`），批 D 不动。
        if (!AiSession().pendingSavePath.empty() && IsSaveDialogDetourTarget(target)) {
            return L"[错误] 已在保存框输入完整路径「" + AiSession().pendingSavePath
                + L"」，不要再点「" + Trim(target)
                + L"」。先 Enter/点保存；提交不动则改写纯文件名并点左侧文件夹。";
        }
        if (IsSaveSubmitTarget(target)) AiSession().pendingSavePath.clear();
        if (!hooks || !hooks->onLocateAndClick) {
            return L"[错误] locateAndClick 需要 AI 动作执行宿主（onLocateAndClick）。";
        }
        // ★「本次动作已调用 locateAndClick N 次（单动作资源上限 24）」的拒绝
        //   **已删（批 D，docs §47）**：写死的次数上限属策略；如实回执里的
        //   `本次动作已定位 N 次` 计数照留（计数是模型看不见的事实）。
        AiNoteLocateAttempt();
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
            target, refineLevels, button, doubleClick ? 2 : 1, elementId);
        if (msg.rfind(L"[错误]", 0) == 0) {
            AiNoteLocateMiss();
            std::wstring out = msg
                + L"\n提示：可换描述/换路线再试，或 completeTask(reason=未找到)。";
            const auto kind = AiLastPageKind();
            if (kind == L"dom" || kind == L"mixed") {
                out += L"当前前台是网页（pageKind=" + kind + L"）：控件树那条路仍然可用。";
            } else {
                out += L"当前前台不是网页（pageKind=" + (kind.empty() ? std::wstring(L"未知") : kind)
                    + L"）。";
            }
            // 找任务栏图标/某程序窗口失败：给台账让它改走本地激活，别继续识图碰运气
            if (LooksLikeWindowSwitchTarget(target)) {
                out = WithWindowLedger(hooks, out)
                    + L"\n★要切窗就别识图找任务栏，直接 activateWindow(match=...)。";
            }
            if (const int attempts = AiLocateAttemptCount(); attempts >= 2) {
                // ★「事实留、祈使删」（批 D，docs §47 / D6④）：次数是**本地计数**（模型看不到），
                //   照实说；原先紧随其后的「别再换措辞重复定位同一目标 + 换路线清单」是
                //   引擎替模型定的做法，已删（归属地 = Skill）。
                out += L"\n★本次动作已定位 " + std::to_wstring(attempts)
                    + L" 次（本地计数）。";
            }
            return out;
        }
        AiResetLocateMiss();
        ++AiSession().actionSeq;
        const wchar_t* marker = observeAfter ? kExecutedObserveMarker : kExecutedSkipObserveMarker;
        std::wstring out = std::wstring(marker) + L"\n" + msg;
        // ★原「三档路由（allow 侧）」提示**已删（批 D，docs §47 / D6④）**：
        //   那个提示函数会在「判断把握一般」时往回执里补一句「命中不了就换控件树，
        //   别反复识图」—— 引擎不再往对话/回执里注入建议（函数名与完整叙述见 docs §47）。
        if (!treeRef.empty()) {
            out += msg.find(L"DOM 精确点击") != std::wstring::npos
                ? (L"\n[宿主] 树上匹配 " + treeRef + L"：本次已按控件树精确点击（省识图）；"
                    L"下次可直接 clickRef 省一轮。")
                : (L"\n[宿主] 树上已有匹配项 " + treeRef + L"：下次可直接 clickRef，勿再识图。");
        }
        // ★原「≥4 次：把『别再换措辞重复定位』说透」那段**已删（批 D，docs §47 / D6④）**：
        //   阈值文案（事实）本来就已经由上面 `本次动作已定位 N 次` 那条如实回执承担，
        //   而「不要换措辞再定位同一目标 + 换路线优先级」是引擎替模型定的做法（策略）⇒ 删。
        return out;
    };
    return tool;
}

AgentTool MakeGetColorTool(AiActionHostHooks* hooks, bool allowAbs) {
    return MakeAtomicMacroTool(
        L"getColor",
        L"读取坐标颜色写入变量（{matchVarName}=该点颜色 #RRGGBB，默认 colorRet；"
        L"取点失败写空字符，不保留上一次的颜色）。"
        L"x/y 为屏幕坐标；imageLocate=1 时 x/y 相对图中心，需 imagePath。",
        LR"({
            "type": "object",
            "properties": {
                "x": { "type": "number" },
                "y": { "type": "number" },
                "matchVarName": { "type": "string", "description": "默认 colorRet" },
                "imageLocate": { "type": "boolean" },
                "imagePath": { "type": "string" },
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
        L"findImageFollowUp: 0点击 1移动 2保存到变量（无保存图片）。"
        L"变量：{matchVarName}=命中点实际颜色(#RRGGBB)，{matchVarName}.matchData=匹配度"
        L"（0~100；未命中或失败为 0），.x/.y=命中点坐标 —— 判断是否命中请用 .matchData。"
        L"未命中/失败都会**写**变量（不是保留上一轮的值）。"
        L"imageLocate=1 时先找图，再在命中图范围内找色。",
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
                "imageLocate": { "type": "boolean" },
                "imagePath": { "type": "string" },
                "observeAfter": { "type": "boolean" }
            },
            "required": ["color"]
        })",
        hooks, allowAbs, false);
}

AgentTool MakeColorMatchTool(AiActionHostHooks* hooks, bool allowAbs) {
    return MakeAtomicMacroTool(
        L"colorMatch",
        L"判断 (x,y) 处颜色是否匹配。color 与 x/y 必填；"
        L"变量写入：{matchVarName}=该点实际颜色(#RRGGBB)，{matchVarName}.matchData=匹配度"
        L"（0~100；未匹配或取点失败为 0），.x/.y=坐标 —— 判断匹配请用 .matchData。"
        L"取点失败时两个变量都清零（不留上一轮的值）。"
        L"imageLocate=1 时 x/y 相对图中心。",
        LR"({
            "type": "object",
            "properties": {
                "x": { "type": "number" },
                "y": { "type": "number" },
                "color": { "type": "string", "minLength": 1 },
                "colorTolerance": { "type": "integer" },
                "matchVarName": { "type": "string" },
                "imageLocate": { "type": "boolean" },
                "imagePath": { "type": "string" },
                "observeAfter": { "type": "boolean" }
            },
            "required": ["x", "y", "color"]
        })",
        hooks, allowAbs, false);
}

}  // namespace

// ★`readDocument` 的**外部链接**定义（修 2026-09-23 的一次跨会话撞车，实测报 C2668）：
//   实现体本文件早处（≈3170 行）**在匿名 namespace 里**，而 `macro_execute_tools.h` 又把它
//   声明成全局（`agent_tools.cpp` 要把 readDocument 注册给聊天助手用）。两者并存会同时踩两个坑：
//     ① 本文件里「全局声明 + 匿名定义」⇒ 调用点（`BuildAiActionExecuteTools`）C2668 重载不明确；
//     ② 别的 TU 只有声明、没有定义 ⇒ LNK2019。
//   ⇒ 匿名那份改名 `MakeReadDocumentToolImpl`（只在本 TU 内可见），这里补一个**中转定义**。
//   ⚠ 教训与 §43 同形：**同一个名字的可链接性只能有一处事实**（要么全导内，要么全导出）。
AgentTool MakeReadDocumentTool() { return MakeReadDocumentToolImpl(); }

/// 执行结果「不确定」→ 必须截屏验收（Midscene act→observe）。
/// ★文件作用域定义：`FinishExecuteActions` 与 `AiDecideNeedObserveAfterExec` 都要用，
///   而后者是导出的（放匿名 namespace 里会内部链接 → LNK2019，已踩过）。
bool AiExecResultLooksUncertain(const std::wstring& s) {
    return s.find(L"[UIA]") != std::wstring::npos
        || (s.find(L"提交钮") != std::wstring::npos
            && s.find(L"不可用") != std::wstring::npos)
        || s.find(L"[事实] settle无反应") != std::wstring::npos
        || s.find(L"几乎无变化") != std::wstring::npos
        || s.find(L"灰色不可用") != std::wstring::npos
        || s.find(L"当前不可用") != std::wstring::npos;
}

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
    AiSession().lastOpenWebpageSite.clear();
    AiSession().pendingSavePath.clear();
    AiSession().actionSeq = 0;
    AiSession().lastHideWindowsSeq = -100;
    AiSession().lastResolvedDir.clear();
    AiSession().lastResolvedFullPath.clear();
    AiSession().saveEscapeWarnedSeq = -100;
    AiSession().lastToolSig.clear();
    AiSession().lastToolSigSeq = 0;
    AiSession().recipeVerifiedSig.clear();
    AiSession().recipePreviewGroups = 0;
    AiSession().recipeRowsWritten = 0;
    AiSession().recipeTemplateTrusted = false;
    AiSession().recipePreviewRowsFp.clear();
    AiSession().lastBusyCoverage = -1.0;
    AiSession().lastSettleStillChanging = false;
    AiSession().saveAsScrollStreak = 0;
    AiSession().locateRetryAfterFail = 0;
    AiSession().locateAttemptsThisAction = 0;
    AiSession().foregroundBrowserOverride = 0;
    AiSession().browserLaunchTargetOverride.clear();
    AiSession().spreadsheetFgOverride = 0;
    AiSession().routeNudgeShown = false;
    AiResetPlanSpendGate();   // 门槛按「本次动作」生效：每个动作重新要求先算账
    AiSession().explicitScreenshotRequest = false;
    AiSession().lastSettleReactedBatch = true;
    AiSession().bareTabStreak = 0;
    AiSession().submitUiBlocked = false;
    AiSession().webFetchUntrusted = false;
    AiSession().lastPageKind.clear();
    AiSession().lastPageUrl.clear();
    AiSession().webBrowseSession = false;
    AiSession().lastClickRef.clear();
    AiSession().lastTypeSearchText.clear();
    AiSession().lastSnapshotHrefs.clear();
    AiSession().lastSnapshotInViewContent = 0;
    AiSession().samePageClickStreak = 0;
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

void AiNoteLocateMiss() {
    ++AiSession().locateRetryAfterFail;
}

void AiResetLocateMiss() {
    AiSession().locateRetryAfterFail = 0;
}

int AiLocateRetryCount() {
    return AiSession().locateRetryAfterFail;
}

int AiLocateAttemptCount() {
    return AiSession().locateAttemptsThisAction;
}

int AiNoteLocateAttempt() {
    return ++AiSession().locateAttemptsThisAction;
}

void AiNoteWebFetchUntrusted() {
    AiSession().webFetchUntrusted = true;
}

void AiClearWebFetchUntrusted() {
    AiSession().webFetchUntrusted = false;
}

bool AiWebFetchUntrustedActive() {
    return AiSession().webFetchUntrusted;
}

void AiNotePageKind(const std::wstring& kind) {
    AiSession().lastPageKind = kind;
    if (kind == L"canvas")
        AiSession().webBrowseSession = false;
    else if (kind == L"dom" || kind == L"mixed")
        AiSession().webBrowseSession = true;
}

void AiNotePageTreeTrusted(bool trusted, const std::wstring& why) {
    AiSession().pageTreeTrusted = trusted;
    AiSession().pageTreeUntrustedReason = trusted ? std::wstring() : why;
}

bool AiPageTreeTrusted() {
    return AiSession().pageTreeTrusted;
}

std::wstring AiPageTreeUntrustedReason() {
    return AiSession().pageTreeTrusted ? std::wstring() : AiSession().pageTreeUntrustedReason;
}

void AiNoteWebBrowseSession(bool on) {
    AiSession().webBrowseSession = on;
}

bool AiWebBrowseSessionActive() {
    return AiSession().webBrowseSession;
}

std::wstring AiLastPageKind() {
    return AiSession().lastPageKind;
}

void AiNotePageUrl(const std::wstring& url) {
    if (!PageUrlsSameDocument(url, AiSession().lastPageUrl)) {
        AiSession().lastClickRef.clear();
        AiSession().samePageClickStreak = 0;
    }
    AiSession().lastPageUrl = url;
}

std::wstring AiLastPageUrl() {
    return AiSession().lastPageUrl;
}

void AiNotePageSnapshot(const PageSnapshot& snap) {
    if (snap.kind != PageKind::Unknown)
        AiNotePageKind(PageKindName(snap.kind));
    if (!snap.url.empty())
        AiNotePageUrl(snap.url);
    AiSession().lastSnapshotHrefs.clear();
    AiSession().lastSnapshotHrefs.reserve(snap.nodes.size());
    for (const auto& n : snap.nodes) {
        if (n.ref.empty()) continue;
        AiActionSessionState::SnapshotHref row;
        row.ref = n.ref;
        row.href = n.href;
        row.name = n.name;
        row.role = n.role;
        row.x = n.x;
        row.y = n.y;
        row.w = n.w;
        row.h = n.h;
        row.screenX = n.screenX;
        row.screenY = n.screenY;
        row.inView = n.inView;
        AiSession().lastSnapshotHrefs.push_back(std::move(row));
    }
    AiSession().lastSnapshotInViewContent = CountPageSnapshotInViewMainContent(snap);
    // ★原先这里还会算一个「列表第1项」ref —— 判据是「重复卡片网格的第 1 项」+
    //   一个按 URL 形状判「这是不是播放页」的站点启发式，
    //   消费点只有两条「别点别的卡 / 别滚动」的静默拒绝，那两条已删（批 D，docs §47）。
}

bool AiLastSnapshotClickPoint(const std::wstring& ref, int& screenX, int& screenY,
    std::wstring& name) {
    screenX = 0;
    screenY = 0;
    name.clear();
    for (const auto& node : AiSession().lastSnapshotHrefs) {
        if (node.ref != ref) continue;
        screenX = node.screenX;
        screenY = node.screenY;
        name = node.name;
        return screenX > 0 && screenY > 0;
    }
    return false;
}

std::wstring AiLastSnapshotRefMatchingTarget(const std::wstring& target) {
    PageSnapshot snap;
    snap.nodes.reserve(AiSession().lastSnapshotHrefs.size());
    for (const auto& n : AiSession().lastSnapshotHrefs) {
        PageSnapshotNode node;
        node.ref = n.ref;
        node.role = n.role;
        node.name = n.name;
        node.href = n.href;
        snap.nodes.push_back(std::move(node));
    }
    return SnapshotRefMatchingLocateTarget(snap, target);
}

PageSnapshot AiLastPageSnapshot() {
    PageSnapshot snap;
    snap.kind = ParsePageKindName(AiSession().lastPageKind);
    snap.url = AiSession().lastPageUrl;
    snap.nodes.reserve(AiSession().lastSnapshotHrefs.size());
    for (const auto& n : AiSession().lastSnapshotHrefs) {
        PageSnapshotNode node;
        node.ref = n.ref;
        node.role = n.role;
        node.name = n.name;
        node.href = n.href;
        node.x = n.x;
        node.y = n.y;
        node.w = n.w;
        node.h = n.h;
        node.screenX = n.screenX;
        node.screenY = n.screenY;
        node.inView = n.inView;
        snap.nodes.push_back(std::move(node));
    }
    return snap;
}

std::wstring AiQueryMatchingNavigationUrl(const std::wstring& query) {
    const std::wstring q = Trim(query);
    if (q.empty()) return {};
    const std::wstring page = AiLastPageUrl();
    // ★这里原先按站点形状排序：先挑「某站用户主页」形状的地址，再挑 href 里含 `/video/`
    //   的。两个判据都是**站点专属启发式**（判据里写死域名/URL 形状 ⇒ 换个网站就坏，
    //   D-c）⇒ 已删（批 D，docs §47）。现在只做**名字匹配**。
    //
    // ★★ 2026-09-28 再收紧：判据从「名字**包含**查询词」改成「名字**就是**这个词」。
    //   为什么（用户实测报障「会点到一些莫名其妙的地方」）：
    //     模型调 `searchOnPage("视频")` 想在本站搜一下，而这里只要**任一节点名字含"视频"**
    //     就把页面**自动导航**过去 —— 实测直接跳到了「安迪**视频**」的个人空间，
    //     模型随后拿着新页面的 ref 去点，落到了帮助中心。**引擎替模型做了它没要求的导航。**
    //   ⇒ 只有"查询词就是某个节点的完整名字"（去掉空白后相等）才认为模型指的是那个实体，
    //     才自动打开；否则只返回搜索结果页，让模型自己 clickRef 选。
    //   ⚠ 长度上限：整句里含这个词（如「打开哔哩哔哩并给…点赞」含「视频」）不算实体名。
    auto compact = [](const std::wstring& s) {
        std::wstring o;
        for (wchar_t c : s) {
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'　') continue;
            o.push_back(static_cast<wchar_t>(towlower(c)));
        }
        return o;
    };
    const std::wstring qc = compact(q);
    if (qc.empty() || qc.size() > 24) return {};
    for (const auto& node : AiSession().lastSnapshotHrefs) {
        if (compact(node.name) != qc) continue;   // ← 精确相等，不再"包含"
        const std::wstring abs = ResolvePageNavigationUrl(node.href, page);
        if (!abs.empty()) return abs;
    }
    return {};
}

std::wstring AiLastOpenWebpageUrl() {
    return AiSession().lastOpenWebpageUrl;
}

/// ★★ **记录"我们刚打开过哪个网页"**（2026-09-30）。
///
/// 为什么需要它：`searchOnPage` 的站点上下文 = `AiLastPageUrl()`（扩展观察到的页）
///   或 `AiLastOpenWebpageUrl()`（本机记账）。而**宏动作回放**的 `openWebpage`
///   （模型把 openWebpage 写进动作 JSON 时走的就是这条路）过去**只开网页、不记账**
///   ⇒ `searchOnPage` 紧接着就报「还不知道当前站点」并**连续重试好几轮**
///   （实测第 1、2、3、4、5 轮都在报这一条，用户看到的就是"原地打转"）。
/// 现在回放侧也调这个函数，两条路（工具调用 / 动作 JSON）**共用同一份记账**。
void AiNoteLastOpenWebpageUrl(const std::wstring& url) {
    const std::wstring v = Trim(url);
    if (v.empty()) return;
    AiSession().lastOpenWebpageUrl = v;
    AiNoteWebBrowseSession(true);
}

bool IsBrowserInternalUrl(const std::wstring& url) {
    std::wstring lower = Trim(url);
    for (auto& c : lower)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return lower.rfind(L"edge://", 0) == 0
        || lower.rfind(L"chrome://", 0) == 0
        || lower.rfind(L"about:", 0) == 0
        || lower.rfind(L"view-source:", 0) == 0;
}

void AiNoteInternalPagePendingVerify(const std::wstring& url) {
    AiSession().internalPagePendingVerify = IsBrowserInternalUrl(url) ? Trim(url) : std::wstring();
}

std::wstring AiConsumeInternalPagePendingVerify() {
    std::wstring url = AiSession().internalPagePendingVerify;
    AiSession().internalPagePendingVerify.clear();
    return url;
}

bool AiPageKindIsCanvas() {
    return AiSession().lastPageKind == L"canvas";
}

bool AiPageKindIsDom() {
    return AiSession().lastPageKind == L"dom";
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

/// 前台窗口当前是不是浏览器。用途：**感知分档**（网页控件树相关的判断只在
/// 「前台确实是浏览器」时才成立）——否则用户切到 Excel 另存为对话框后，
/// 那些判断会拿上一次的网页 pageKind 去描述桌面操作（实测：另存为里按 Enter 被当成
/// 「在站内搜索框回车」）。
/// ⚠ 批 D 之后它**不再**用来拦 Enter / 拦新标签（那两条已改成执行 + 如实标注，
///   见 docs §47），但「前台是不是浏览器」仍是判断日志/分档要用的**事实**。
bool AiForegroundIsBrowserWindow() {
    // 自检用的确定性覆盖：本函数的答案取决于**运行时的真实前台窗口**，
    // 于是「网页守卫」类用例会随着测试机上当前前台是不是浏览器而随机红/绿
    //（实测同一二进制连跑三次 153/153、148/5 交替）。自检需要确定答案时显式设置。
    if (AiSession().foregroundBrowserOverride != 0)
        return AiSession().foregroundBrowserOverride > 0;
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return false;
    wchar_t cls[64]{};
    GetClassNameW(fg, cls, 64);
    if (wcscmp(cls, L"#32770") == 0) return false;   // 桌面系统对话框：不是网页
    wchar_t title[512]{};
    GetWindowTextW(fg, title, 512);
    return LooksLikeBrowserWindowTitle(title);
}

/// ★「这个窗口类是不是浏览器」——**一份事实、两处消费**（前台判定 + 找浏览器窗口）。
/// ⚠ 别把类名表抄第二份：批 D 的教训就是"同一事实两处写必然漂移"（docs §43/§47）。
bool WindowClassIsBrowserClass(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return false;
    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, 128);
    // Win32 模态对话框（另存为/打开/消息框）：标题常为空，绝不是网页
    if (wcscmp(cls, L"#32770") == 0) return false;
    return wcscmp(cls, L"Chrome_WidgetWin_1") == 0
        || wcscmp(cls, L"Chrome_WidgetWin_0") == 0
        || wcscmp(cls, L"MozillaWindowClass") == 0;
}

bool ForegroundWindowIsBrowserClass() {
    if (AiSession().foregroundBrowserOverride != 0)
        return AiSession().foregroundBrowserOverride > 0;
    return WindowClassIsBrowserClass(GetForegroundWindow());
}
void SetAiForegroundBrowserOverrideForTest(int force) {
    AiSession().foregroundBrowserOverride =
        (force > 0) ? 1 : ((force < 0) ? -1 : 0);
}

void SetAiBrowserLaunchTargetOverrideForTest(const std::wstring& target) {
    AiSession().browserLaunchTargetOverride = Trim(target);
}

void SetAiSpreadsheetForegroundOverrideForTest(int force) {
    AiSession().spreadsheetFgOverride = (force > 0) ? 1 : ((force < 0) ? -1 : 0);
}

/// 前台是浏览器时返回可用于 runProgram 的启动名（edge/chrome/firefox/brave）；否则空。
/// 用途：打开 edge://chrome:// 这类**内置页**时不再合成「Ctrl+L→打字→Enter」——
/// 那条路依赖焦点与 IME，实测会把 "edge://history" 打成 "dge://history"（首字母被吞），
/// 于是浏览器当成搜索词 → 进了必应搜索页。直接让浏览器自己带 URL 打开既确定又可回放。
std::wstring DetectForegroundBrowserLaunchName() {
    if (AiSession().foregroundBrowserOverride < 0) return {};
    // 自检用：直接给一个启动目标（名或路径），跳过前台探测
    if (!AiSession().browserLaunchTargetOverride.empty())
        return AiSession().browserLaunchTargetOverride;
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (!pid) return {};
    std::wstring exe = ProcessImageNameByPid(pid);
    for (auto& c : exe)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    if (exe.find(L"msedge") != std::wstring::npos) return L"edge";
    if (exe.find(L"chrome") != std::wstring::npos) return L"chrome";
    if (exe.find(L"firefox") != std::wstring::npos) return L"firefox";
    if (exe.find(L"brave") != std::wstring::npos) return L"brave";
    return {};
}

bool LooksLikeUrlInput(const std::wstring& text) {
    const std::wstring s = Trim(text);
    if (s.size() < 4 || s.size() > 2048) return false;
    if (s.find(L' ') != std::wstring::npos || s.find(L'\n') != std::wstring::npos) return false;
    if (s.find(L"://") != std::wstring::npos) return true;          // http:// edge:// chrome:// …
    std::wstring low;
    low.reserve(s.size());
    for (const wchar_t c : s)
        low.push_back(c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c);
    if (low.rfind(L"www.", 0) == 0) return true;                    // www.example.com
    if (low.rfind(L"localhost", 0) == 0) return true;
    // 裸域名：至少一个点 + 末尾是 2~24 个字母的 TLD，且整体没有中文
    const size_t dot = low.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 2 > low.size()) return false;
    const std::wstring tld = low.substr(dot + 1);
    if (tld.size() < 2 || tld.size() > 24) return false;
    for (const wchar_t c : tld) {
        if (!((c >= L'a' && c <= L'z'))) return false;
    }
    for (const wchar_t c : low) {
        if (c >= 0x2E80) return false;                              // 含中日韩字符 → 不是 URL
    }
    const std::wstring host = low.substr(0, dot);
    return host.find(L'.') != std::wstring::npos || host.size() >= 2;
}

// ★「计划门闩」整族**已删（批 D，docs §47）**。原实现：顶层 Agent 在没写
//   `updateTaskMemo(section=goal/todos)` 之前，**硬拒**所有执行类工具
//   （回执是「[错误] 尚未写下任务骨架…」），外加每轮往指令里注入一句祈使句。
//   ⚠ 被删的四个函数名见 docs §47 —— 源码注释里不留已删符号名（审计按子串扫描，
//     注释里的符号名同样算命中）。
//   撤销理由：① 那是**写死的工作流策略**（引擎规定「先写骨架才准动手」），
//   原则原话是「永远不要写死策略，把决策留给模型本身，skill 只提建议」；
//   ② 它是又一个**把模型锁在门外的出口**（§46.4 教训 6）—— 模型已经在动手了，
//   引擎却因为「没写 goal」把它唯一的执行路径全关掉。
//   `updateTaskMemo` / `readTaskMemo` 两个工具本身**保留**（备忘是模型自己的工具）。
//   连带的两个会话字段（计划门闩开关 / 已解锁）与只看备忘 goal/todos 的那个判据函数
//   一并删除。`MemoTextUnlocksPlan()` 保留 —— 它还被 `ai_logic_convert.cpp` 用（另一个子系统）。

std::wstring ExtractActionJsonArrayText(const std::wstring& text) {
    return ExtractLeadingJsonArray(text);
}

void NoteAiActionUiBusy(double busyCoverageRatio, bool settleStillChanging) {
    AiSession().lastBusyCoverage = busyCoverageRatio;
    AiSession().lastSettleStillChanging = settleStillChanging;
}

bool ShouldUseDomFirstAction(const DomFirstActionGateInput& in, std::wstring* outWhy) {
    auto reject = [&](const wchar_t* why) {
        if (outWhy) *outWhy = why;
        return false;
    };
    if (!in.extensionConnected) return reject(L"扩展未连接");
    if (!in.hasDomHooks) return reject(L"宿主未注册 DOM 钩子");
    if (in.pageIsCanvas) return reject(L"canvas 页禁用 DOM");
    if (in.rightButton) return reject(L"右键走识图（避免误触上下文菜单）");
    // 必须确认前台**真的**是浏览器窗口，否则直接按控件树操作会打在错误窗口上。
    //
    // ★这里曾经有一条「标题读不出来时，只要本会话是网页就放行」的兜底 —— 它是个坑：
    // 另存为/打开这类 Win32 模态对话框（类名 #32770）**标题常常是空的**，
    // 于是宿主一边看到「前台标题空」，一边以为「那就是浏览器」，把 clickRef 打进了
    // 浏览器 DOM；而模态对话框又让 activateWindow 切不回来，模型就彻底卡死在保存界面。
    // 现在改成看**窗口类**：Chromium 系（Chrome_WidgetWin_*）/火狐类才算浏览器，
    // #32770 一律不算；标题读不出来也不再放行。
    if (!in.foregroundLooksBrowser && !in.foregroundBrowserClass) {
        return reject(L"前台不是浏览器窗口（对话框/其它程序）");
    }
    if (outWhy) outWhy->clear();
    return true;
}

namespace {
/// 前台窗口是否像「自绘画面」：子窗口极少。游戏/模拟器/自绘应用把内容画在自己窗口上
/// （子窗口 0~2 个）；普通 Win32 程序与对话框都有一排子控件。EnumChildWindows 极廉价。
bool ForegroundWindowLooksSelfDrawn() {
    HWND fg = GetForegroundWindow();
    if (!fg || !IsWindow(fg)) return false;
    // 控制台/终端有自己的文本树，交给 UIA 路线，别当游戏
    wchar_t cls[128]{};
    GetClassNameW(fg, cls, 128);
    const std::wstring klass = cls;
    for (const wchar_t* skip : { L"ConsoleWindowClass", L"CASCADIA_HOSTING_WINDOW_CLASS" }) {
        if (klass == skip) return false;
    }
    int n = 0;
    EnumChildWindows(fg, [](HWND, LPARAM lp) -> BOOL {
        int* c = reinterpret_cast<int*>(lp);
        return (++(*c) > 6) ? FALSE : TRUE;   // 够 6 个就够判定了
    }, reinterpret_cast<LPARAM>(&n));
    return n <= 2;
}
}  // namespace

/// 执行后要不要回传观察帧（三档合取，顺序即优先级）。
/// ★必须放在文件作用域（不在上面的匿名 namespace 内），否则外部目标链接不到（LNK2019）。
bool AiDecideNeedObserveAfterExec(bool requestedObserve, bool firstActionOfTask,
    const std::wstring& execMsg, const std::wstring& summary) {
    if (requestedObserve) return true;
    // 结果不确定必须看（settle 无反应/灰钮/UIA 事实）—— 这条与第一动作无关
    if (AiExecResultLooksUncertain(execMsg) || AiExecResultLooksUncertain(summary))
        return true;
    // ★本任务第一次动手：无论模型写没写 observeAfter，都强制看一眼。
    //   实测「抢跑」：模型第一轮把工具批甩出来就等下一轮，而它此时对本任务的
    //   界面**一个像素都没看过**（首帧往往还没生成），只能靠猜；等它看到画面
    //   可能已经白点了一两步。第一次观察的收益最高、代价只有一个帧。
    if (firstActionOfTask) return true;
    return false;
}

/// 归一 `lookupMacroAction` 的查询词。
///
/// 为什么需要（实测日志）：模型会照 Skill 的写法把**整个键值对**塞进 type 参数
/// （`lookupMacroAction(section=game)` → `type: "section=game"`），而下游是按
/// **裸值**字符串比较的（`q == L"game"` 才命中游戏 Skill）。结果 `section=game`
/// 落进兜底分支，报「未找到「section=game」」，白烧一轮 API；
/// 更糟的是模型会**再试一次**（日志里连着两轮都在 lookup 同一件事）。
///
/// 顺带把模型的其它常见写法一起归一：`type=x` / `type: x` / `section: x` /
/// `section＝x`（全角等号）/ 前后引号与空白。
///
/// ★必须放在文件作用域（不在上面的匿名 namespace 内）：否则头文件的外部声明
///   与匿名 namespace 的内部定义会同时可见，外部目标链接不到（实测 LNK2019）。
std::wstring NormalizeMacroLookupQuery(const std::wstring& raw) {
    std::wstring q = Trim(raw);
    // 去掉包裹的引号（模型有时写 "game" / 'game' / 「game」）
    auto stripQuotes = [](std::wstring& s) {
        while (s.size() >= 2) {
            const wchar_t a = s.front(), b = s.back();
            const bool pair = (a == L'"' && b == L'"') || (a == L'\'' && b == L'\'')
                || (a == L'「' && b == L'」') || (a == L'“' && b == L'”');
            if (!pair) break;
            s = s.substr(1, s.size() - 2);
        }
    };
    stripQuotes(q);
    q = Trim(q);
    // 去掉 `键=` / `键:` 前缀（半角/全角等号与冒号都见模型写过）
    const wchar_t* kKeys[] = { L"section", L"type", L"s", L"t" };
    for (const wchar_t* k : kKeys) {
        const size_t klen = wcslen(k);
        if (q.size() <= klen) continue;
        bool keyMatch = true;
        for (size_t i = 0; i < klen; ++i) {
            wchar_t c = q[i];
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
            if (c != k[i]) { keyMatch = false; break; }
        }
        if (!keyMatch) continue;
        const wchar_t sep = q[klen];
        // 半角/全角等号与冒号都见模型写过；**不**把空格当分隔符（太松）
        if (sep != L'=' && sep != L':' && sep != L'\uFF1A' && sep != L'\uFF1D') continue;
        std::wstring rest = Trim(q.substr(klen + 1));
        stripQuotes(rest);
        rest = Trim(rest);
        if (!rest.empty()) { q = rest; break; }
    }
    return q;
}

// ── 判断表入口（src/ai_decide.h）：把线程局部的桌面状态装进信号向量 ──────────
// 判断逻辑本身在 ai_decide.cpp（纯函数，可逐格自检）；这里只负责「取宿主事实」。
AiDecideSignals CollectAiDecideSignals() {
    AiDecideSignals s;
    s.pageKind = AiSession().lastPageKind;
    s.foregroundBrowserClass = ForegroundWindowIsBrowserClass();
    s.foregroundSelfDrawn = ForegroundWindowLooksSelfDrawn();
    s.foregroundSpreadsheet = ForegroundIsSpreadsheetApp();
    s.webSessionActive = AiWebBrowseSessionActive();
    s.busyCoverage = AiSession().lastBusyCoverage;
    s.settleStillChanging = AiSession().lastSettleStillChanging;
    // domHooksAvailable 刻意留空（false）：只有**置信度**用它，第 1 刀不许它改判决，
    // 而为此把 window_mode 的 ExtBridgeServer 拉进本文件会多一条跨模块依赖，
    // 不值。扩展连接状态在宿主侧（engine_script_run.cpp 的 DOM 门禁）已经判过，
    // 第 2 刀要真用它时，应从宿主经 AiActionHostHooks 传进来，而不是在这里反查单例。
    return s;
}

/// 游戏前台判断的薄封装会把它写在这里，供**有延迟**的调用方（settle 节拍）按
/// 置信度分档 —— 游戏画面覆盖率是抖的，「这次算不算游戏」本身有把握高低之分。
thread_local AiGameForegroundDecision g_lastGameForeground;

/// 视觉闸（薄封装）。判断在 ai_decide.cpp；这里只取宿主事实 + 留一行诊断。
/// 调用方要按三档路由加提示时，传 `outDecision` 拿走本次的理由与置信度。
bool AiActionUiTooBusyForVisionLocate(AiDecisionRecord* outDecision) {
    AiDecisionRecord rec;
    const AiDecideSignals sig = CollectAiDecideSignals();
    const AiVisionGate gate = AiDecideVisionGate(sig, &rec);
    if (outDecision) *outDecision = rec;
    // ★诊断：这条闸曾经把游戏 100% 拦死，用户只能看到「没反应」。
    //   现在每次判断都留一行「放行/拦下 + 依据 + 置信度 + 输入信号」，
    //   报障时先看这行；★带信号是为了**离线影子测试**能复算（docs §23），
    //   只记判决的话「换个阈值会不会更准」就永远答不了。
    NoteAiDecisionLog(FormatAiDecision(L"vision_gate", AiVisionGateName(gate), rec)
        + L" | " + FormatAiSignals(sig));
    return gate == AiVisionGate::DenyBusyFallback;
}

// ── 三档路由：**已整体删除（批 D，docs §47）** ──────────────────────────
// 原实现把 confidence 变成「往对话/回执里塞一句提示」（高置信不吭声、中/低置信补一句
// 「改走控件树、别反复识图」）。⚠ 那个函数名与完整叙述见 docs §47（源码注释里不留
// 已删符号名 —— `rollback_audit.ps1` 故意把注释里的符号名也算命中）。
// 撤销理由：① 引擎不该往对话里注入建议 —— 想用什么手段由模型自己定（Skill 只提建议）；
// ② 它顺带消灭一整类坑：`visionIsOnlyWay`（画布页 / 非浏览器前台）时误注入
//    「改用控件树」—— 那正是当年把游戏锁死的那条错指引（docs §20.1）。
//    **不注入就不会注入错。**
// 保留：判断表本身（`AiDecide*` + confidence + why）与 `NoteAiDecisionLog` /
//    `FormatAiSignals` 的**如实留痕**（可离线复算，属感知）—— 见 ai_decide.h。

/// 游戏前台判断（薄封装）。判断在 ai_decide.cpp；这里取宿主事实 + 留诊断。
/// ★另有一条「有延迟的调用方」约定：`AiLastGameForegroundDecision()` 服务的是
///   **远离判断点**的调用方（如 settle 节拍选择：工具循环已经跑完、状态又变了）。
///   近处调用方应像视觉闸那样用 out 参数拿同一份记录，别用这个 getter 读「上次」。
bool AiActionGameForegroundLikely() {
    AiDecisionRecord rec;
    const AiDecideSignals sig = CollectAiDecideSignals();
    const AiGameForeground v = AiDecideGameForeground(sig, &rec);
    g_lastGameForeground.verdict = v;
    g_lastGameForeground.record = rec;
    // 同样带输入信号：离线影子测试要复算这一条（含 kAiGameConfDecisive 的分档）。
    NoteAiDecisionLog(FormatAiDecision(L"game_foreground", AiGameForegroundName(v), rec)
        + L" | " + FormatAiSignals(sig));
    return v != AiGameForeground::NotGame;
}

/// 最近一次游戏前台判断（判决 + 依据），供 settle 节拍这类**有延迟**的调用方
/// 按置信度分档。约定见 ai_decide.h 的 kAiGameConfDecisive。
AiGameForegroundDecision AiLastGameForegroundDecision() { return g_lastGameForeground; }

// ── 选卡/批量选择硬门槛：本次动作是否调用过 planSpend ─────────────────
// ★必须在文件作用域（不能在文件上方的匿名 namespace 里）：否则头文件的外部声明
//   与本地的内部定义会同时可见，调用处报 C2668「对重载函数的调用不明确」（已踩过）。
namespace { bool g_planSpendCalledThisAction = false; }
bool AiPlanSpendCalledThisAction() { return g_planSpendCalledThisAction; }
void AiNotePlanSpendCalled() { g_planSpendCalledThisAction = true; }
/// 每个动作开始都要复位：门槛按「本次动作」生效，不能一次算账之后永久放行
void AiResetPlanSpendGate() { g_planSpendCalledThisAction = false; }

// ── 「模型明确要了截图」→ 下一次观察必须回传帧 ──────────────────────────
// ★必须在文件作用域（同 planSpend 门槛：内部定义 + 头文件外部声明会报 C2668）。
void NoteAiExplicitScreenshotRequest() { AiSession().explicitScreenshotRequest = true; }
bool AiTakeExplicitScreenshotRequest() {
    const bool v = AiSession().explicitScreenshotRequest;
    AiSession().explicitScreenshotRequest = false;
    return v;
}

void NoteAiUiSettleReacted(bool reacted) { AiSession().lastSettleReactedBatch = reacted; }
bool AiLastUiSettleReacted() { return AiSession().lastSettleReactedBatch; }

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
                return true; // 去重
            }
        }
        lines.push_back(stored);
    }
    while (lines.size() > kAiMemoMaxLines) lines.erase(lines.begin());
    if (!SaveMemoLines(lines)) return false;
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
    if (toolName == L"observePage") return 16000;
    if (toolName == L"clickRef") return 16000;
    if (toolName == L"typeRef") return 16000;
    // typeByLabel 只回紧凑摘要（不塞整棵树），预算按摘要给
    if (toolName == L"typeByLabel") return 1200;
    // ⚠⚠ 这张表和 `engine_script_run.cpp` 里 `onListUiControls` 的
    //   `FormatUiControlListForAgent(items, 3600)` **必须一起改** ——
    //   台账在引擎里按 3600 渲染，若这里只放 2400 就会被二次砍回去，
    //   表现是「明明列了更多条，模型只看到 47 条」（实测症状，排查了很久）。
    //   列表型界面（历史记录/书签/文件列表）一行约 40–60 字符，2400 只够 40 条出头。
    if (toolName == L"listUiControls") return 4000;
    if (toolName == L"invokeUiControl") return 900;
    if (toolName == L"runCommand") return 1200;
    if (toolName == L"readDocument") return 9000;
    if (toolName == L"computer") return 700;
    if (toolName == L"searchOnPage") return 16000;
    // webSearch：结果清单（标题/URL/摘要）本来就紧凑；带 readTop 时正文自己已按 maxChars 截过
    if (toolName == L"webSearch") return 20000;
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
        || name == L"computer"
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
        || name == L"clickRef"
        || name == L"typeRef"
        || name == L"typeByLabel"
        || name == L"invokeUiControl"
        || name == L"runCommand"
        || name == L"searchOnPage"
        || name == L"openWebpage"
        || name == L"runProgram"
        || name == L"openAppViaSearch"
        || name == L"openFile"
        || name == L"getColor"
        || name == L"findColor"
        || name == L"colorMatch";
}

bool AlignToolParamsWithActionFields(const std::wstring& toolName, nlohmann::json& args) {
    if (!args.is_object()) args = nlohmann::json::object();

    // ① 只列**名字确实不同**的那些（同名的不用动）。
    //    加新工具时若参数名与动作字段不一致，**必须**在这里补一行，
    //    否则下面的 ② 会把它挡成「不可描述」→ 只影响回执详细度，不会说谎。
    if (toolName == L"mouseDrag") {
        static const struct { const char* from; const char* to; } kAliases[] = {
            {"fromX", "x"}, {"fromY", "y"}, {"toX", "endX"}, {"toY", "endY"},
        };
        for (const auto& a : kAliases) {
            if (!args.contains(a.from)) continue;
            // 动作字段已被显式给定时不覆盖（模型可能两套都写了）
            if (!args.contains(a.to)) args[a.to] = args[a.from];
        }
    }

    // ② 「参数里有坐标名，却没有一个动作字段接得住」⇒ 不可描述。
    //    判据要**只看坐标类键名**：`keyClick{keyText}` 这类本来就没有坐标，必须放行。
    static const char* kCoordKeys[] = {
        "x", "y", "endX", "endY",
        "fromX", "fromY", "toX", "toY",
        "x1", "y1", "x2", "y2", "startX", "startY",
    };
    bool anyCoordKey = false;
    for (const char* k : kCoordKeys) {
        if (args.contains(k)) { anyCoordKey = true; break; }
    }
    if (!anyCoordKey) return true;
    return args.contains("x") || args.contains("y");
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
        || name == L"listUiControls"
        || name == L"observePage"
        || name == L"resolveSystemPath"
        || name == L"readDocument";
}

// ── `zoom`：把一块区域**按原始分辨率放大**回传 ─────────────────────────
// 解决一个**通用**瓶颈：整帧降到 1024 宽之后，小字/图标只剩几十像素，模型只能靠猜
// （实测那一局 15 张僵尸卡每张 ≈40 upload 像素，模型整轮整轮在推"这张卡是什么"）。
// 依据：Efficient GUI Agents 系统综述（arXiv 2609.02309）把「区域化视觉感知」列为
// Observation Efficiency 的**专门一类**（R-VLM / RegionFocus / ShowUI / DiMo-GUI…），
// 并把 **image crops 当作一等观测原语单独计量**；AguVis 的 limitations 亦写明
// 「对视觉模型的分辨率要求很高」。我们不训练模型 ⇒ 那就给它一个**放大镜**。
//
// 三条口径纪律（本仓刚因为三套坐标混说烧掉过几千 token，别再犯）：
//   ① 入参 = **upload 截图像素**（与元素索引/文字索引同一套）⇒ 抄索引坐标即可，零换算；
//   ② 回执必须写清「区域、倍率 N、图内某点的 upload 坐标 = x1 + 图x/N」；
//   ③ 成本如实记账（PNG 字节数进回执；这轮请求体的图 KB 由「请求体拆解」如实报）。
bool AiZoomClampRect(int x1, int y1, int x2, int y2, int frameW, int frameH,
    int minSide, AiZoomRect& out, std::wstring& why) {
    why.clear();
    if (frameW <= 0 || frameH <= 0) {
        why = L"还不知道当前帧尺寸（先 computer(action=screenshot) 观察一轮再放大）";
        return false;
    }
    if (x2 < x1) std::swap(x1, x2);
    if (y2 < y1) std::swap(y1, y2);
    if (x2 <= 0 || y2 <= 0 || x1 >= frameW || y1 >= frameH) {
        why = L"区域完全在帧外（本帧是 " + std::to_wstring(frameW) + L"×"
            + std::to_wstring(frameH) + L" upload 像素）";
        return false;
    }
    x1 = std::clamp(x1, 0, frameW);
    x2 = std::clamp(x2, 0, frameW);
    y1 = std::clamp(y1, 0, frameH);
    y2 = std::clamp(y2, 0, frameH);
    const int minS = (std::max)(1, minSide);
    if (x2 - x1 < minS || y2 - y1 < minS) {
        why = L"区域太小（每条边至少 " + std::to_wstring(minS) + L" upload 像素；"
              L"再小的话放大图里放不下东西）";
        return false;
    }
    out = AiZoomRect{ x1, y1, x2, y2 };
    return true;
}

namespace {

/// 倍率 = 交付图里的像素 / 该块的 upload 像素（两位小数，`×2.50`）。
std::wstring FormatZoomMagnification(int deliveredW, int upW) {
    const int uw = (std::max)(1, upW);
    const double mag = static_cast<double>(deliveredW) / static_cast<double>(uw);
    const int magX100 = static_cast<int>(mag * 100.0 + 0.5);
    return std::to_wstring(magX100 / 100) + L"." + (magX100 % 100 < 10 ? L"0" : L"")
        + std::to_wstring(magX100 % 100);
}

std::wstring FormatZoomKb(size_t bytes) {
    return std::to_wstring((bytes + 1023) / 1024) + L" KB";
}

std::wstring ZoomEntryLabel(const AiZoomResult& d) {
    std::wstring out = L"（入口＝";
    if (d.byTarget) {
        out += L"文字标签";
        if (!d.resolvedName.empty()) out += L"「" + d.resolvedName + L"」";
    } else {
        out += L"坐标";
    }
    out += L"）";
    return out;
}

std::wstring FormatZoomRect(const AiZoomRect& r) {
    return L"upload(" + std::to_wstring(r.x1) + L"," + std::to_wstring(r.y1) + L")-("
        + std::to_wstring(r.x2) + L"," + std::to_wstring(r.y2) + L")";
}

}  // namespace

std::vector<AiZoomRect> PlanAiZoomTiles(const AiZoomRect& area, double nativePerUpX,
    double nativePerUpY, int maxEdge, int maxTiles, int* needTiles) {
    if (needTiles) *needTiles = 0;
    std::vector<AiZoomRect> tiles;
    if (area.width() <= 0 || area.height() <= 0) return tiles;
    const double perX = nativePerUpX > 0.0 ? nativePerUpX : 1.0;
    const double perY = nativePerUpY > 0.0 ? nativePerUpY : 1.0;
    const int mEdge = (std::max)(64, maxEdge);
    // 每块在 **upload 空间** 的最大边长 = 原生上限 ÷ 原生像素/upload 像素
    const int tileMaxW = (std::max)(8, static_cast<int>(static_cast<double>(mEdge) / perX));
    const int tileMaxH = (std::max)(8, static_cast<int>(static_cast<double>(mEdge) / perY));
    const int nx = (std::max)(1, (area.width() + tileMaxW - 1) / tileMaxW);
    const int ny = (std::max)(1, (area.height() + tileMaxH - 1) / tileMaxH);
    const int need = nx * ny;
    if (needTiles) *needTiles = need;
    const int cap = (std::max)(1, maxTiles);
    if (need > cap) return tiles;   // 交给调用方走「整块降采样」并如实说明
    // 均分（整除余数摊到各块，避免最后一块只剩几像素）；顺序＝阅读顺序（先左右、后上下），
    // 因为模型按「第 1 张/第 2 张」对上这些块。
    for (int iy = 0; iy < ny; ++iy) {
        const int ty1 = area.y1 + static_cast<int>(
            static_cast<long long>(area.height()) * iy / ny);
        const int ty2 = area.y1 + static_cast<int>(
            static_cast<long long>(area.height()) * (iy + 1) / ny);
        for (int ix = 0; ix < nx; ++ix) {
            const int tx1 = area.x1 + static_cast<int>(
                static_cast<long long>(area.width()) * ix / nx);
            const int tx2 = area.x1 + static_cast<int>(
                static_cast<long long>(area.width()) * (ix + 1) / nx);
            tiles.push_back(AiZoomRect{ tx1, ty1, tx2, ty2 });
        }
    }
    return tiles;
}

std::wstring FormatAiZoomReceipt(const AiZoomResult& d) {
    if (d.tiles.empty()) return L"[错误] zoom 没有可交付的图（宿主实现缺陷）";
    const size_t n = d.tiles.size();
    const bool multi = n > 1;
    std::wstring out = L"zoom 已回传**放大图**" + ZoomEntryLabel(d) + L"：你要的区域 "
        + FormatZoomRect(d.requested) + L" 共 " + std::to_wstring(d.requested.width()) + L"×"
        + std::to_wstring(d.requested.height());
    if (multi) {
        out += L"，**分 " + std::to_wstring(n) + L" 张给你**"
            L"（每张都是原生分辨率裁剪，你要的整块都在里面 —— 没有任何一块被丢掉）";
    }
    out += L"。\n";
    for (size_t i = 0; i < n; ++i) {
        const AiZoomTile& t = d.tiles[i];
        const std::wstring mag = FormatZoomMagnification(t.outWidth, t.area.width());
        if (multi) out += L"· 第 " + std::to_wstring(i + 1) + L"/" + std::to_wstring(n) + L" 张";
        else out += L"· 你看到的这张图";
        out += L" = " + FormatZoomRect(t.area) + L"，图 " + std::to_wstring(t.outWidth) + L"×"
            + std::to_wstring(t.outHeight) + L"，倍率 ×" + mag + L"，"
            + FormatZoomKb(t.bytes) + L"\n";
    }
    // ★坐标换算必须**逐块**给基准：分块时拿错块的 x1/y1 会整体平移（正是本仓踩过的
    //   「一句话里混两套坐标」）。所以每块都写清「用哪张的基准 + 除哪个倍率」。
    out += L"⚠ 坐标口径：图内某点 (图x,图y) 换算回 **upload 截图像素** = ";
    if (!multi) {
        const AiZoomTile& t = d.tiles.front();
        out += L"(" + std::to_wstring(t.area.x1) + L" + 图x/"
            + FormatZoomMagnification(t.outWidth, t.area.width()) + L", "
            + std::to_wstring(t.area.y1) + L" + 图y/"
            + FormatZoomMagnification(t.outHeight, t.area.height()) + L")";
    } else {
        for (size_t i = 0; i < n; ++i) {
            const AiZoomTile& t = d.tiles[i];
            if (i) out += L"；";
            out += L"第 " + std::to_wstring(i + 1) + L" 张 → ("
                + std::to_wstring(t.area.x1) + L" + 图x/"
                + FormatZoomMagnification(t.outWidth, t.area.width()) + L", "
                + std::to_wstring(t.area.y1) + L" + 图y/"
                + FormatZoomMagnification(t.outHeight, t.area.height()) + L")";
        }
    }
    out += L"；要点击就用换算出来的 upload 值"
        L"（mouseClick / locateAndClick 收的就是它，别拿图像像素直接点）。";
    if (!d.note.empty()) out += L"\n⚠ " + d.note;
    return out;
}

std::wstring FormatAiZoomTempPath(const std::wstring& dir, unsigned long pid, unsigned seq) {
    // ★后缀是 `.jpg` 而不是 `.png`：交付图**一次就编成送图链路的形态**（JPEG q82 +
    //   长边 ≤ `kAgentAttachmentMaxLongEdge`）⇒ 磁盘上的字节数就是模型拿到的字节数。
    //   写 PNG 的代价实测过：一张 1150×725 的游戏裁剪是 1365 KB，而模型手里那张
    //   重编码后的 JPEG 只有 ~200 KB —— 回执按前者报，等于对模型说谎。
    return dir + L"\\zoom_" + std::to_wstring(pid) + L"_" + std::to_wstring(seq) + L".jpg";
}

void PruneAiZoomTempDir(const std::wstring& dir, unsigned long long olderThanMs) {
    if (dir.empty()) return;
    FILETIME nowFt{};
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER nu{};
    nu.LowPart = nowFt.dwLowDateTime;
    nu.HighPart = nowFt.dwHighDateTime;

    // 两种后缀都清：`.jpg` 是现在的交付格式，`.png` 是旧版本留下的（升级后不该永远躺在盘里）
    for (const wchar_t* pattern : { L"\\zoom_*.jpg", L"\\zoom_*.png" }) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((dir + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
            ULARGE_INTEGER ft{};
            ft.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            ft.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            // 未来时间戳 / 时间倒退 → 年龄算不出来：宁可不删（删错的代价是模型要的图没了）
            if (nu.QuadPart <= ft.QuadPart) continue;
            const unsigned long long ageMs = (nu.QuadPart - ft.QuadPart) / 10000ULL;
            if (ageMs < olderThanMs) continue;
            DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

AgentTool MakeZoomTool(AiActionHostHooks* hooks) {
    AgentTool tool;
    tool.name = L"zoom";
    tool.description =
        L"把当前画面的**一块区域放大回传**（从原始分辨率裁剪，不经过整帧降采样）——"
        L"看不清小字/图标/角标/卡价时用它，**别靠猜**。"
        L"⚠ 坐标是 **upload 截图像素**，与元素索引/文字索引给的那一套**完全相同**"
        L"（直接把索引里的 (x,y) 抄进来即可，不需要任何换算）。"
        L"两种用法任选：① x1,y1,x2,y2（左上角、右下角）；"
        L"② target=\"屏幕上的短标签\"，由宿主用本帧元素索引解析区域"
        L"（与 locateAndClick 查表是**同一张表** ⇒ 放大的就是会点的那个地方）。"
        L"回执会写明这次用了哪个入口、**每张图各覆盖哪一块**、放大倍率、"
        L"以及「图内坐标 → upload 像素」的换算公式。"
        L"⚠ **你要的区域一定会全给你**：区域大到一张装不下时会**分块**（第 1/2、第 2/2 张，"
        L"各带自己的区域与倍率）；再大就只能整块降采样（回执会说明放大倍数已丢失，"
        L"要看清细部请把区域改小）。整条卡槽/整行按钮直接给那一条的包围盒即可，别反复刷图。";
    tool.parameters_json = LR"({
        "type": "object",
        "properties": {
            "x1": {"type": "integer", "description": "区域左上角 x（upload 截图像素）"},
            "y1": {"type": "integer", "description": "区域左上角 y（upload 截图像素）"},
            "x2": {"type": "integer", "description": "区域右下角 x（upload 截图像素）"},
            "y2": {"type": "integer", "description": "区域右下角 y（upload 截图像素）"},
            "target": {"type": "string", "description": "可选：屏幕上真实存在的短标签；给了它就用元素索引解析区域（此时忽略上面四个坐标）"},
            "maxEdge": {"type": "integer", "minimum": 256, "maximum": 1280, "description": "每张回传图的长边上限，默认 1280（= 送图链路的硬上限，写更大也不会更清楚）；区域装不下时会分块，再大则整块降采样（回执都会说明）"}
        }
    })";
    tool.execute = [hooks](const std::wstring& paramsJson) -> std::wstring {
        std::wstring parseError;
        json params = ParseToolParams(paramsJson, parseError);
        if (!parseError.empty()) return parseError;
        const std::wstring target = (params.contains("target") && params["target"].is_string())
            ? Trim(FromUtf8(params["target"].get<std::string>())) : std::wstring();
        const bool hasCoords = params.contains("x1") && params.contains("y1")
            && params.contains("x2") && params.contains("y2")
            && params["x1"].is_number_integer() && params["y1"].is_number_integer()
            && params["x2"].is_number_integer() && params["y2"].is_number_integer();
        if (target.empty() && !hasCoords) {
            return L"[错误] zoom 需要两种入参之一：① x1,y1,x2,y2（upload 截图像素）；"
                   L"② target=\"屏幕上的短标签\"（宿主用元素索引解析）。两个都没给，我不猜区域。";
        }
        // ★上限必须与**送图链路**的硬上限一致（`kAgentAttachmentMaxLongEdge`）：
        //   附件进请求前会被缩到那个长边。工具要是允许 2048，模型拿到的是 1280 的图，
        //   而回执按 2048 报倍率 ⇒ 它按那个公式换算出的 upload 坐标**系统性偏移**
        //   （实测：想点 600 的卡、点到 400 的卡上）。写更大**不会**更清楚，只会更不准。
        int maxEdge = kAgentAttachmentMaxLongEdge;
        if (params.contains("maxEdge") && params["maxEdge"].is_number_integer())
            maxEdge = std::clamp(params["maxEdge"].get<int>(), 256,
                kAgentAttachmentMaxLongEdge);
        if (!hooks || !hooks->onZoomRegion) {
            return L"[错误] zoom 需要 AI 动作执行宿主（放大能力由宿主提供）。"
                   L"可先用 computer(action=screenshot) 看整帧。";
        }
        const AiZoomResult r = hooks->onZoomRegion(
            hasCoords ? params["x1"].get<int>() : 0,
            hasCoords ? params["y1"].get<int>() : 0,
            hasCoords ? params["x2"].get<int>() : 0,
            hasCoords ? params["y2"].get<int>() : 0,
            target, maxEdge);
        if (!r.ok)
            return L"[错误] zoom 失败：" + (r.error.empty() ? L"宿主没给原因" : r.error);
        if (r.tiles.empty())
            return L"[错误] zoom 说成功了却没给图（宿主实现缺陷）";
        std::wstring out = FormatAiZoomReceipt(r);
        // 图片**随本轮工具结果**回传（AgentCore 认这个标记；非多模态模型会降级成路径提示）。
        // 分块时有多张 —— AgentCore 会按顺序编成「附图 1/2」，与回执里的「第 1/2 张」对齐。
        for (const auto& t : r.tiles) {
            if (t.imagePath.empty()) continue;
            out += L"\n[[AGENT_IMG:" + t.imagePath + L"]]";
        }
        return out;
    };
    return tool;
}

std::vector<AgentTool> BuildAiActionExecuteTools(AiActionHostHooks* hooks, AiActionToolOptions opts) {
    const bool allow = opts.allowAbsolutePointer;
    std::vector<AgentTool> tools = {
        MakeQuickInputTool(hooks, allow),
        MakeKeyClickTool(hooks, allow),
        MakeMouseClickTool(hooks, allow),
        MakeComputerTool(hooks, allow),
        MakeMoveMouseTool(hooks, allow),
        MakeMouseDragTool(hooks, allow),
        MakeScrollWheelTool(hooks, allow),
        MakeWaitTool(hooks, allow),
        MakeHotkeyShortcutTool(hooks, allow),
        MakeListWindowsTool(hooks),
        MakeListUiControlsTool(hooks),
        MakeInvokeUiControlTool(hooks),
        MakeActivateWindowTool(hooks),
        MakeRunActionRecipeTool(hooks, allow),
        MakeSwitchWindowTool(hooks),
        MakeFindImageTool(hooks, allow),
        MakeLocateAndClickTool(hooks),
        MakeZoomTool(hooks),
        MakeObservePageTool(hooks),
        MakeClickRefTool(hooks),
        MakeTypeRefTool(hooks),
        MakeTypeByLabelTool(hooks),
        MakeSearchOnPageTool(hooks),
        MakeResolveSystemPathTool(),
        MakeReadDocumentTool(),
        // ★★ 「统计最近 N 条浏览记录 / 看书签」的正路。必须排在 openWebpage 之前 ——
        //    模型看到工具顺序会优先选「更直接的读法」，而不是「开浏览器截图抄」。
        MakeReadBrowserHistoryTool(),
        MakeOpenWebpageTool(hooks, allow),
        MakeRunProgramTool(hooks, allow),
        MakeRunCommandTool(hooks, allow),
        MakeOpenAppViaSearchTool(hooks, allow),
        MakeOpenFileTool(hooks, allow),
        MakeFetchWebPageTool(),
        // ★「查资料」的第一步：开浏览器搜是最后手段（它会抢前台），宿主 HTTP 搜索才是正路
        MakeWebSearchTool(),
        MakeGetColorTool(hooks, allow),
        MakeFindColorTool(hooks, allow),
        MakeColorMatchTool(hooks, allow),
        MakeUpdateTaskMemoTool(),
        MakeReadTaskMemoTool(),
        MakeSaveTaskDataTool(),
        MakeReadTaskDataTool(),
        MakeSwitchImeTool(),
        MakeSubmitMacroActionsToolLocal(hooks, allow),
        MakePlanSpendTool(),
        MakeLookupMacroActionTool(),
        MakeCompleteTaskTool(),
    };
    // ★原「本场景裁工具表」两处已整族撤销：
    //   ① 「游戏前台裁工具」**批 C，docs §46**；
    //   ② 「按**提示词子串**裁工具表」（`AiActionToolOptions` 里那个布尔字段，靠
    //      resolvedPrompt 里有没有「只填表」这类**文本**决定不下发
    //      runProgram/openWebpage/hotkey）**批 E，docs §48**。
    //   两处都是「引擎写死这个场景用不到这些工具」+ 按文案匹配 ⇒ **工具表永远给全**，
    //   用哪个由模型自己判断。该结构体现在只剩「无图禁绝对坐标」一个入参。
    // ★原「先写 goal/todos 才准动手」的门闩包装**已删（批 D，docs §47）**：
    //   写死的工作流策略，见文件上方说明。
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

// ── 公开包装：把「读办公文档」工具暴露给 AI 脚本助手 ──────────────────
//
// 为什么需要这个包装：真正的 `MakeReadDocumentTool()` 定义在**匿名命名空间**里
// （内部链接，只有本 TU 的 BuildAiActionExecuteTools 能拿到）。直接在头文件里声明
// 同名函数会造出**两个重载**，编译器在 6499 行调用点报 C2668「调用不明确」——
// 这正是第一次接助手时踩到的编译错。所以对外只暴露这个名字不同的包装，
// 内部函数一个字都不用动。
AgentTool MakeAgentReadDocumentTool() {
    return MakeReadDocumentTool();
}
