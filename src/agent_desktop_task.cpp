// ──────────────────────────────────────────────────────────────────
// agent_desktop_task.cpp — 实现见头文件顶部的设计说明
// ──────────────────────────────────────────────────────────────────

#include "agent_desktop_task.h"

#include "utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cwctype>

using json = nlohmann::json;

namespace qst {
namespace agent {
namespace {

const wchar_t* const kResultVar = L"__qstAgentTaskResult";

std::wstring LowerCopy(const std::wstring& s) {
    std::wstring out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return out;
}

std::wstring ReadStr(const json& obj, const char* key) {
    if (!obj.contains(key) || !obj[key].is_string()) return L"";
    return Trim(FromUtf8(obj[key].get<std::string>()));
}

/// 危险动作词表（**不可逆 / 对外**）—— 中文按子串匹配。
///
/// 收录口径：做完之后**用户无法靠撤销拿回来**的事。
///  · 删 / 清空 / 格式化 / 覆盖  —— 数据没了就是没了
///  · 发送 / 提交 / 付款 / 下单  —— 对面已经收到了，撤不回来
///  · 卸载 / 关机 / 重启 / 注册表 —— 影响整机，不是单个文件
/// 刻意**不**收录：打开、关闭窗口、输入文字、滚动、点击普通按钮 ——
/// 那些是桌面任务的日常，每步都问会把闸问废。
const wchar_t* const kDangerWordsCjk[] = {
    L"删除", L"删掉", L"删了", L"清空", L"格式化", L"抹掉",
    L"发送", L"发出", L"发给", L"回复消息", L"发消息", L"发邮件", L"提交", L"上传",
    L"付款", L"支付", L"下单", L"购买", L"转账", L"充值",
    L"卸载", L"关机", L"重启电脑", L"注销",
    L"注册表", L"组策略",
    L"覆盖原", L"覆盖保存", L"永久删除",
};

/// 危险动作词表 —— 英文按**词边界**匹配。
///
/// ⚠ 为什么必须按词边界而不是子串：英文的危险词都是短词，子串匹配会大面积误伤
/// （`send` 命中 `sender`、`format` 命中 `information`）。
/// 反过来也踩过：只写 `send message` / `send email` 这种**搭配**又太窄 ——
/// 实测漏掉了「press send / click send button」这类真实说法（用例
/// `partial_limiter_does_not_suppress` 抓到的就是这个洞）。
const wchar_t* const kDangerWordsEn[] = {
    L"send", L"delete", L"remove", L"erase", L"wipe", L"submit", L"upload",
    L"purchase", L"uninstall", L"shutdown", L"reboot", L"registry", L"format",
};

/// 明确否定：用户已经说了「别做这一步」。压掉误报，避免把闸问废。
///
/// ⚠ 只收**整句级别的、无歧义**的否定。「只填表」「only fill」这类**局部限定**
/// 不能收 —— 「只填表，然后点发送」里同时含限定词与危险词，收进来会把闸整个压掉，
/// 那才是真出事。宁可多问一句。
const wchar_t* const kNegations[] = {
    L"不要发送", L"不用发送", L"先别发送", L"别发送", L"不要发", L"先别发",
    L"不要删除", L"不要删", L"先别删", L"别删除", L"不要提交", L"先别提交",
    L"不要支付", L"不要下单", L"不要点发送", L"不要点提交", L"不要上传",
    L"do not send", L"don't send", L"do not delete", L"don't delete",
    L"do not remove", L"don't remove", L"do not submit", L"don't submit",
};

/// 英文按词边界找词：`send` 命中 "press send"，但不命中 "sender"。
bool ContainsEnglishWord(const std::wstring& low, const std::wstring& word) {
    if (word.empty()) return false;
    size_t pos = 0;
    while ((pos = low.find(word, pos)) != std::wstring::npos) {
        const size_t end = pos + word.size();
        const bool leftOk = (pos == 0)
            || !std::iswalpha(static_cast<wint_t>(low[pos - 1]));
        const bool rightOk = (end >= low.size())
            || !std::iswalpha(static_cast<wint_t>(low[end]));
        if (leftOk && rightOk) return true;
        pos = end;
    }
    return false;
}

std::atomic_bool g_desktopTaskCancel{false};

}  // namespace

bool ParseDesktopTaskOptions(const std::wstring& paramsJson,
    DesktopTaskOptions& out, std::wstring& err) {
    err.clear();
    json j;
    try {
        j = json::parse(ToUtf8(paramsJson));
    } catch (const json::parse_error&) {
        err = L"参数 JSON 解析失败";
        return false;
    }
    if (!j.is_object()) {
        err = L"参数必须是 JSON 对象";
        return false;
    }
    out.goal = ReadStr(j, "goal");
    if (out.goal.empty()) {
        err = L"runDesktopTask 需要 goal（要做什么，一句话说清）";
        return false;
    }
    if (j.contains("withImage") && j["withImage"].is_boolean()) {
        out.withImage = j["withImage"].get<bool>();
    }
    if (j.contains("maxSteps") && j["maxSteps"].is_number_integer()) {
        out.maxSteps = j["maxSteps"].get<int>();
    }
    if (j.contains("timeoutSec") && j["timeoutSec"].is_number_integer()) {
        out.timeoutSec = j["timeoutSec"].get<int>();
    }
    if (j.contains("contextMode") && j["contextMode"].is_number_integer()) {
        out.contextMode = j["contextMode"].get<int>();
    }
    if (j.contains("confirmed") && j["confirmed"].is_boolean()) {
        out.confirmed = j["confirmed"].get<bool>();
    }
    out.modelName = ReadStr(j, "modelName");

    // 夹紧到引擎能接受的范围：超时太小会当场失败，太大等于把助手挂死。
    if (out.timeoutSec < 10) out.timeoutSec = 10;
    if (out.timeoutSec > 600) out.timeoutSec = 600;
    if (out.maxSteps == 0) out.maxSteps = 12;
    if (out.maxSteps > 60) out.maxSteps = 60;
    if (out.contextMode < 0 || out.contextMode > 3) out.contextMode = 0;
    return true;
}

bool DesktopTaskNeedsConfirm(const std::wstring& goal) {
    const std::wstring low = LowerCopy(goal);
    if (low.empty()) return false;
    for (const auto* neg : kNegations) {
        if (low.find(LowerCopy(neg)) != std::wstring::npos) return false;
    }
    for (const auto* w : kDangerWordsCjk) {
        if (low.find(LowerCopy(w)) != std::wstring::npos) return true;
    }
    for (const auto* w : kDangerWordsEn) {
        if (ContainsEnglishWord(low, LowerCopy(w))) return true;
    }
    return false;
}

std::wstring DesktopTaskConfirmPrompt(const std::wstring& goal) {
    return L"[需用户确认] 这个任务包含**不可逆或对外的动作**，助手不能自己决定：\n"
        L"  " + goal + L"\n"
        L"请把上面这句话原样念给用户听，问清楚「是否现在就做」，**先不要调用任何工具**。\n"
        L"用户明确同意后，再调用 runDesktopTask 并把 confirmed 设为 true。\n"
        L"用户没有明确同意（包括含糊的「嗯」「看着办」）一律不要执行。";
}

ScriptAction BuildDesktopTaskAction(const DesktopTaskOptions& opts) {
    ScriptAction a{};
    a.type = ActionType::AiActionExecute;
    a.aiPrompt = opts.goal;
    a.aiWithImage = opts.withImage;
    a.aiMaxSteps = opts.maxSteps;
    a.aiTimeoutSec = opts.timeoutSec;
    a.aiContextMode = opts.contextMode;
    a.aiModelName = opts.modelName;
    a.aiOutputVarName = kResultVar;
    a.duration = 0.0;
    return a;
}

ScriptFileData BuildDesktopTaskScript(const DesktopTaskOptions& opts) {
    ScriptFileData data{};
    data.schemaVersion = 2;
    data.scriptName = L"AI助手桌面任务";
    data.actions.push_back(BuildDesktopTaskAction(opts));
    // 默认窗口/后台窗口模式（不绑定任何窗口）：助手任务的目标窗口由 AI 自己用
    // activateWindow / listWindows 找，绑定死在脚本头里反而会跑错窗口。
    return data;
}

const wchar_t* DesktopTaskResultVar() {
    return kResultVar;
}

std::wstring DesktopTaskTempScriptPath() {
    static std::atomic<unsigned long> counter{0};
    const unsigned long n = counter.fetch_add(1);
    // 前缀来自 utils 的**唯一来源**（`kAgentTaskTempScriptPrefix`）——
    // 它同时被「脚本库列表过滤」和「启动清扫」依赖，别在这里另写一份字面量。
    return ScriptsDir() + L"\\" + kAgentTaskTempScriptPrefix() + std::to_wstring(GetCurrentProcessId())
        + L"_" + std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(n) + L".json";
}

std::wstring SummarizeDesktopTaskResult(const std::wstring& raw) {
    std::wstring s = Trim(raw);
    if (s.empty()) return L"（AI 未给出结论）";

    // 去掉回放内部用的控制标记：它们对模型有意义，对用户是噪音。
    static const wchar_t* const kMarkers[] = {
        L"[EXECUTED]", L"[OBSERVE]", L"[结果]", L"[错误]", L"[EXECUTED][OBSERVE]",
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto* m : kMarkers) {
            const std::wstring marker = m;
            if (s.rfind(marker, 0) == 0) {
                s = Trim(s.substr(marker.size()));
                changed = true;
            }
        }
    }

    // 压平换行：助手面板是单行气泡，多行会把摘要撑爆
    std::wstring flat;
    flat.reserve(s.size());
    bool lastSpace = false;
    for (wchar_t c : s) {
        if (c == L'\r' || c == L'\n' || c == L'\t') {
            if (!lastSpace) { flat.push_back(L' '); lastSpace = true; }
            continue;
        }
        flat.push_back(c);
        lastSpace = (c == L' ');
    }
    flat = Trim(flat);
    if (flat.empty()) return L"（AI 未给出结论）";
    if (flat.size() > 400) flat = flat.substr(0, 400) + L"…";
    return flat;
}

void RequestDesktopTaskCancel() {
    g_desktopTaskCancel.store(true, std::memory_order_release);
}

void ClearDesktopTaskCancel() {
    g_desktopTaskCancel.store(false, std::memory_order_release);
}

bool DesktopTaskCancelRequested() {
    return g_desktopTaskCancel.load(std::memory_order_acquire);
}

}  // namespace agent
}  // namespace qst
