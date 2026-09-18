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

bool PathIsAllDigitsUid(const std::wstring& path) {
    size_t i = (!path.empty() && path[0] == L'/') ? 1 : 0;
    if (i >= path.size()) return false;
    for (size_t k = i; k < path.size(); ++k) {
        if (path[k] < L'0' || path[k] > L'9') return false;
    }
    return true;
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
    std::wstring listFirst;
    if (!LooksLikeWatchVideoSiteUrl(snap.url))
        listFirst = SnapshotListFirstRef(snap);
    if (!listFirst.empty()) {
        out += L"列表第1项=" + listFirst
            + L"（同类卡片最上最左；大图/底部推荐不是第1）。clickRef 该项，禁止滚动去点别的卡。\n";
    }
    if (LooksLikeWatchVideoSiteUrl(snap.url)) {
        out += L"播放页：工具栏按钮优先 clickRef";
        std::wstring btns;
        int nBtn = 0;
        for (const auto& node : snap.nodes) {
            if (node.role != L"button" && node.role != L"checkbox" && node.role != L"switch")
                continue;
            if (node.name.empty()) continue;
            if (!btns.empty()) btns += L"、";
            btns += node.name + L"=" + node.ref;
            if (++nBtn >= 4) break;
        }
        if (!btns.empty()) out += L"（" + btns + L"）";
        out += L"；树上没有再用 locateAndClick。相关推荐不是目标。\n";
    }
    if (LooksLikeSiteSearchResultsUrl(snap.url)) {
        out += L"已在搜索结果页：点 href 含 space.bilibili.com 的用户卡片（clickRef 会打开主页）"
            L"或 /video/ 标题。点「综合/用户」筛选项不会跳转。禁止再搜索。\n";
    } else if (LooksLikeUserSpaceSiteUrl(snap.url)) {
        out += L"这是某个用户空间。找别的UP请 searchOnPage(query)，勿用站内搜。\n";
    }

    int inViewN = 0;
    int offN = 0;
    for (const auto& n : snap.nodes) {
        if (PageSnapshotNodeInView(n, snap)) ++inViewN;
        else ++offN;
    }
    if (!snap.nodes.empty())
        out += FormatSnapshotPageCoverage(snap, inViewN, offN);

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

bool LooksLikeUserSpaceSiteUrl(const std::wstring& url) {
    return ExtractUrlHostLower(url) == L"space.bilibili.com";
}

bool LooksLikeWatchVideoSiteUrl(const std::wstring& url) {
    const std::wstring path = ExtractUrlPathLower(url);
    if (path.find(L"/upload/") != std::wstring::npos) return false;
    if (path.find(L"/video/") != std::wstring::npos
        || path.find(L"/bangumi/") != std::wstring::npos)
        return true;
    return path == L"/watch" || path.rfind(L"/watch/", 0) == 0;
}

bool ShouldDirectNavigateSnapshotHref(const std::wstring& url) {
    if (url.empty() || LooksLikeWatchVideoSiteUrl(url)) return false;
    return LooksLikeUserSpaceSiteUrl(url);
}

std::wstring SnapshotListFirstRef(const PageSnapshot& snap) {
    struct Card {
        std::wstring ref;
        int x = 0;
        int y = 0;
    };
    std::vector<Card> cards;
    const int vw = snap.vw > 0 ? snap.vw : 1200;
    const int heroW = (std::min)(vw * 45 / 100, 420);
    for (const auto& n : snap.nodes) {
        if (!PageSnapshotNodeInView(n, snap)) continue;
        if (n.href.empty() || !LooksLikeWatchVideoSiteUrl(n.href)) continue;
        if (n.w > heroW && n.h > 80) continue;
        if (n.ref.empty()) continue;
        cards.push_back({n.ref, n.x, n.y});
    }
    if (cards.size() < 2) return {};
    int bestCount = 0;
    int bestY = (std::numeric_limits<int>::max)();
    std::wstring bestRef;
    for (const auto& seed : cards) {
        std::vector<Card> group;
        for (const auto& o : cards) {
            const int dy = o.y > seed.y ? o.y - seed.y : seed.y - o.y;
            if (dy <= 36) group.push_back(o);
        }
        if (group.size() < 2) continue;
        Card first = group[0];
        for (const auto& g : group) {
            if (g.y < first.y || (g.y == first.y && g.x < first.x)) first = g;
        }
        const int n = static_cast<int>(group.size());
        if (n > bestCount || (n == bestCount && first.y < bestY)) {
            bestCount = n;
            bestY = first.y;
            bestRef = first.ref;
        }
    }
    return bestCount >= 2 ? bestRef : std::wstring();
}

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
        if (LooksLikeWatchVideoSiteUrl(n.href)) score -= 800;
        if (score > bestScore) {
            bestScore = score;
            best = n.ref;
        }
    }
    return best;
}

bool LooksLikeEmptyUserSpaceUrl(const std::wstring& url) {
    if (!LooksLikeUserSpaceSiteUrl(url)) return false;
    std::wstring path = ExtractUrlPathLower(url);
    while (path.size() > 1 && path.back() == L'/') path.pop_back();
    return path.empty() || path == L"/";
}

bool LooksLikeGuessedUserSpaceUrl(const std::wstring& url) {
    if (!LooksLikeUserSpaceSiteUrl(url)) return false;
    std::wstring path = ExtractUrlPathLower(url);
    while (path.size() > 1 && path.back() == L'/') path.pop_back();
    if (path.empty() || path == L"/") return true;
    return PathIsAllDigitsUid(path);
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
    if (LooksLikeNonUserFacingWebUrl(abs)) return {};

    std::wstring destHost = ExtractUrlHostLower(abs);
    std::wstring destPath = ExtractUrlPathLower(abs);
    const std::wstring site = ExtractUrlSiteKey(pageUrl.empty() ? abs : pageUrl);

    if (site == L"bilibili.com") {
        if (destPath.size() > 7 && destPath.compare(0, 7, L"/space/") == 0
            && destHost.find(L"space.") == std::wstring::npos) {
            const std::wstring uid = destPath.substr(7);
            if (PathIsAllDigitsUid(L"/" + uid)) {
                abs = L"https://space.bilibili.com/" + uid;
                destHost = L"space.bilibili.com";
                destPath = L"/" + uid;
            }
        }
        if (destPath.size() >= 7 && destPath.compare(0, 7, L"/video/") == 0
            && destHost.find(L"search.") != std::wstring::npos) {
            abs = L"https://www.bilibili.com" + destPath;
            destHost = L"www.bilibili.com";
        }
        if (destPath.size() >= 9 && destPath.compare(0, 9, L"/bangumi/") == 0
            && destHost.find(L"search.") != std::wstring::npos) {
            abs = L"https://www.bilibili.com" + destPath;
            destHost = L"www.bilibili.com";
        }
        if (destHost.find(L"search.") != std::wstring::npos && PathIsAllDigitsUid(destPath))
            return {};
    }

    if (PageUrlsSameDocument(abs, pageUrl)) return {};
    return abs;
}

bool LooksLikeSiteSearchResultsUrl(const std::wstring& url) {
    const std::wstring u = LowerCopy(url);
    if (u.find(L"search.bilibili.com") != std::wstring::npos) return true;
    if (u.find(L"/search") != std::wstring::npos) return true;
    if (u.find(L"keyword=") != std::wstring::npos
        && (u.find(L"search") != std::wstring::npos || u.find(L"/all?") != std::wstring::npos))
        return true;
    return false;
}

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

bool IsClickableSnapshotRole(const std::wstring& role) {
    return role == L"button" || role == L"link" || role == L"menuitem" || role == L"tab"
        || role == L"option" || role == L"checkbox" || role == L"radio" || role == L"switch";
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

PageSnapshotPick PickPageSnapshotRefForText(const PageSnapshot& snap, const std::wstring& text,
    PageSnapshotPickKind kind) {
    PageSnapshotPick out;
    const std::wstring raw = Trim(text);
    if (raw.empty()) return out;

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
