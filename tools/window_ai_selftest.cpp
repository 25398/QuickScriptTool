// =============================================================================
// window_ai_selftest.cpp — 窗口反代（B 类·界面反代）**纯逻辑**自检
//
// 只链 `window_ai_profile.cpp` + `qst_utils`：不起窗口、不碰 UIA、不碰键鼠。
//
// ⚠ 为什么这一层必须逐格钉住（每一格都对应一次真机上会发生的"错打到别处"）：
//   · 档案合并错 ⇒ 判据用的是**上一个客户端**的规则；
//   · 窗口匹配松 ⇒ 打到**别的程序**的窗口上；
//   · 输入框判据松 ⇒ 把提示词打进**代码编辑器**（Cursor 那种"编辑器+聊天面板"共存）；
//   · 回复提取错 ⇒ 把**上一轮回答**或**自己的提问**当成新回答；
//   · 稳定判据错 ⇒ 把半截流式回答当最终答案。
//   这些都**没有**"回读长度"那种现成证据可看，所以只能靠断言。
//
// 真机部分（到底能不能写进豆包客户端/Cursor）**不在本套**：
// 它必须由产品进程内的 `POST /qst/window-ai/probe` 跑（要 UIA + 前台 + 真控件树）。
// =============================================================================

#include "selftest_harness.h"

#include "web_ai/window_ai_profile.h"

#include <string>
#include <vector>

using namespace quickscript::webai;

namespace {

WindowAiUiNode Node(const wchar_t* type, const wchar_t* name, int id, int l, int t, int r,
    int b, bool focusable, bool valuePattern) {
    WindowAiUiNode n;
    n.controlType = type;
    n.name = name;
    n.controlTypeId = id;
    n.rect = RECT{ l, t, r, b };
    n.focusable = focusable;
    n.valuePattern = valuePattern;
    return n;
}

RECT Client(int l, int t, int r, int b) {
    return RECT{ l, t, r, b };
}

/// UTF-8 → UTF-16（自检要显示"为什么拒"的中文原因；逐字节加宽会乱码）
std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
        nullptr, 0);
    if (n <= 0) return L"(原因非 UTF-8)";
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

const WindowAiClientProfile* FindClient(const std::vector<WindowAiClientProfile>& v,
    const char* id) {
    for (const auto& c : v) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

// ── 1. 内置档案与类型名表 ───────────────────────────────────────────────────

void CaseBuiltinsCoverNamedClients() {
    const auto v = BuiltinWindowAiClients();
    const auto* doubao = FindClient(v, "doubao-app");
    const auto* cursor = FindClient(v, "cursor");
    const auto* codex = FindClient(v, "codex");
    const bool ok = doubao && cursor && codex
        && doubao->processNames.size() >= 1
        && cursor->input.requireHints          // ★ Cursor 必须**失效安全**
        && cursor->input.nameContains.empty()  // 默认未校准 ⇒ 一个候选都不给
        && codex->input.requireFocusable == false;
    selftest::Emit(L"builtin_clients_cover_doubao_cursor_codex", ok,
        (L"clients=" + std::to_wstring(v.size())).c_str());
}

void CaseControlTypeNameRoundTrip() {
    // 英文名 → id → 英文名 必须闭合（配置里写的是英文名）
    bool ok = true;
    std::wstring detail;
    for (const wchar_t* name : { L"Edit", L"Document", L"Button", L"Text", L"Pane", L"List",
             L"Custom", L"Window" }) {
        const int id = WindowAiControlTypeIdFromName(name);
        const wchar_t* back = WindowAiControlTypeName(id);
        if (id == 0 || !back || std::wstring(back) != name) {
            ok = false;
            detail += std::wstring(L"[") + name + L"→" + std::to_wstring(id) + L"]";
        }
    }
    // 反例：不存在/空名字必须给 0（不许瞎猜一个类型）
    if (WindowAiControlTypeIdFromName(L"NotAType") != 0) ok = false;
    if (WindowAiControlTypeIdFromName(L"") != 0) ok = false;
    selftest::Emit(L"control_type_name_round_trip", ok, detail.c_str());
}

// ── 2. 档案合并（用户覆盖）──────────────────────────────────────────────────

void CaseMergeOverride() {
    std::string err;
    // 覆盖内置档案的一个字段 + 新增一个客户端
    const std::string json =
        "{\"clients\":{"
        "  \"doubao-app\":{\"processNames\":[\"MyDoubao.exe\"],"
        "     \"input\":{\"nameContains\":[\"输入\"],\"requireHints\":true}},"
        "  \"my-client\":{\"label\":\"我的客户端\",\"processNames\":[\"Mine.exe\"],"
        "     \"input\":{\"controlTypes\":[\"Edit\"],\"pick\":\"lowest\"}}"
        "}}";
    const auto merged = MergeWindowAiProfiles(json, BuiltinWindowAiClients(), err);
    const auto* d = FindClient(merged, "doubao-app");
    const auto* m = FindClient(merged, "my-client");
    const bool ok = err.empty() && d && m
        && d->processNames.size() == 1 && d->processNames[0] == L"MyDoubao.exe"
        && d->input.requireHints && d->input.nameContains.size() == 1
        && d->input.nameContains[0] == L"输入"
        && m->label == "我的客户端" && m->input.pick == L"lowest"
        // 未提到的字段必须保留内置默认（否则覆盖一个字段会清掉其它判据）
        && d->input.controlTypes.size() == 2;
    selftest::Emit(L"profile_merge_override_keeps_defaults", ok,
        (L"err=" + std::wstring(err.begin(), err.end())).c_str());
}

void CaseMergeBadJsonKeepsBuiltins() {
    std::string err;
    const auto merged = MergeWindowAiProfiles("{ this is not json", BuiltinWindowAiClients(), err);
    const bool ok = !err.empty() && merged.size() == BuiltinWindowAiClients().size();
    selftest::Emit(L"profile_merge_bad_json_keeps_builtins", ok, L"");
}

void CaseFindClientByModelName() {
    const auto v = BuiltinWindowAiClients();
    const bool ok = FindWindowAiClient(v, "doubao-app") == FindWindowAiClient(v, "doubao-app")
        && FindWindowAiClient(v, "DOUBAO-APP") >= 0        // 大小写不敏感
        && FindWindowAiClient(v, "豆包客户端") >= 0          // 别名
        && FindWindowAiClient(v, "cursor") >= 0
        && FindWindowAiClient(v, "gpt-4o") < 0             // 不认识的一律 -1（不许兜底）
        && FindWindowAiClient(v, "") < 0;
    selftest::Emit(L"find_client_by_model_name", ok, L"");
}

// ── 3. 窗口匹配 ─────────────────────────────────────────────────────────────

void CaseWindowMatch() {
    const auto v = BuiltinWindowAiClients();
    const auto* d = FindClient(v, "doubao-app");
    const auto* c = FindClient(v, "cursor");
    if (!d || !c) { selftest::Emit(L"window_match_process_and_title", false, L"缺内置档案"); return; }
    const bool ok =
        // 进程名命中 + 标题含「豆包」
        WindowAiWindowMatches(*d, L"Doubao.exe", L"豆包 - 和你的 AI 助手聊天")
        // 进程名对但标题不含关键词 ⇒ 不算（防打到别的窗口）
        && !WindowAiWindowMatches(*d, L"Doubao.exe", L"设置")
        // 进程名不对 ⇒ 一律不算
        && !WindowAiWindowMatches(*d, L"chrome.exe", L"豆包")
        // 排除项优先于包含项（Cursor 的欢迎窗没有会话可读）
        && !WindowAiWindowMatches(*c, L"Cursor.exe", L"Welcome - Cursor")
        // 标题不限时，进程名命中即可
        && WindowAiWindowMatches(*c, L"Cursor.exe", L"main.cpp - my-project - Cursor")
        && !WindowAiWindowMatches(*c, L"code.exe", L"main.cpp - Cursor");
    selftest::Emit(L"window_match_process_and_title", ok, L"");
}

// ── 4. 输入框打分与挑选（最容易"打到编辑器"的一环）────────────────────────

void CaseInputScoreRejectsUnfit() {
    WindowAiInputSpec spec;   // 默认：Edit/Document，minWidth 120，minHeight 18，需可聚焦
    const RECT client = Client(0, 0, 1920, 1080);
    const bool ok =
        // 太小（会话里的一行文本）
        ScoreWindowAiInput(spec, Node(L"Text", L"hello", 50020, 100, 100, 300, 116, false, false), client) < 0
        // 不可聚焦
        && ScoreWindowAiInput(spec, Node(L"Document", L"", 50030, 100, 900, 1800, 1000, false, false), client) < 0
        // 类型不在名单里
        && ScoreWindowAiInput(spec, Node(L"Button", L"发送", 50000, 1700, 950, 1800, 1000, true, false), client) < 0
        // 合格：Edit + 够大 + 可聚焦 + 在下半部
        && ScoreWindowAiInput(spec, Node(L"Edit", L"输入消息", 50004, 100, 950, 1700, 1000, true, true), client) > 0;
    selftest::Emit(L"input_score_rejects_unfit", ok, L"");
}

void CaseInputRequireHintsFailsSafe() {
    // ★★ 本套最重要的一条：Cursor 那种"编辑器 + 聊天面板"共存的客户端里，
    //    **主编辑器也是 Document**，而且面积最大、也在下半部 —— 若判据只看类型+面积，
    //    就会把提示词打进**用户的代码文件**里（真·破坏性）。
    //    `requireHints` + 空名单 = 显式"未校准" ⇒ 必须**一个候选都不给**（失效安全）。
    WindowAiInputSpec spec;
    spec.requireHints = true;        // 名单留空
    const RECT client = Client(0, 0, 1920, 1080);
    const std::vector<WindowAiUiNode> nodes = {
        Node(L"Document", L"main.cpp", 50030, 0, 100, 1400, 1000, true, false),   // 编辑器（巨大）
        Node(L"Document", L"", 50030, 1420, 850, 1900, 1000, true, false),        // 聊天输入框
    };
    const int pick = PickWindowAiInput(spec, nodes, client);

    // 填上提示词之后必须能挑中（证明"失效安全"是可解除的，不是坏的）
    WindowAiInputSpec tuned = spec;
    tuned.automationIdContains = { L"chat-input" };
    std::vector<WindowAiUiNode> tunedNodes = nodes;
    tunedNodes[1].automationId = L"workbench.panel.chat.chat-input";
    const int pick2 = PickWindowAiInput(tuned, tunedNodes, client);
    const bool ok = pick < 0 && pick2 == 1;
    selftest::Emit(L"input_require_hints_fails_safe_until_calibrated", ok,
        (L"未校准=" + std::to_wstring(pick) + L" 校准后=" + std::to_wstring(pick2)).c_str());
}

void CaseInputPickPrefersComposerOverEditor() {
    // 调好判据后：聊天输入框（在下方、略小）必须赢过编辑器
    WindowAiInputSpec spec;
    spec.requireHints = true;
    spec.nameContains = { L"输入" };
    const RECT client = Client(0, 0, 1920, 1080);
    const std::vector<WindowAiUiNode> nodes = {
        Node(L"Document", L"main.cpp", 50030, 0, 100, 1400, 1000, true, false),
        Node(L"Edit", L"输入消息", 50004, 1420, 900, 1900, 980, true, true),
        Node(L"Text", L"输入历史", 50020, 1420, 200, 1900, 260, false, false),  // 只在名字里撞车
    };
    const int pick = PickWindowAiInput(spec, nodes, client);
    const bool ok = pick == 1;
    selftest::Emit(L"input_pick_prefers_composer_over_editor", ok,
        (L"pick=" + std::to_wstring(pick)).c_str());
}

void CasePlaceholderToleratesOcrGarble() {
    // ★★ 真机实测（2026-09-27）：OCR 把占位符**读花了** ——
    //   「发消息或按住空格说话…」被读成「发氵肖息或按任空格说话“」
    //   （"消"被拆成"氵肖"、"按住"读成"按任"）。
    //   精确匹配必然落空 ⇒ 空框被当成"用户草稿" ⇒ 整条链**永远拒绝工作**。
    const std::vector<std::wstring> hints{ L"发消息", L"按住空格", L"输入", L"问我" };
    const bool ok =
        WindowAiLooksLikePlaceholder(hints, L"发氵肖息或按任空格说话“")      // 实测那句
        && WindowAiLooksLikePlaceholder(hints, L"发消息或按住空格说话...")   // 正常
        // 反例：真草稿不能误判（既长、重合度也低）
        && !WindowAiLooksLikePlaceholder(hints, L"帮我写一个能自动点按钮的脚本，要点是每 500ms 点一次")
        // 反例：短但完全不相干
        && !WindowAiLooksLikePlaceholder(hints, L"QWER 循环");
    selftest::Emit(L"placeholder_tolerates_ocr_garble", ok, L"");
}

void CaseInputPickLowest() {
    WindowAiInputSpec spec;
    spec.pick = L"lowest";
    const RECT client = Client(0, 0, 1000, 1000);
    const std::vector<WindowAiUiNode> nodes = {
        Node(L"Edit", L"a", 50004, 10, 100, 400, 200, true, true),    // 上面
        Node(L"Edit", L"b", 50004, 10, 700, 300, 760, true, true),    // 下面（更小）
    };
    const bool ok = PickWindowAiInput(spec, nodes, client) == 1;
    selftest::Emit(L"input_pick_lowest_prefers_bottom", ok, L"");
}

// ── 5. 回复提取（"读到自己的提问"是这里最典型的错）──────────────────────────

void CaseExtractReplyAfterPrompt() {
    const std::wstring prompt = L"用一句话回答：1+1 等于几？";
    const std::wstring baseline = L"你好呀\n有什么可以帮你的吗？";
    const std::wstring after = baseline + L"\n" + prompt + L"\n1+1 等于 2。";
    const std::wstring reply = ExtractWindowAiReply(baseline, after, prompt, 12000);
    const bool ok = reply == L"1+1 等于 2。";
    selftest::Emit(L"extract_reply_after_prompt", ok, reply.c_str());
}

void CaseExtractReplyHandlesRepeatedPrompt() {
    // ★ 同一个问题问过第二遍时，会话里会出现**两次**相同的提问。
    //   必须取**最后一次**之后的内容（find 会取到第一段，那是上一轮的回答）。
    const std::wstring prompt = L"今天几号？";
    const std::wstring baseline = L"用户：" + prompt + L"\n助手：9 月 25 号。";
    const std::wstring after = baseline + L"\n用户：" + prompt + L"\n助手：9 月 26 号。";
    const std::wstring reply = ExtractWindowAiReply(baseline, after, prompt, 12000);
    const bool ok = reply.find(L"9 月 26 号") != std::wstring::npos
        && reply.find(L"9 月 25 号") == std::wstring::npos;
    selftest::Emit(L"extract_reply_uses_last_prompt_occurrence", ok, reply.c_str());
}

void CaseExtractReplyPrefixDiffFallback() {
    // 客户端把提问做了排版（加了「你」前缀之类）⇒ 找不到原样 prompt，退回公共前缀剥离
    const std::wstring baseline = L"A\nB\n";
    const std::wstring after = L"A\nB\n你：你好吗\n助手：我很好";
    const std::wstring reply = ExtractWindowAiReply(baseline, after, L"你好吗", 12000);
    const bool ok = reply.find(L"我很好") != std::wstring::npos;
    selftest::Emit(L"extract_reply_prefix_diff_fallback", ok, reply.c_str());
}

void CaseExtractReplyEmptyWhileWaiting() {
    // ★ 刚提交、回答还没出现（客户端只回显了提问）⇒ **必须返回空**，
    //   让调用方继续等。返回"提问之前的内容"就是把上一轮回答当新回答。
    const std::wstring baseline = L"历史：上一轮的回答是 42。";
    const std::wstring prompt = L"再算一次";
    const std::wstring after = baseline + L"\n" + prompt;
    const std::wstring reply = ExtractWindowAiReply(baseline, after, prompt, 12000);
    selftest::Emit(L"extract_reply_empty_while_waiting", reply.empty(), reply.c_str());
}

void CaseExtractReplyStripsEchoedInput() {
    // 有些客户端把**输入框里还没发出去的字**也算进整窗文本 ⇒ 尾部会挂着我们的 prompt
    const std::wstring baseline = L"旧会话";
    const std::wstring prompt = L"新问题";
    const std::wstring after = L"旧会话\n回答来了\n" + prompt;   // 尾部是输入框回显
    const std::wstring reply = ExtractWindowAiReply(baseline, after, prompt, 12000);
    const bool ok = reply.find(L"回答来了") != std::wstring::npos
        && reply.find(prompt) == std::wstring::npos;
    selftest::Emit(L"extract_reply_strips_echoed_input", ok, reply.c_str());
}

void CaseExtractReplyMatchesDespiteOcrSpacing() {
    // ★★ 本套第二条关键用例（真机上踩了两次）：
    //   OCR 会在数字/字母之间**插空格** —— 我们发的「…11+12 等于几？」
    //   在屏幕上被识别成「…1 1 + 1 2等于几？」。
    //   精确 rfind 必然落空 ⇒ 退回"公共前缀剥离" ⇒ 把整段会话（推荐问题、右侧栏）
    //   都当成回答（实测 replyChars=122 全是噪声）。
    const std::wstring prompt = L"请只回答一个数字：11+12 等于几？";
    const std::wstring baseline = L"豆包\n旧会话标题";
    const std::wstring after = baseline
        + L"\n请只回答一个数字：1 1 + 1 2等于几？\n23\n"
        + L"100 + 200等于几？\n对话产物〉\n技能〉";
    const std::wstring reply = ExtractWindowAiReply(baseline, after, prompt, 12000);
    // 必须从答案「23」开头（后面的推荐问题是可容忍的尾巴：工具协议靠标记取内容，
    // 多余文本不会被执行；但**绝不能**把提问之前的内容当回答）
    const bool ok = reply.rfind(L"23", 0) == 0
        && reply.find(prompt.substr(0, 6)) == std::wstring::npos;
    selftest::Emit(L"extract_reply_matches_despite_ocr_spacing", ok, reply.c_str());
}

void CaseCopyButtonPicksLowest() {
    // ★★ 剪贴板通道的关键判据：**用户自己那条消息下面也有「复制」按钮**。
    //   点错就把我们的提问复制回来，表现为"回答 = 提问"，而回执看着一切正常
    //   （剪贴板确实变了）。⇒ 必须取屏幕上**最靠下**的那个（= 最新回答的操作栏）。
    WindowAiReplySpec spec;
    const RECT client = Client(0, 0, 1800, 1200);
    const std::vector<WindowAiUiNode> nodes = {
        Node(L"Button", L"复制", 50000, 1200, 300, 1240, 340, true, false),   // 用户消息的复制（上）
        Node(L"Button", L"赞", 50000, 1250, 300, 1290, 340, true, false),     // 无关按钮
        Node(L"Button", L"复制", 50000, 1200, 900, 1240, 940, true, false),   // 回答的复制（下）
        Node(L"Button", L"复制全部", 50000, 300, 100, 380, 140, true, false),  // 工具栏（最上）
    };
    const int pick = PickWindowAiCopyButton(spec, nodes, client);
    // 反例：全改成不认识的名字 ⇒ 一个都不给（不许瞎点）
    std::vector<WindowAiUiNode> noHit = nodes;
    for (auto& n : noHit) n.name = L"某按钮";
    const int none = PickWindowAiCopyButton(spec, noHit, client);
    const bool ok = pick == 2 && none < 0;
    selftest::Emit(L"copy_button_picks_lowest_answer_row", ok,
        (L"pick=" + std::to_wstring(pick) + L" none=" + std::to_wstring(none)).c_str());
}

void CaseCopyRowButtonPicksLeftmostOfBottomRow() {
    // ★★ 真机实测（2026-09-27）：豆包客户端的消息操作栏 = **一排等间距的无名方形按钮**
    //   实测 x=851,902,953,1004,1055,1106,1157,1208，全部 36x36，间距 51px，
    //   最左那颗是「复制」、第二颗是「朗读」（唯一有名字的）。
    //   ⇒ 判据：按 y 分排、每排 ≥3 颗，取**最靠下**那排的**最左**一颗。
    //   ⚠ 往右第二颗之后是 赞/踩/分享/重新生成 —— 点错会**改变用户会话**（实测点到「踩」
    //     弹出了反馈弹窗），所以"取最左"这条不能松。
    WindowAiReplySpec spec;
    const RECT client = Client(395, 84, 2202, 1284);   // 实测窗口 1807x1200
    std::vector<WindowAiUiNode> nodes;
    // 上一条回答的操作栏（y=300）
    for (int k = 0; k < 8; ++k) {
        nodes.push_back(Node(L"Button", k == 1 ? L"朗读" : L"", 50000,
            851 + 51 * k, 300, 887 + 51 * k, 336, true, false));
    }
    // 最新回答的操作栏（y=551）—— 应该选这一排
    for (int k = 0; k < 8; ++k) {
        nodes.push_back(Node(L"Button", k == 1 ? L"朗读" : L"", 50000,
            851 + 51 * k, 551, 887 + 51 * k, 587, true, false));
    }
    // 干扰：推荐问题（Hyperlink，不是 Button）、输入区里的按钮（在输入框下方）
    nodes.push_back(Node(L"Hyperlink", L"10 + 10等于几？", 50000, 875, 995, 1169, 1022, true, false));
    nodes.push_back(Node(L"Button", L"发送", 50000, 1800, 1150, 1860, 1210, true, false));
    const int pick = PickWindowAiCopyRowButton(spec, nodes, client, 1100);
    // 反例：一排只有 2 颗（不成排）⇒ 不给
    std::vector<WindowAiUiNode> two;
    two.push_back(Node(L"Button", L"", 50000, 851, 551, 887, 587, true, false));
    two.push_back(Node(L"Button", L"", 50000, 902, 551, 938, 587, true, false));
    const int none = PickWindowAiCopyRowButton(spec, two, client, 1100);
    const bool ok = pick == 8 && none < 0;   // 第二排第一颗（下标 8）
    selftest::Emit(L"copy_row_button_leftmost_of_bottom_row", ok,
        (L"pick=" + std::to_wstring(pick) + L" none=" + std::to_wstring(none)).c_str());
}

void CaseClipboardReplyUsable() {
    const std::wstring prompt = L"请只回答一个数字：8+9 等于几？";
    std::string why;
    const bool ok =
        // 正常：变了、非空、不是提问
        WindowAiClipboardReplyUsable(L"旧内容", L"17", prompt, 12000, why)
        // 没变 ⇒ 复制按钮没点中
        && !WindowAiClipboardReplyUsable(L"旧内容", L"旧内容", prompt, 12000, why)
        // 空 ⇒ 没复制到
        && !WindowAiClipboardReplyUsable(L"旧内容", L"", prompt, 12000, why)
        // ★ 把提问复制回来了（点到了用户消息的复制按钮）⇒ 拒
        && !WindowAiClipboardReplyUsable(L"旧内容", prompt, prompt, 12000, why)
        // 忽略空白后基本等于提问 ⇒ 也拒
        && !WindowAiClipboardReplyUsable(L"旧内容", L"请只回答一个数字：8 + 9等于几？", prompt, 12000, why)
        // 回答里**包含**提问（客户端把整轮都复制了）⇒ 该接受
        && WindowAiClipboardReplyUsable(L"旧内容",
            L"请只回答一个数字：8+9 等于几？\n\n答案是 17。", prompt, 12000, why)
        // 大到离谱 ⇒ 拒（多半复制了整页）
        && !WindowAiClipboardReplyUsable(L"旧内容", std::wstring(60000, L'x'), prompt, 12000, why);
    selftest::Emit(L"clipboard_reply_usable_judgement", ok,
        (L"最后一次判定原因=" + W(why)).c_str());
}

void CaseExtractReplyCap() {
    const std::wstring prompt = L"P";
    const std::wstring big(5000, L'x');
    const std::wstring reply = ExtractWindowAiReply(L"", prompt + big, prompt, 100);
    selftest::Emit(L"extract_reply_respects_max_chars", reply.size() == 100,
        (L"len=" + std::to_wstring(reply.size())).c_str());
}

// ── 6. 稳定判据 ─────────────────────────────────────────────────────────────

void CaseStabilityNeedsTwoSameReads() {
    WindowAiStability st;
    const bool a = FeedWindowAiStability(st, L"半截");
    const bool b = FeedWindowAiStability(st, L"半截了");   // 变了 ⇒ 重新计数
    const bool c = FeedWindowAiStability(st, L"半截了");
    const bool ok = !a && !b && c;   // 第三次才稳（连续两次相同）
    // 空内容永远不算稳定（否则"还没出回答"会被当成"稳定且为空"）
    WindowAiStability st2;
    FeedWindowAiStability(st2, L"");
    const bool empty = FeedWindowAiStability(st2, L"");
    selftest::Emit(L"stability_needs_two_same_reads", ok && !empty,
        (L"a=" + std::to_wstring(a) + L" b=" + std::to_wstring(b) + L" c=" + std::to_wstring(c)
            + L" empty=" + std::to_wstring(empty)).c_str());
}

// 前向声明：本用例的定义在 kCases 之后（插在尾部），数组初始化要用到它的名字
void CaseReplyStripsOurOwnPromptEcho();

struct Case { const wchar_t* name; void (*fn)(); const wchar_t* meaning; };

const Case kCases[] = {
    {L"builtin_clients_cover_doubao_cursor_codex", CaseBuiltinsCoverNamedClients,
     L"内置客户端档案：豆包客户端 / Cursor / 终端 TUI"},
    {L"control_type_name_round_trip", CaseControlTypeNameRoundTrip,
     L"控件类型英文名 ↔ id 闭合（配置写英文名）"},
    {L"profile_merge_override_keeps_defaults", CaseMergeOverride,
     L"用户覆盖只改点名字段，其余保留内置默认"},
    {L"profile_merge_bad_json_keeps_builtins", CaseMergeBadJsonKeepsBuiltins,
     L"坏 JSON 不抛异常，退回内置档案"},
    {L"find_client_by_model_name", CaseFindClientByModelName,
     L"模型名 → 客户端（id/别名/大小写），不认识返回 -1"},
    {L"window_match_process_and_title", CaseWindowMatch,
     L"窗口匹配：进程名 + 标题包含/排除"},
    {L"input_score_rejects_unfit", CaseInputScoreRejectsUnfit,
     L"输入框判据拒掉太小/不可聚焦/类型不符"},
    {L"input_require_hints_fails_safe_until_calibrated", CaseInputRequireHintsFailsSafe,
     L"★ 未校准时失效安全：宁可 NO_INPUT，也不打进代码编辑器"},
    {L"input_pick_prefers_composer_over_editor", CaseInputPickPrefersComposerOverEditor,
     L"校准后聊天输入框必须赢过主编辑器"},
    {L"input_pick_lowest_prefers_bottom", CaseInputPickLowest, L"pick=lowest 取最靠下的"},
    {L"placeholder_tolerates_ocr_garble", CasePlaceholderToleratesOcrGarble,
     L"★ OCR 把占位符读花时仍要认出它是占位符（否则永远拒绝工作）"},
    {L"extract_reply_after_prompt", CaseExtractReplyAfterPrompt,
     L"回答 = 我们提问之后的内容"},
    {L"extract_reply_uses_last_prompt_occurrence", CaseExtractReplyHandlesRepeatedPrompt,
     L"★ 同一个问题问两遍时取**最后**一段（不许把上一轮回答当新回答）"},
    {L"extract_reply_prefix_diff_fallback", CaseExtractReplyPrefixDiffFallback,
     L"客户端排版改了提问 ⇒ 退回公共前缀剥离"},
    {L"extract_reply_matches_despite_ocr_spacing", CaseExtractReplyMatchesDespiteOcrSpacing,
     L"★ OCR 插空格时仍要能定位到提问（否则把整段会话当回答）"},
    {L"reply_strips_our_own_prompt_echo", CaseReplyStripsOurOwnPromptEcho,
     L"★ OCR 把提示词读花时仍要把「我们自己的提问回声」剔掉（否则把提问当回答）"},
    {L"extract_reply_empty_while_waiting", CaseExtractReplyEmptyWhileWaiting,
     L"★ 回答还没出现时必须返回空（让调用方继续等）"},
    {L"extract_reply_strips_echoed_input", CaseExtractReplyStripsEchoedInput,
     L"剥掉尾部「输入框回显」的提问"},
    {L"copy_button_picks_lowest_answer_row", CaseCopyButtonPicksLowest,
     L"★ 复制按钮取最靠下的（用户消息下也有复制，点错就把提问复制回来）"},
    {L"copy_row_button_leftmost_of_bottom_row", CaseCopyRowButtonPicksLeftmostOfBottomRow,
     L"★ 操作栏定位：最靠下那排的最左一颗=复制（往右是赞/踩/重新生成）"},
    {L"clipboard_reply_usable_judgement", CaseClipboardReplyUsable,
     L"★ 剪贴板内容能不能当回答（变了/非空/不是提问/不离谱）"},
    {L"extract_reply_respects_max_chars", CaseExtractReplyCap, L"回答长度上限"},
    {L"stability_needs_two_same_reads", CaseStabilityNeedsTwoSameReads,
     L"★ 稳定 = 连续两次读数相同；空内容永不算稳定"},
};

// ★ OCR 兜底最后一道保险：把"我们自己的提问回声"从回答里剔掉（2026-09-28）
//   背景：OCR 把提示词读花（`结构化请求`→`结构化讠青求`）⇒ 精确回声匹配落空 ⇒
//   整段提示词被当成回答返回（用户实测：回答里出现〖键鼠工坊·网页AI桥接〕…〖用户〗）。
void CaseReplyStripsOurOwnPromptEcho() {
    const std::wstring prompt =
        L"【键鼠工坊·网页 AI 桥接】下面这段是程序发来的结构化请求，不是普通聊天。\n"
        L"你只能看到被写进输入框的这部分内容；请直接给出结果，不要复述本说明。\n"
        L"===== 系统设定（必须始终遵守） =====\n"
        L"===== 对话 =====\n"
        L"【用户】\n"
        L"请只回答一个数字：9+9 等于几？";
    // 真机 OCR 读到的样子：花掉的提示词 + 真正的回答在最后
    const std::wstring after =
        L"9 + 9等于几？\n"
        L"〖键鼠工坊·网页AI桥接〕下面这段是程序发来的结构化讠青求，不是普涌聊天。\n"
        L"你只能看到被写进输入框的这部分内容；请直接给出结果，不要复述本说明。\n"
        L"=对话=\n"
        L"〖用户〗\n"
        L"18";
    const std::wstring got = ExtractWindowAiReply(L"", after, prompt, 4000);
    bool ok = got == L"18";
    // 只有回声、回答还没出现 ⇒ **必须返回空**（让调用方继续等，绝不把提问当回答）
    const std::wstring echoOnly =
        L"〖键鼠工坊·网页AI桥接〕下面这段是程序发来的结构化讠青求，不是普涌聊天。\n"
        L"=对话=\n"
        L"〖用户〗";
    const std::wstring none = ExtractWindowAiReply(L"", echoOnly, prompt, 4000);
    ok = ok && none.empty();
    // 正常回答（带回声的完整会话 + 回答）也要能留下回答
    const std::wstring ok2 = ExtractWindowAiReply(L"", L"18", prompt, 4000);
    ok = ok && ok2 == L"18";
    selftest::Emit(L"reply_strips_our_own_prompt_echo", ok,
        (got + L" | echoOnly=" + (none.empty() ? L"(空)" : none)).c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--json") selftest::gJson = true;
        else if (a == L"--list") list = true;
        else if (a == L"--help") {
            std::fwprintf(stdout, L"WindowAiSelfTest [--json] [--list]\n");
            return 0;
        }
    }
    selftest::InitUtf8Stdout();
    const size_t n = sizeof(kCases) / sizeof(kCases[0]);
    if (list) {
        std::vector<selftest::CaseInfo> infos;
        infos.reserve(n);
        for (const auto& c : kCases) infos.push_back({ c.name, L"default", c.meaning });
        selftest::PrintCaseList(L"WindowAiSelfTest", infos.data(), infos.size());
        return 0;
    }
    for (const auto& c : kCases) c.fn();
    selftest::EmitSummary();
    return selftest::ExitCode();
}
