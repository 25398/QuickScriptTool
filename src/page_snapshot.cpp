#include "page_snapshot.h"

#include "utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <limits>

namespace {

std::wstring JsonWide(const nlohmann::json& j, const char* key) {
    if (!j.contains(key)) return {};
    const auto& v = j[key];
    if (v.is_string()) return FromUtf8(v.get<std::string>());
    return {};
}

int JsonInt(const nlohmann::json& j, const char* key, int def = 0) {
    if (!j.contains(key)) return def;
    const auto& v = j[key];
    if (v.is_number_integer()) return v.get<int>();
    if (v.is_number_float()) return static_cast<int>(std::lround(v.get<double>()));
    return def;
}

double JsonNum(const nlohmann::json& j, const char* key, double def = 0.0) {
    if (!j.contains(key)) return def;
    const auto& v = j[key];
    if (v.is_number_float()) return v.get<double>();
    if (v.is_number_integer()) return static_cast<double>(v.get<int>());
    return def;
}

bool JsonBool(const nlohmann::json& j, const char* key, bool def = false) {
    if (!j.contains(key)) return def;
    const auto& v = j[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number_integer()) return v.get<int>() != 0;
    return def;
}

std::wstring LowerCopy(std::wstring s) {
    for (auto& c : s) {
        if (c >= L'A' && c <= L'Z')
            c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return s;
}

std::wstring ExtractUrlHostLower(const std::wstring& url) {
    const std::wstring u = LowerCopy(url);
    size_t start = 0;
    const size_t scheme = u.find(L"://");
    if (scheme != std::wstring::npos) start = scheme + 3;
    else if (u.size() >= 2 && u[0] == L'/' && u[1] == L'/') start = 2;
    const size_t end = u.find_first_of(L"/?#", start);
    std::wstring host = u.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    const size_t at = host.find(L'@');
    if (at != std::wstring::npos) host = host.substr(at + 1);
    const size_t colon = host.find(L':');
    if (colon != std::wstring::npos) host = host.substr(0, colon);
    return host;
}

std::wstring ExtractUrlPathLower(const std::wstring& url) {
    const std::wstring u = LowerCopy(url);
    size_t start = 0;
    const size_t scheme = u.find(L"://");
    if (scheme != std::wstring::npos) start = scheme + 3;
    else if (u.size() >= 2 && u[0] == L'/' && u[1] == L'/') start = 2;
    else if (!u.empty() && u[0] == L'/') {
        const size_t end = u.find_first_of(L"?#");
        return u.substr(0, end == std::wstring::npos ? std::wstring::npos : end);
    }
    const size_t slash = u.find(L'/', start);
    if (slash == std::wstring::npos) return L"/";
    const size_t end = u.find_first_of(L"?#", slash);
    return u.substr(slash, end == std::wstring::npos ? std::wstring::npos : end - slash);
}

std::wstring ExtractUrlPathOriginal(const std::wstring& url) {
    const std::wstring u = LowerCopy(url);
    size_t start = 0;
    const size_t scheme = u.find(L"://");
    if (scheme != std::wstring::npos) start = scheme + 3;
    else if (u.size() >= 2 && u[0] == L'/' && u[1] == L'/') start = 2;
    else if (!url.empty() && url[0] == L'/') {
        const size_t end = url.find_first_of(L"?#");
        return url.substr(0, end == std::wstring::npos ? std::wstring::npos : end);
    }
    const size_t slash = u.find(L'/', start);
    if (slash == std::wstring::npos) return L"/";
    const size_t end = u.find_first_of(L"?#", slash);
    return url.substr(slash, end == std::wstring::npos ? std::wstring::npos : end - slash);
}

std::wstring CompactPageHref(std::wstring href, const std::wstring& pageUrl) {
    href = Trim(href);
    if (href.empty()) return {};
    const std::wstring lower = LowerCopy(href);
    if (lower.rfind(L"javascript:", 0) == 0 || lower.rfind(L"mailto:", 0) == 0)
        return {};
    std::wstring host;
    std::wstring path;
    if (lower.rfind(L"https://", 0) == 0 || lower.rfind(L"http://", 0) == 0
        || lower.rfind(L"//", 0) == 0) {
        host = ExtractUrlHostLower(href);
        path = ExtractUrlPathOriginal(href);
    } else if (href[0] == L'/') {
        const size_t cut = href.find_first_of(L"?#");
        path = (cut == std::wstring::npos) ? href : href.substr(0, cut);
    } else {
        if (href.size() > 48) href = href.substr(0, 45) + L"...";
        return href;
    }
    const std::wstring pageHost = ExtractUrlHostLower(pageUrl);
    if (!host.empty() && (pageHost.empty() || host != pageHost)) {
        std::wstring compact = L"//" + host + (path.empty() ? L"/" : path);
        if (compact.size() > 64) compact = compact.substr(0, 61) + L"...";
        return compact;
    }
    if (path.empty()) path = L"/";
    if (path.size() > 48) path = path.substr(0, 45) + L"...";
    return path;
}

}  // namespace

PageKind ClassifyPageKind(double canvasRatio, int interactiveCount) {
    if (canvasRatio >= 0.50 && interactiveCount <= 6) return PageKind::Canvas;
    if (canvasRatio >= 0.22) return PageKind::Mixed;
    return PageKind::Dom;
}

const wchar_t* PageKindName(PageKind kind) {
    switch (kind) {
    case PageKind::Dom: return L"dom";
    case PageKind::Mixed: return L"mixed";
    case PageKind::Canvas: return L"canvas";
    default: return L"";
    }
}

PageKind ParsePageKindName(const std::wstring& name) {
    if (name == L"dom") return PageKind::Dom;
    if (name == L"mixed") return PageKind::Mixed;
    if (name == L"canvas") return PageKind::Canvas;
    return PageKind::Unknown;
}

bool LooksLikePageSnapshotRef(const std::wstring& ref) {
    if (ref.size() < 2 || ref.size() > 5 || ref[0] != L'e') return false;
    if (ref[1] < L'1' || ref[1] > L'9') return false;
    for (size_t i = 2; i < ref.size(); ++i) {
        if (ref[i] < L'0' || ref[i] > L'9') return false;
    }
    return true;
}

PageSnapshot ParsePageSnapshotJson(const std::string& jsonUtf8) {
    PageSnapshot snap;
    if (jsonUtf8.empty()) {
        snap.error = L"空响应";
        return snap;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(jsonUtf8);
    } catch (...) {
        snap.error = L"JSON 解析失败";
        return snap;
    }
    if (!j.is_object()) {
        snap.error = L"响应不是对象";
        return snap;
    }
    if (j.contains("ok") && j["ok"].is_boolean() && !j["ok"].get<bool>()) {
        snap.error = JsonWide(j, "message");
        if (snap.error.empty()) snap.error = JsonWide(j, "error");
        if (snap.error.empty()) snap.error = L"扩展返回失败";
        return snap;
    }
    snap.url = JsonWide(j, "url");
    snap.title = JsonWide(j, "title");
    snap.canvasRatio = JsonNum(j, "canvasRatio", 0.0);
    snap.interactive = JsonInt(j, "interactive", 0);
    snap.vw = JsonInt(j, "vw", 0);
    snap.vh = JsonInt(j, "vh", 0);
    snap.scrollY = JsonInt(j, "scrollY", 0);
    snap.pageHeight = JsonInt(j, "pageHeight", 0);
    snap.contentInView = j.contains("contentInView") ? JsonInt(j, "contentInView", 0) : -1;
    snap.queryHits = j.contains("queryHits") ? JsonInt(j, "queryHits", -1) : -1;
    // ★覆盖度（缺字段 = 旧扩展 → -1/0，渲染层据此不谎报「完整」）
    snap.enumerateTotal = j.contains("enumerateTotal") ? JsonInt(j, "enumerateTotal", -1) : -1;
    snap.offset = j.contains("offset") ? JsonInt(j, "offset", 0) : 0;
    snap.nextOffset = j.contains("nextOffset") ? JsonInt(j, "nextOffset", 0) : 0;
    snap.query = JsonWide(j, "query");
    snap.skippedHtml = JsonBool(j, "skippedHtml", false);
    snap.attachedNow = JsonBool(j, "attachedNow", false);
    snap.kind = ParsePageKindName(JsonWide(j, "pageKind"));
    if (snap.kind == PageKind::Unknown)
        snap.kind = ClassifyPageKind(snap.canvasRatio, snap.interactive);
    if (j.contains("nodes") && j["nodes"].is_array()) {
        for (const auto& n : j["nodes"]) {
            if (!n.is_object()) continue;
            PageSnapshotNode node;
            node.ref = JsonWide(n, "ref");
            node.group = JsonWide(n, "group");
            node.role = JsonWide(n, "role");
            node.name = JsonWide(n, "name");
            node.path = JsonWide(n, "path");
            node.value = JsonWide(n, "value");
            node.href = CompactPageHref(JsonWide(n, "href"), snap.url);
            node.checked = JsonBool(n, "checked", false);
            node.x = JsonInt(n, "x");
            node.y = JsonInt(n, "y");
            node.w = JsonInt(n, "w");
            node.h = JsonInt(n, "h");
            node.screenX = JsonInt(n, "sx");
            node.screenY = JsonInt(n, "sy");
            if (n.contains("inView"))
                node.inView = JsonBool(n, "inView", true);
            else
                node.inView = snap.vh <= 0
                    || (node.y + (node.h > 0 ? node.h : 1) > 0 && node.y < snap.vh);
            // absent = 内容完全在视口之外。旧扩展不给这个字段 ⇒ 留 -1，
            // 由 PageSnapshotNodeAbsent() 回落到 y/vh 推断。
            if (n.contains("absent"))
                node.absent = JsonBool(n, "absent", false) ? 1 : 0;
            if (!LooksLikePageSnapshotRef(node.ref)) continue;
            snap.nodes.push_back(std::move(node));
        }
    }
    if (snap.kind == PageKind::Canvas && snap.nodes.empty())
        snap.skippedHtml = true;
    return snap;
}

bool PageSnapshotNodeInView(const PageSnapshotNode& n, const PageSnapshot& snap) {
    (void)snap;
    return n.inView;
}

bool PageSnapshotNodeAbsent(const PageSnapshotNode& n, const PageSnapshot& snap) {
    // 新扩展会直接给 absent（与视口完全不相交）。优先用它 ——
    // 「在不在可视区」由扩展按真实视口算，比宿主拿 y/h 猜可靠。
    if (n.absent >= 0) return n.absent != 0;
    // 旧扩展不给 absent：只能从坐标推，且**推不出「完全不相交」**，
    // 只能推出「明显在下方」。宁可少报也不要误报成「屏外」——
    // 误报会把可点的节点说成「要滚动才能点」，反而误导模型。
    if (snap.vh <= 0) return false;
    return n.y >= snap.vh;
}

int CountPageSnapshotInViewMainContent(const PageSnapshot& snap) {
    if (snap.contentInView >= 0) return snap.contentInView;
    const int header = snap.vh > 0 ? (std::min)(140, snap.vh * 18 / 100) : 140;
    int n = 0;
    for (const auto& node : snap.nodes) {
        if (!PageSnapshotNodeInView(node, snap)) continue;
        if (node.role == L"tab" || node.role == L"searchbox") continue;
        if (node.y < header && (node.h <= 0 || node.h < 80)) continue;
        if (!node.href.empty() && node.h >= 48) {
            ++n;
            continue;
        }
        if (node.name.size() >= 4) ++n;
    }
    return n;
}

namespace {

std::wstring FormatSnapshotPageCoverage(const PageSnapshot& snap, int inViewN, int offN) {
    std::wstring s = L"可视";
    s += std::to_wstring(inViewN);
    s += L" · 屏外";
    s += std::to_wstring(offN);
    if (snap.pageHeight > 0 && snap.vh > 0) {
        const int rest = snap.pageHeight - snap.scrollY - snap.vh;
        if (rest <= snap.vh / 10 && offN == 0) {
            s += L" · 已到页底";
        } else if (rest > 0) {
            const int tenths = (rest * 10 + snap.vh / 2) / snap.vh;
            s += L" · 下";
            if (tenths < 1) {
                s += L"0屏";
            } else if (tenths % 10 == 0) {
                s += std::to_wstring(tenths / 10);
                s += L"屏";
            } else {
                s += std::to_wstring(tenths / 10);
                s += L".";
                s += std::to_wstring(tenths % 10);
                s += L"屏";
            }
        }
    }
    s += L"\n";
    return s;
}

std::wstring FormatSnapshotNodeLine(const PageSnapshotNode& n) {
    std::wstring line = L"- ";
    line += n.role.empty() ? L"generic" : n.role;
    if (!n.name.empty()) {
        line += L" \"";
        line += n.name;
        line += L"\"";
    }
    if (!n.href.empty()) {
        line += L" href=";
        line += n.href;
    }
    if (!n.path.empty()) {
        line += L" in \"";
        line += n.path;
        line += L"\"";
    }
    if (!n.value.empty()) {
        line += L" =\"";
        line += n.value;
        line += L"\"";
    }
    if (n.checked) line += L" checked";
    line += L" [ref=";
    line += n.ref;
    if (n.w > 0 && n.h > 0) {
        line += L" @";
        line += std::to_wstring(n.x);
        line += L",";
        line += std::to_wstring(n.y);
    }
    line += L"]\n";
    return line;
}

}  // namespace


// ★★ **树的能力摘要**（2026-10-02，用户要求"通用性"）：任何 DOM 页都成立的事实 ——
//   树里**已经列着**的可交互节点按角色计数给模型看：要选/点哪个，直接 clickRef(ref)
//   （0 次识图）；树里没有才 locateAndClick。纯事实，不替模型决定点什么。
//   ⚠ 判据只看节点角色计数（≥3 才说），**不看域名/站点** —— 通用机制，不是专项补丁。
bool IsClickableSnapshotRole(const std::wstring& role) {
    return role == L"button" || role == L"link" || role == L"menuitem" || role == L"tab"
        || role == L"option" || role == L"checkbox" || role == L"radio" || role == L"switch";
}
void FormatPageSnapshotCapabilityHint(const PageSnapshot& snap, std::wstring& out) {
    int radios = 0, options = 0, buttons = 0, links = 0, checks = 0, others = 0;
    for (const auto& n : snap.nodes) {
        if (!LooksLikePageSnapshotRef(n.ref)) continue;
        const std::wstring r = LowerCopy(n.role);
        if (r == L"radio") ++radios;
        else if (r == L"option") ++options;
        else if (r == L"button") ++buttons;
        else if (r == L"link") ++links;
        else if (r == L"checkbox") ++checks;
        else if (IsClickableSnapshotRole(n.role)) ++others;
    }
    const int total = radios + options + buttons + links + checks + others;
    if (total < 3) return;
    out += L"★本树已列出可交互节点 " + std::to_wstring(total) + L" 个";
    if (radios) out += L"（radio " + std::to_wstring(radios);
    if (options) out += L"、option " + std::to_wstring(options);
    if (buttons) out += L"、button " + std::to_wstring(buttons);
    if (links) out += L"、link " + std::to_wstring(links);
    if (checks) out += L"、checkbox " + std::to_wstring(checks);
    if (others) out += L"、其它可点 " + std::to_wstring(others);
    if (radios || options || buttons || links || checks) out += L"）";
    out += L"：**目标在树里就直接 clickRef(ref)（0 次识图），别去截图/猜坐标**；"
           L"树里没有才 observePage 重取或 locateAndClick。\n";
}
std::wstring FormatPageSnapshotForAgent(const PageSnapshot& snap, size_t maxChars) {
    if (maxChars < 200) maxChars = 200;
    std::wstring url = snap.url;
    if (url.size() > 96) url = url.substr(0, 93) + L"...";
    std::wstring title = snap.title;
    if (title.size() > 40) title = title.substr(0, 37) + L"...";

    std::wstring out = L"[";
    out += PageKindName(snap.kind);
    out += L"] ";
    if (!title.empty()) {
        out += title;
        out += L" | ";
    }
    if (!url.empty()) {
        out += url;
        out += L" | ";
    }
    if (snap.vw > 0 && snap.vh > 0) {
        out += std::to_wstring(snap.vw);
        out += L"x";
        out += std::to_wstring(snap.vh);
        out += L" ";
    }
    wchar_t ratioBuf[32]{};
    swprintf_s(ratioBuf, L"canvas=%.2f n=%d", snap.canvasRatio, snap.interactive);
    out += ratioBuf;
    out += L"\n";

    if (snap.kind == PageKind::Canvas || snap.skippedHtml) {
        out += L"禁止抓 HTML。请 locateAndClick / 找图。进游戏后人手或脚本接。\n";
        return out;
    }
    if (snap.kind == PageKind::Mixed) {
        out += L"外壳优先 clickRef/typeRef；树上没有则 locateAndClick。旧 ref 已作废。\n";
    } else {
        out += L"搜人/搜词用 searchOnPage(query)。树上 clickRef/typeRef；没有则 locateAndClick。"
            L"勿拼搜索框/Enter。旧 ref 已作废。\n";
    }
    if (snap.queryHits == 0) {
        std::wstring q = snap.query;
        if (q.size() > 24) q = q.substr(0, 24);
        if (q.empty()) q = L"该关键字";
        out += L"树上无「" + q + L"」：locateAndClick 识图兜底，勿空滚或乱点其它 ref。\n";
    }
    // ★★ **列表顺序 ≠ 时间顺序**（2026-09-29，用户实测报障后加）—— 通用事实，不是站点规则。
    //
    //   实测：任务要「主页**最新**的一个视频」，模型直接点了树上 **第一个** ref（e1），
    //   而列表第一项往往是**置顶/推荐**（那条是 2021 年的代表作）⇒ 整轮跑偏、反复重来。
    //   ⚠ 判据只看"这一页有多个可点条目"（节点数），**不看域名/URL 形状** ——
    //     批 D 删掉的正是那种站点专属启发式（docs §47）。这条是任何列表页都成立的事实。
    //   ⚠ 同时点明"树里可能没有时间字段"，与「判断不了就用图兜底」的取舍对齐：
    //     模型该做的是**看每项的时间/序号**，看不到就按图确认，而不是默认第一项。
    if (snap.nodes.size() >= 8) {
        out += L"★列表类页面：**树上第一项不一定是「最新」**（可能是置顶/推荐/广告）。"
               L"要「最新/最早/第 N 个」就得看每项的**时间或序号**；"
               L"树里没有这些字段时**看截图确认**，别默认第一项就是你要的那个。\n";
    }
    FormatPageSnapshotCapabilityHint(snap, out);
    // ★批 D 删掉的三段**站点专属**文案（docs §47 / D1，具体符号名见该节）：
    //   ① `列表第1项=eN（…）。clickRef 该项，禁止滚动去点别的卡。`
    //      —— 判据 =「重复卡片网格的第 1 项」+ 按 URL 形状判「是不是播放页」，
    //      消费点是「别点别的卡 / 别滚动」两条静默拒绝，已一并删除；
    //   ② `播放页：工具栏按钮优先 clickRef…相关推荐不是目标。`（视频站播放页专属建议）；
    //   ③ 搜索结果页/用户空间两段（判据同样是 URL 形状）。
    //   留在树文本里的只有**观测到的事实**（可视区/屏外/节点清单），怎么操作由模型自己定。

    int inViewN = 0;
    int offN = 0;
    // ★「屏外」的两种口径必须分开，否则回执自洽得看不出丢东西（实测教训）：
    //    ① **本次返回的节点里**有多少在屏外（offN）。
    //    ② **根本没返回**的节点（enumerateTotal - offset - nodes.size()）。
    //    上一版只报 ①，而扩展那时会把屏外元素全部预筛掉 ⇒ ① 恒为 0，
    //    回执永远写「可视23 · 屏外0」，看起来像一个完整的页面。
    // ★「可视 N · 屏外 M」这两个数必须**口径分开**：
    //    · inViewN = 与视口相交的节点（横跨边界也算看得见，坐标可点）；
    //    · offN    = **内容完全在视口之外**的节点（要滚动才看得见）。
    //   旧实现把 offN 定义成 `!inView`，而扩展那时又把屏外元素全预筛掉了
    //   ⇒ offN 恒为 0，回执永远写「可视23 · 屏外0」。这个数字**自洽得看不出丢东西**，
    //    模型据此以为「整页就这 23 个」，退化成盲滚 + 截图（实测烧掉 3.77 CNY）。
    //   现在扩展会把屏外节点一并上报（absent=true），这两个数才是真的。
    for (const auto& n : snap.nodes) {
        if (PageSnapshotNodeInView(n, snap)) ++inViewN;
        if (PageSnapshotNodeAbsent(n, snap)) ++offN;
    }
    if (!snap.nodes.empty())
        out += FormatSnapshotPageCoverage(snap, inViewN, offN);

    // ★★ 「这棵树是不是被砍过」必须**在树的开头**说清楚。
    //    实测教训（超星 50 题作业页）：扩展在排序后把候选砍到 64 条，一道多选题
    //    连题干带选项就吃掉 5~6 条 ⇒ 只有前 1~2 题进树，第 3 题起全没了。
    //    而当时的回执只有 `n=21` +「屏外0 · 下17.6屏」—— 树**自洽**，
    //    看不出丢了 48 道题，模型于是退化成「盲滚 + 截图抄」，还抄不全。
    //    结论：**丢了多少必须由系统说出来，不能指望模型从计数里悟**。
    if (snap.enumerateTotal >= 0) {
        const int total = snap.enumerateTotal;
        const int got = static_cast<int>(snap.nodes.size());
        const int from = snap.offset;
        if (total > got || from > 0) {
            out += L"⚠ 本页共 " + std::to_wstring(total) + L" 个可枚举控件，本次给的是第 "
                + std::to_wstring(from + 1) + L"~" + std::to_wstring(from + got) + L" 条";
            if (snap.nextOffset > 0) {
                out += L"（**没给完**：用 observePage(offset=" + std::to_wstring(snap.nextOffset)
                    + L") 接着取，或用 query 缩小范围）";
            } else {
                out += L"（已到末尾）";
            }
            out += L"。\n";
        } else if (total == got) {
            // 完整也要明说：否则模型无法区分「真的只有这么点」和「被砍了」。
            out += L"（本页 " + std::to_wstring(total) + L" 个可枚举控件，已全部列出）\n";
        }
    } else if (!snap.nodes.empty()) {
        // ★★ 旧扩展不给 enumerateTotal（-1）⇒ **覆盖度是未知的，不是「完整」**。
        //    实测这一条极其要命：扩展侧 `isVisible` 把屏外元素全滤了，
        //    于是 `nodes.size()` 只有可视区那 20~24 个，而回执写「可视23 · 屏外0」——
        //    树自洽、看不出丢东西，模型只能靠「下17.6屏」瞎猜，
        //    退化成「滚一下、换棵树、再截张图」，21 次截屏就这么烧掉的。
        //    ⇒ 不知道就必须说不知道，并且给出**唯一能拿到全量的办法**。
        out += L"⚠ 本次只拿到可视区的 " + std::to_wstring(snap.nodes.size())
            + L" 个控件；**本页到底共多少个，当前扩展没上报**（覆盖度未知，不要当作已看全）。"
            L"要枚举全量请用 searchOnPage(query) 缩小范围，或滚动后重新 observePage。\n";
    }

    auto appendLine = [&](const std::wstring& line) -> bool {
        if (out.size() + line.size() + 18 > maxChars) {
            out += L"…(截断)\n";
            return false;
        }
        out += line;
        return true;
    };

    bool truncated = false;
    if (offN > 0) {
        if (!appendLine(L"可视区:\n")) truncated = true;
        if (!truncated) {
            for (const auto& n : snap.nodes) {
                if (!PageSnapshotNodeInView(n, snap)) continue;
                if (!appendLine(FormatSnapshotNodeLine(n))) {
                    truncated = true;
                    break;
                }
            }
        }
        if (!truncated && !appendLine(L"屏外:\n")) truncated = true;
        if (!truncated) {
            for (const auto& n : snap.nodes) {
                if (PageSnapshotNodeInView(n, snap)) continue;
                if (!appendLine(FormatSnapshotNodeLine(n))) {
                    truncated = true;
                    break;
                }
            }
        }
    } else {
        for (const auto& n : snap.nodes) {
            if (!appendLine(FormatSnapshotNodeLine(n))) break;
        }
    }
    if (snap.nodes.empty() && !snap.skippedHtml)
        out += L"（无可交互节点；滚动露出或改 locateAndClick）\n";
    if (out.size() > maxChars)
        out.resize(maxChars);
    return out;
}

namespace {

bool PathLooksLikeSiteApi(const std::wstring& lowerUrl) {
    auto has = [&](const wchar_t* needle) {
        return lowerUrl.find(needle) != std::wstring::npos;
    };
    if (has(L"/x/web-interface") || has(L"/x/polymer") || has(L"/x/v2/")
        || has(L"/graphql") || has(L"/ajax/") || has(L"/openapi")
        || has(L"/rpc/") || has(L"/internal/"))
        return true;
    if (has(L"/api/") || has(L"/api?") || has(L"/api."))
        return true;
    const size_t q = lowerUrl.find(L'?');
    std::wstring path = q == std::wstring::npos ? lowerUrl : lowerUrl.substr(0, q);
    if (path.size() >= 5 && path.compare(path.size() - 5, 5, L".json") == 0)
        return true;
    return false;
}

}  // namespace

std::wstring ExtractUrlSiteKey(const std::wstring& url) {
    std::wstring host = ExtractUrlHostLower(url);
    if (host.empty()) return {};
    if (host.size() >= 4 && host.compare(0, 4, L"www.") == 0)
        host = host.substr(4);
    if (host.find(L'.') == std::wstring::npos) return host;
    auto ends = [&](const wchar_t* sfx) {
        const size_t n = std::char_traits<wchar_t>::length(sfx);
        return host.size() > n && host.compare(host.size() - n, n, sfx) == 0;
    };
    if (ends(L".com.cn") || ends(L".net.cn") || ends(L".org.cn") || ends(L".co.uk")) {
        size_t dots = 0;
        size_t cut = host.size();
        for (size_t i = host.size(); i > 0; --i) {
            if (host[i - 1] == L'.') {
                ++dots;
                if (dots == 3) {
                    cut = i;
                    break;
                }
            }
        }
        if (dots >= 3) return host.substr(cut);
        return host;
    }
    size_t dots = 0;
    size_t cut = 0;
    for (size_t i = host.size(); i > 0; --i) {
        if (host[i - 1] == L'.') {
            ++dots;
            if (dots == 2) {
                cut = i;
                break;
            }
        }
    }
    if (dots >= 2) return host.substr(cut);
    return host;
}

bool LooksLikeNonUserFacingWebUrl(const std::wstring& url) {
    if (url.empty()) return false;
    const std::wstring host = ExtractUrlHostLower(url);
    if (host.rfind(L"api.", 0) == 0 || host.find(L".api.") != std::wstring::npos)
        return true;
    if (host.rfind(L"graphql.", 0) == 0 || host.rfind(L"gateway.", 0) == 0)
        return true;
    return PathLooksLikeSiteApi(LowerCopy(url));
}

// ★批 D 删除的站点专属启发式（docs §47 / D1，符号名见该节）：某站空间 URL（含空根路径、
//   纯数字 UID 两种变体，以及只服务它的「路径是不是纯数字 UID」判据）、播放页 URL 形状、
//   「用户主页才直接导航」、快照里的「列表第1项」、搜索结果页 URL 形状。
//   判据里出现具体域名/URL 形状 ⇒ 换个网站就坏（与「对着测试用例下刀」同形，D-c）。

std::wstring SnapshotRefMatchingLocateTarget(const PageSnapshot& snap, const std::wstring& target) {
    const std::wstring t = Trim(target);
    if (t.size() < 2) return {};
    std::wstring best;
    int bestScore = -1;
    for (const auto& n : snap.nodes) {
        const std::wstring name = Trim(n.name);
        if (name.size() < 2) continue;
        if (t.find(name) == std::wstring::npos && name.find(t) == std::wstring::npos)
            continue;
        int score = static_cast<int>(name.size());
        if (n.role == L"button" || n.role == L"checkbox" || n.role == L"switch")
            score += 1000;
        // ★原先还有一条「播放页 URL 就降权 800」——按 URL 形状判站点内容类型的启发式，
        //   已删（批 D，docs §47）。
        if (score > bestScore) {
            bestScore = score;
            best = n.ref;
        }
    }
    return best;
}

bool PageUrlsSameDocument(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return ExtractUrlHostLower(a) == ExtractUrlHostLower(b)
        && ExtractUrlPathLower(a) == ExtractUrlPathLower(b);
}

std::wstring ResolvePageNavigationUrl(const std::wstring& href, const std::wstring& pageUrl) {
    const std::wstring h = Trim(href);
    if (h.empty() || h[0] == L'#') return {};
    const std::wstring low = LowerCopy(h);
    if (low.rfind(L"javascript:", 0) == 0 || low.rfind(L"mailto:", 0) == 0)
        return {};

    std::wstring abs;
    if (low.rfind(L"https://", 0) == 0 || low.rfind(L"http://", 0) == 0)
        abs = h;
    else if (low.rfind(L"//", 0) == 0)
        abs = L"https:" + h;
    else if (h[0] == L'/') {
        const std::wstring pageHost = ExtractUrlHostLower(pageUrl);
        if (pageHost.empty()) return {};
        const size_t cut = h.find_first_of(L"?#");
        abs = L"https://" + pageHost + (cut == std::wstring::npos ? h : h.substr(0, cut));
    } else {
        return {};
    }

    const size_t hash = abs.find(L'#');
    if (hash != std::wstring::npos) abs = abs.substr(0, hash);
    // ⚠ `LooksLikeNonUserFacingWebUrl` 这条保留（**非**站点专属：api./graphql./gateway./.json
    //   是全网站点的通用形状）；批 D 只删掉了 openWebpage 里那条**拒绝**（改成放行 + 标注），
    //   这里仍把「像接口的 href」当作**解析不出可导航目标**（返回空 = 调用方自己决定）。
    if (LooksLikeNonUserFacingWebUrl(abs)) return {};
    // ★原先这里有一整段 `if (site == L"bilibili.com") { … }`（docs §47 / D1 点名的一处）：
    //   把 `/space/<uid>` 改写成 space.bilibili.com、把 search.* 上的 /video/ /bangumi/
    //   改写回 www.bilibili.com、并拒绝 search.* 上的裸 UID 路径。
    //   那是**站点专属**的 URL 形状魔改（换个网站就坏），已整段删除。
    //   绝对 URL（含 https://space.bilibili.com/123）本来就直接透传，行为不变。

    if (PageUrlsSameDocument(abs, pageUrl)) return {};
    return abs;
}

// ★快照「这是不是搜索结果页」的判据（点名的站点域名 / `/search` / `keyword=`）
//   **已删（批 D，docs §47 / D1）**：判据里写死站点与 URL 形状 ⇒ 换个网站就坏（D-c），
//   而它的全部消费点都是「引擎替模型决定这一页还能不能搜 / 能不能开」的静默拒绝。

std::wstring SanitizeObservePageQuery(const std::wstring& query) {
    std::wstring q = Trim(query);
    if (q.empty()) return {};
    for (;;) {
        const size_t p = q.find(L"搜索");
        if (p == std::wstring::npos) break;
        q.replace(p, 2, L" ");
    }
    std::wstring collapsed;
    bool sp = false;
    for (wchar_t c : q) {
        if (c == L' ' || c == L'\t') {
            if (!sp && !collapsed.empty()) collapsed.push_back(L' ');
            sp = true;
        } else {
            collapsed.push_back(c);
            sp = false;
        }
    }
    return Trim(collapsed);
}

bool LooksLikeBrowserWindowTitle(const std::wstring& title) {
    const std::wstring t = LowerCopy(title);
    if (t.find(L"edge") != std::wstring::npos) return true;
    if (t.find(L"chrome") != std::wstring::npos) return true;
    if (t.find(L"firefox") != std::wstring::npos) return true;
    if (t.find(L"brave") != std::wstring::npos) return true;
    return false;
}

std::wstring StripBrowserWindowTitleDecorations(const std::wstring& title) {
    std::wstring s = Trim(title);
    if (s.empty()) return s;
    // 1) 末尾的浏览器/配置后缀：「 - 个人 - Microsoft Edge」「 - Profile 1 - Google Chrome」
    //    「 - Microsoft Edge」「 - InPrivate」。从右往左剥，最多剥 3 段。
    for (int round = 0; round < 3; ++round) {
        const size_t dash = s.rfind(L" - ");
        if (dash == std::wstring::npos || dash == 0) break;
        const std::wstring tail = LowerCopy(s.substr(dash + 3));
        const bool browserOrProfile = tail.find(L"edge") != std::wstring::npos
            || tail.find(L"chrome") != std::wstring::npos
            || tail.find(L"firefox") != std::wstring::npos
            || tail.find(L"brave") != std::wstring::npos
            || tail.find(L"profile") != std::wstring::npos
            || tail.find(L"inprivate") != std::wstring::npos
            || tail.find(L"个人") != std::wstring::npos
            || tail.find(L"访客") != std::wstring::npos;
        if (!browserOrProfile) break;
        s = Trim(s.substr(0, dash));
        if (s.empty()) return Trim(title);   // 别把整串剥没了
    }
    // 2) 末尾的「和另外 N 个页面」/「and N more pages」
    static const wchar_t* kMoreKeys[] = { L" 和另外", L" and ", L" 及其他" };
    for (const wchar_t* key : kMoreKeys) {
        const size_t pos = s.rfind(key);
        if (pos == std::wstring::npos || pos == 0) continue;
        const std::wstring tail = LowerCopy(s.substr(pos));
        if (tail.find(L"页面") == std::wstring::npos && tail.find(L"page") == std::wstring::npos
            && tail.find(L"标签") == std::wstring::npos && tail.find(L"tab") == std::wstring::npos) {
            continue;
        }
        const std::wstring head = Trim(s.substr(0, pos));
        if (!head.empty()) s = head;
        break;
    }
    return Trim(s);
}

std::wstring UrlEncodeQuery(const std::wstring& text) {
    const std::string u = ToUtf8(text);
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(u.size() * 3);
    for (unsigned char c : u) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return FromUtf8(out);
}

bool LooksLikeSiteHomepageUrl(const std::wstring& url) {
    const std::wstring u = LowerCopy(url);
    if (u.empty()) return false;
    size_t start = 0;
    const size_t scheme = u.find(L"://");
    if (scheme != std::wstring::npos) start = scheme + 3;
    const size_t pathStart = u.find_first_of(L"/?#", start);
    if (pathStart == std::wstring::npos) return true;
    const wchar_t sep = u[pathStart];
    if (sep == L'?' || sep == L'#') return true;
    const size_t pathEnd = u.find_first_of(L"?#", pathStart);
    const std::wstring path = u.substr(pathStart,
        pathEnd == std::wstring::npos ? std::wstring::npos : pathEnd - pathStart);
    return path.empty() || path == L"/" || path == L"/index" || path == L"/index.html"
        || path == L"/index.htm" || path == L"/index.php";
}

std::wstring BuildSiteSearchUrl(const std::wstring& query, const std::wstring& currentUrl) {
    const std::wstring q = Trim(query);
    if (q.empty()) return {};
    const std::wstring encoded = UrlEncodeQuery(q);
    const std::wstring site = ExtractUrlSiteKey(currentUrl);
    auto hasSite = [&](const wchar_t* needle) {
        return site.find(needle) != std::wstring::npos;
    };
    if (hasSite(L"bilibili.com"))
        return L"https://search.bilibili.com/all?keyword=" + encoded;
    if (hasSite(L"baidu.com"))
        return L"https://www.baidu.com/s?wd=" + encoded;
    if (hasSite(L"youtube.com"))
        return L"https://www.youtube.com/results?search_query=" + encoded;
    if (hasSite(L"google."))
        return L"https://www.google.com/search?q=" + encoded;
    if (hasSite(L"bing.com"))
        return L"https://www.bing.com/search?q=" + encoded;
    if (hasSite(L"github.com"))
        return L"https://github.com/search?q=" + encoded;
    if (hasSite(L"zhihu.com"))
        return L"https://www.zhihu.com/search?type=content&q=" + encoded;
    if (hasSite(L"taobao.com"))
        return L"https://s.taobao.com/search?q=" + encoded;
    if (hasSite(L"jd.com"))
        return L"https://search.jd.com/Search?keyword=" + encoded;
    if (hasSite(L"douyin.com"))
        return L"https://www.douyin.com/search/" + encoded;
    if (hasSite(L"weibo.com"))
        return L"https://s.weibo.com/weibo?q=" + encoded;
    if (hasSite(L"twitter.com") || site == L"x.com")
        return L"https://x.com/search?q=" + encoded;
    return {};
}

namespace {

/// 控件名里的通用后缀：「登录按钮」→「登录」。只在剥掉后仍 ≥2 字时生效。
std::wstring StripGenericControlSuffix(const std::wstring& in) {
    static const wchar_t* kSuffixes[] = {
        L"按钮", L"图标", L"链接", L"选项卡", L"菜单项", L"选项", L"输入框", L"搜索框",
        L"标签", L"复选框", L"单选框", L"下拉框", L"下拉菜单", L"开关",
    };
    const std::wstring s = Trim(in);
    for (const wchar_t* suf : kSuffixes) {
        const size_t len = wcslen(suf);
        if (s.size() > len + 1 && s.compare(s.size() - len, len, suf) == 0) {
            return Trim(s.substr(0, s.size() - len));
        }
    }
    return s;
}



bool IsInputSnapshotRole(const std::wstring& role) {
    return role == L"textbox" || role == L"searchbox" || role == L"combobox"
        || role == L"textarea" || role == L"spinbutton";
}

/// 剥掉「请帮我点击」「点击」这类动作前缀（扩展查询只做子串命中，带前缀必 0 命中）。
std::wstring StripActionPrefixForQuery(const std::wstring& in) {
    static const wchar_t* kPrefixes[] = {
        L"请帮我点击", L"帮我点击一下", L"帮我点击", L"请点击一下", L"请点击", L"点击一下",
        L"左键点击", L"双击一下", L"双击", L"单击", L"点一下", L"点选", L"点击",
        L"请帮我打开", L"帮我打开", L"打开", L"定位到", L"定位", L"寻找", L"找到",
        L"click on the", L"click on", L"click the", L"click", L"locate",
    };
    std::wstring s = Trim(in);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const wchar_t* pfx : kPrefixes) {
            const size_t len = wcslen(pfx);
            if (s.size() > len + 1
                && _wcsnicmp(s.c_str(), pfx, len) == 0) {
                s = Trim(s.substr(len));
                changed = true;
                break;
            }
        }
    }
    return s;
}

/// 名字相对短标签的匹配档位：0=不匹配，越高越可信。
int SnapshotNameMatchTier(const std::wstring& nameLower, const std::wstring& targetLower) {
    if (nameLower.empty() || targetLower.empty()) return 0;
    if (nameLower == targetLower) return 4;
    if (nameLower.rfind(targetLower, 0) == 0) return 3;
    if (nameLower.find(targetLower) != std::wstring::npos) return 2;
    if (targetLower.find(nameLower) != std::wstring::npos && nameLower.size() >= 2) return 1;
    return 0;
}

}  // namespace

std::wstring PageSnapshotTargetKeyword(const std::wstring& target) {
    std::wstring s = StripActionPrefixForQuery(StripGenericControlSuffix(target));
    // 引号/书名号只用于人类断句，扩展侧子串匹配必须去掉
    auto stripQuote = [](const std::wstring& in) {
        std::wstring o;
        o.reserve(in.size());
        for (const wchar_t c : in) {
            if (c == L'"' || c == L'\'' || c == L'\u201c' || c == L'\u201d'
                || c == L'\u300c' || c == L'\u300d' || c == L'\u300e' || c == L'\u300f'
                || c == L'\u3010' || c == L'\u3011')
                continue;
            o.push_back(c);
        }
        return Trim(o);
    };
    s = stripQuote(s);
    while (!s.empty() && (s.back() == L'。' || s.back() == L'，' || s.back() == L'！'
        || s.back() == L'？' || s.back() == L'.' || s.back() == L',' || s.back() == L'!'
        || s.back() == L'?')) {
        s.pop_back();
    }
    s = Trim(s);
    if (s.size() >= 2) return s;
    // 压完不足 2 字（如「登录」被剥成空）→ 回退原短语，交给宿主打分过滤
    return Trim(target);
}

namespace {

/// ★ 目标要求**精确命中**：纯数字（"3"/"27"）或单字符（"三"/"A"）。
/// 这类目标用包含档匹配必错（"3" 会命中「题量: 27…」）。与元素索引侧
/// 「纯数字/单字只允许精确档」同一把尺。
bool SnapshotTargetNeedsExact(const std::wstring& target) {
    if (target.empty()) return false;
    if (target.size() == 1) return true;
    for (wchar_t c : target) {
        if (c < L'0' || c > L'9') return false;
    }
    return true;   // 全部是 ASCII 数字
}

}  // namespace

PageSnapshotPick PickPageSnapshotRefForText(const PageSnapshot& snap, const std::wstring& text,
    PageSnapshotPickKind kind) {
    PageSnapshotPick out;
    const std::wstring raw = Trim(text);
    if (raw.empty()) return out;

    // ★★ **纯数字 / 单字目标：只允许精确命中**（2026-10-02 真机事故）。
    //
    //   目标「3」（题目导航里的题号）被**包含档**配到了「题量: 27 满分: 100…」
    //   （score=597）⇒ clickRef 点到页头说明块 ⇒ 用户："点击定位不准，总是选错选项"。
    //   元素索引那条路**早就有**这条（「纯数字/单字只允许精确档」），DOM 这条路漏了。
    //   ⇒ 补同一把尺：这类目标 tier<4（非精确）一律跳过；命中不了就回落视觉/UIA，
    //     绝不拿一个模糊候选去 clickRef。
    const bool strictOnly = SnapshotTargetNeedsExact(raw);

    std::wstring targetLower = LowerCopy(raw);
    const std::wstring stripped = StripGenericControlSuffix(raw);
    const std::wstring strippedLower = LowerCopy(stripped);
    const bool hasStripped = strippedLower.size() >= 2 && strippedLower != targetLower;

    struct Cand {
        const PageSnapshotNode* node;
        int score;
        bool exact;
        int tier;
        size_t nameLen;
    };
    std::vector<Cand> cands;

    for (const auto& n : snap.nodes) {
        if (!LooksLikePageSnapshotRef(n.ref)) continue;
        const std::wstring name = Trim(n.name);
        if (name.size() < 2) continue;
        const std::wstring nameLower = LowerCopy(name);

        int tier = SnapshotNameMatchTier(nameLower, targetLower);
        // ★ strict 目标：**连"去掉控件后缀"的变体也不许**（只认原始文本的精确档）
        if (strictOnly && tier < 4) continue;
        if (hasStripped) {
            const int alt = SnapshotNameMatchTier(nameLower, strippedLower);
            if (alt > tier) tier = alt;
        }
        if (tier == 0) continue;

        int score = 0;
        switch (tier) {
        case 4: score = 1000; break;
        case 3: score = 700; break;
        case 2: score = 500; break;
        default: score = 400; break;
        }
        // 目标 role 加权：点击要 button/link，填写要 textbox/searchbox——
        // 用同一套权重会把「密码」判到「忘记密码」链接上。
        if (kind == PageSnapshotPickKind::Input) {
            if (IsInputSnapshotRole(n.role)) score += 160;
            else if (n.role == L"link") score -= 200;
            else if (n.role == L"button") score -= 120;
        } else if (IsClickableSnapshotRole(n.role)) {
            score += 120;
        }
        if (PageSnapshotNodeInView(n, snap)) score += 60;
        else score -= 80;
        const int area = (n.w > 0 && n.h > 0) ? n.w * n.h : 0;
        if (area > 200000) score -= 400;
        else if (area > 60000) score -= 200;
        else if (area > 20000) score -= 80;
        // 阅读顺序：越靠上越可能是主控件（扩展侧的「列表第1项」同源）
        if (n.y > 0) score -= (std::min)(n.y, 2000) / 40;
        cands.push_back({&n, score, tier == 4, tier, name.size()});
    }

    out.candidates = static_cast<int>(cands.size());
    if (cands.empty()) return out;

    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.node->y != b.node->y) return a.node->y < b.node->y;
        return a.node->x < b.node->x;
    });

    const Cand& best = cands.front();
    out.ref = best.node->ref;
    out.name = best.node->name;
    out.role = best.node->role;
    out.score = best.score;
    out.exact = best.exact;
    for (const auto& c : cands) {
        if (LowerCopy(Trim(c.node->name)) == LowerCopy(Trim(best.node->name))) ++out.sameNameCount;
    }
    // 可信门槛：至少要「包含」级命中（tier>=2 → 基础分 ≥500，扣分后仍须 ≥400）
    if (best.score < 400) {
        out.ref.clear();
        return out;
    }
    // 同名多项：扩展已按相关性 + 阅读顺序排好，取最前 = 列表第1项，不算歧义。
    // 部分命中：只有「同一匹配档 + 名字长度几乎相同」的竞争项才算歧义——
    // 名字明显更长的那个已经是更具体的匹配，不该因为一点分差被判歧义。
    if (out.exact) {
        out.ambiguous = false;
    } else {
        for (size_t k = 1; k < cands.size(); ++k) {
            if (cands[k].tier != best.tier) continue;
            if (cands[k].score < best.score - 120) continue;
            const size_t d = cands[k].nameLen > best.nameLen
                ? cands[k].nameLen - best.nameLen : best.nameLen - cands[k].nameLen;
            if (d <= 1) {
                out.ambiguous = true;
                break;
            }
        }
    }
    // 部分命中但命中项与短标签几乎无共同语义（如「搜索」命中「搜索历史记录」）交给识图。
    return out;
}

// ── 批量选择题流水线（新路由，2026-10-02 用户批准）────────────────────────

namespace {

bool LooksLikeQuizMarker(const std::wstring& name) {
    const std::wstring t = Trim(name);
    if (t.empty()) return false;
    if (t.rfind(L"题目", 0) == 0) return true;
    if (t.rfind(L"第", 0) == 0 && t.find(L"题") != std::wstring::npos) return true;
    if (t.size() >= 2 && t.size() <= 10
        && t[0] >= L'0' && t[0] <= L'9'
        && (t.find(L'.') != std::wstring::npos || t.find(L'、') != std::wstring::npos
            || t.find(L'）') != std::wstring::npos || t.find(L')') != std::wstring::npos)) {
        return true;
    }
    return false;
}

}  // namespace

bool BuildQuizChoiceGroups(const PageSnapshot& snap, std::vector<QuizChoiceGroup>& out,
                           int minOptionsPerGroup) {
    out.clear();
    if (minOptionsPerGroup < 2) minOptionsPerGroup = 2;
    // ★★ **去"题号"化**（2026-10-02，用户要求"通用一点"）：
    //   不再认「题目 N. / 第 N 题」字样 —— 纯几何聚类：
    //   把树里的 radio/checkbox 按视觉 y 排序，相邻间距超过阈值就切新组；
    //   组标题取"该组上方最近的一条 option/heading 文本"（有就用，没有留空）。
    //   任何批量选择表单（题库/问卷/投票/测试）都触发，不依赖命名习惯。
    struct Item { std::wstring name, ref, group; int y = 0, x = 0; };
    std::vector<Item> items;
    for (const auto& n : snap.nodes) {
        if (!LooksLikePageSnapshotRef(n.ref)) continue;
        if (n.role != L"radio" && n.role != L"checkbox") continue;
        items.push_back({Trim(n.name), n.ref, n.group, n.y, n.x});
    }
    if (items.size() < 4) return false;   // 至少两组的量才值得走流水线
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.y != b.y) return a.y < b.y;
        return a.x < b.x;
    });
    // 组间距阈值：组内相邻选项的 y 间距小，题与题之间有组标题区（间距明显更大）。
    // 70px 是对超星/常见表单两种页面实测的中间值。
    const int kClusterGapPx = 70;
    std::vector<std::vector<Item>> clusters;
    // ★★★ **优先按「容器标识」分组**（2026-10-03 方向 1：抓取通用性）
    //
    //   ⚠⚠ 原来**按 y 坐标**聚类 ⇒ 页面一滚动 y 全变 ⇒ 同题被切两组（重复）
    //     或落进缝隙（漏题）—— 真机铁证：题号 11 → 13 → 15 → **15（重复）** ✓
    //   ⇒ `group` 是扩展按"**最近的、含 ≥2 个同构孩子的祖先**"算的 **DOM 结构 id**
    //     （`TAG.class@idx`），**不依赖坐标** ⇒ 对滚动**天然稳定** ✓
    //   ⚠ 通用性：判据是**结构**（同构兄弟）而不是 `radio/checkbox` 语义
    //     ⇒ 题 / 表格行 / 消息 / 卡片 通吃，**不针对"练习题"写死** ✓
    //   ⚠ 兜底：一个 group 都没有（老扩展 / 非列表页）⇒ **回退原来的 y 聚类** ✓
    bool anyGroup = false;
    for (const auto& it : items) { if (!it.group.empty()) { anyGroup = true; break; } }
    if (anyGroup) {
        std::vector<std::wstring> order;          // 保持首次出现顺序（= y 序，稳）
        for (const auto& it : items) {
            if (it.group.empty()) continue;
            if (std::find(order.begin(), order.end(), it.group) == order.end())
                order.push_back(it.group);
        }
        for (const auto& g : order) {
            std::vector<Item> c;
            for (const auto& it : items) { if (it.group == g) c.push_back(it); }
            if (!c.empty()) clusters.push_back(std::move(c));
        }
    } else {
        clusters.push_back({items[0]});
        for (size_t i = 1; i < items.size(); ++i) {
            if (items[i].y - clusters.back().back().y <= kClusterGapPx)
                clusters.back().push_back(items[i]);
            else
                clusters.push_back({items[i]});
        }
    }

    // 组标题：该组第一个选项**上方最近**的 option/heading 文本（无则空）
    std::vector<const PageSnapshotNode*> titles(clusters.size(), nullptr);
    for (size_t c = 0; c < clusters.size(); ++c) {
        const int y0 = clusters[c].front().y;
        const PageSnapshotNode* best = nullptr;
        for (const auto& n : snap.nodes) {
            if (n.role != L"option" && n.role != L"heading") continue;
            if (!LooksLikePageSnapshotRef(n.ref)) continue;
            if (n.y >= y0) continue;
            if (!best || n.y > best->y) best = &n;
        }
        titles[c] = best;
    }

    for (size_t c = 0; c < clusters.size(); ++c) {
        QuizChoiceGroup g;
        if (titles[c]) g.title = Trim(titles[c]->name);
        bool dup = false;
        for (size_t i = 0; i < clusters[c].size(); ++i) {
            const Item& it = clusters[c][i];
            wchar_t letter = 0;
            for (wchar_t ch : it.name) {
                if ((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')) { letter = ch; break; }
            }
            if (letter == 0) continue;
            if (letter >= L'a' && letter <= L'z')
                letter = static_cast<wchar_t>(letter - L'a' + L'A');
            for (const auto& o : g.options)
                if (!o.letter.empty() && o.letter[0] == letter) { dup = true; break; }
            if (dup) break;
            g.options.push_back({std::wstring(1, letter), it.name, it.ref, it.y});
        }
        // ★★★ **跨帧/跨滚动的去重**（2026-10-03 用户报"漏题 + 重复"）
        //
        //   ⚠⚠ 聚类是**按 y 坐标**分组的 ⇒ 页面一滚动 y 全变 ⇒
        //     同一题可能被切成两组（重复）、或某题落进缝隙被跳过（漏题）
        //     ⇒ 真机铁证：题号 11 → 13 → 15 → **15（重复）** ✓
        //   ⇒ 用**题干文本的指纹**判断"这题见过没有" —— 它对滚动**不变** ✓
        //   ⚠ 通用性：指纹取自**题干文本**（不是位置 / 序号 / 题号）
        //     ⇒ 换任何页面、任何语言都成立；**不针对"练习题"写死** ✓
                auto normTitle = [](const std::wstring& s) {
                    std::wstring r;
                    for (wchar_t c : s) {
                        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
                        if (c == L'。' || c == L'，' || c == L'、' || c == L'：' || c == L'；') continue;
                        r.push_back(c);
                        if (r.size() >= 40) break;   // 前 40 字足够区分同一页的题
                    }
                    return r;
                };
                bool seenBefore = false;
                const std::wstring fp = normTitle(g.title);
                if (!fp.empty()) {
                    for (const auto& e : out) {
                        if (normTitle(e.title) == fp) { seenBefore = true; break; }
                    }
                }
                if (!dup && !seenBefore && static_cast<int>(g.options.size()) >= minOptionsPerGroup)
                    out.push_back(std::move(g));
    }
    return out.size() >= 2;
}

std::wstring FormatQuizSingleQuestionPrompt(const QuizChoiceGroup& g,
                                            int questionNo, int totalQuestions) {
    // ★★★ 题号措辞（2026-10-03 用户报"传的题号和抓的题号对不上"）
    //   ⚠⚠ 这里的 `questionNo` 是**收割循环自己的计数**（第几个被答的），
    //     **不是页面上的题号**（页面上可能是"第 11 题"）⇒ 用户看到以为错位 ✓
    //   ⇒ 措辞改成"本次第 N 题"，**明确它是流程计数**；
    //     ⚠ **不写页面题号**：那是 DOM 的显示文本，与答题无关，写了反而误导 ✓
    std::wstring p = L"本次第 " + std::to_wstring(questionNo) + L" 题（本页共 "
        + std::to_wstring(totalQuestions) + L" 题）。选项：";
    for (size_t i = 0; i < g.options.size(); ++i) {
        if (i) p += L"；";
        p += g.options[i].letter + L": " + g.options[i].text;
    }
    p += L"。只回答一个字母，不要解释。";
    return p;
}

wchar_t ParseQuizAnswerLetter(const std::wstring& reply) {
    const std::wstring s = Trim(reply);
    if (s.empty()) return 0;
    // ① 整条只有一个 A-D 字母 ⇒ 直接采信
    std::vector<wchar_t> caps;
    for (wchar_t c : s) {
        if (c >= L'A' && c <= L'D') caps.push_back(c);
        else if (c >= L'a' && c <= L'd')
            caps.push_back(static_cast<wchar_t>(c - L'a' + L'A'));
    }
    if (caps.size() == 1) return caps[0];
    // ② 否则取第一个 A-D（按出现顺序）
    for (wchar_t c : s) {
        if (c >= L'A' && c <= L'D') return c;
        if (c >= L'a' && c <= L'd') return static_cast<wchar_t>(c - L'a' + L'A');
    }
    return 0;
}

// ★★★ **一次性**拼「本页全部题目」的提示词（2026-10-03 用户要求"一次传全"）
//
//   目的：**1 轮 API 换 N 轮**。逐题问 = N 轮 API（真机每题 10~15 秒，27 题要 5~9 分钟）；
//   一次问 = **1 轮** ⇒ 20 题从 3~5 分钟降到 ~15 秒 ✓
//   ⚠ 输出格式**只说一次**（不每题重复），每题只列题干 + 选项 ⇒ 提示词也短得多 ✓
std::wstring FormatQuizAllQuestionsPrompt(const std::vector<QuizChoiceGroup>& groups) {
    std::wstring p;
    p += L"本页共 " + std::to_wstring(groups.size()) + L" 道题。请**一次性**给出全部答案。\n";
    p += L"只输出一个 JSON 数组，元素是每题选中的字母，按题号顺序：\n";
    p += L"[\"A\",\"C\",\"B\"]\n";
    p += L"⚠ 答不出的位置写 \"?\"（不要编造）；不要解释、不要别的字段、不要调用工具。\n\n";
    for (size_t i = 0; i < groups.size(); ++i) {
        const QuizChoiceGroup& g = groups[i];
        p += std::to_wstring(i + 1) + L". " + g.title + L"\n";
        for (const auto& o : g.options) {
            p += L"   " + o.letter + L": " + o.text + L"\n";
        }
    }
    return p;
}

// ★★★ 解析「一次返回的答案数组」（`["A","C","?","D"]`）
//
//   ⚠⚠ **只在第一个 `[ ... ]` 区间内抽字母** —— 否则会把**题干/选项文字里的 A/B/C**
//     也当成答案（那些字母满地都是）⇒ 解析必然错位 ✓
//   ⇒ 提示词已**明确要求**输出 JSON 数组；没有 `[` 就视为"没按格式答" ⇒ 返回全 0（全跳过）✓
std::vector<wchar_t> ParseQuizAnswerArray(const std::wstring& reply, size_t count) {
    std::vector<wchar_t> out(count, 0);
    const size_t lb = reply.find(L'[');
    if (lb == std::wstring::npos) return out;          // 没按格式 ⇒ 全 0
    const size_t rb = reply.find(L']', lb);
    const size_t to = (rb == std::wstring::npos) ? reply.size() : rb;
    size_t idx = 0;
    for (size_t i = lb + 1; i < to && idx < count; ++i) {
        const wchar_t c = reply[i];
        if (c >= L'a' && c <= L'z') { out[idx++] = static_cast<wchar_t>(c - L'a' + L'A'); continue; }
        if (c >= L'A' && c <= L'Z') { out[idx++] = c; continue; }
        if (c == L'?') { out[idx++] = 0; continue; }   // 显式"答不出"
    }
    return out;
}
std::wstring FindNextPageControlRef(const PageSnapshot& snap) {
    static const wchar_t* kNext[] = {
        L"下一页", L"下一頁", L"下页", L"后一页", L"next", L"Next", L"›", L"»", L"→",
    };
    for (const auto& n : snap.nodes) {
        if (!LooksLikePageSnapshotRef(n.ref)) continue;
        if (n.role != L"button" && n.role != L"link") continue;
        const std::wstring t = Trim(n.name);
        if (t.empty() || t.size() > 14) continue;   // 长文本不会是翻页按钮
        for (const wchar_t* k : kNext) {
            if (t.find(k) != std::wstring::npos) return n.ref;
        }
    }
    return {};
}