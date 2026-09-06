#include "ai_action_router.h"

#include "script_action_builder.h"
#include "utils.h"

#include <algorithm>
#include <cwctype>

namespace {

std::wstring ToLowerCopy(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool HasAny(const std::wstring& p, std::initializer_list<const wchar_t*> keys) {
    for (const wchar_t* k : keys) {
        if (p.find(k) != std::wstring::npos) return true;
    }
    return false;
}

bool HasActionVerb(const std::wstring& p) {
    // 含「回答/回复/发送」等桌面操作意图：必须走 tools，不能当识图问答
    if (HasAny(p, {L"点击", L"双击", L"单击", L"点一下", L"按下"})) return true;
    if (HasAny(p, {L"输入", L"按键", L"拖动", L"滚动", L"打开", L"关闭", L"运行",
            L"按下", L"回答", L"回复", L"发送", L"提交", L"填写", L"粘贴", L"打字", L"写入",
            L"键入", L"敲回车", L"回车", L"滑动", L"滚轮", L"拖拽", L"切换", L"保存", L"另存为",
            L"删除", L"新建", L"重命名", L"启动", L"退出", L"最大化", L"最小化", L"全选", L"复制",
            L"剪切", L"清空", L"撤销", L"恢复", L"选择"})) {
        return true;
    }
    // 「移动」只有在明显指移动鼠标/光标时才算动作，避免「移动端/移动硬盘」等名词误伤
    if (p.find(L"移动") != std::wstring::npos
        && HasAny(p, {L"鼠标", L"光标", L"指针", L"移动到"})) {
        return true;
    }
    // 「按」单独算动词，但不能把「按钮」里的「按」算进去
    size_t pos = 0;
    while ((pos = p.find(L"按", pos)) != std::wstring::npos) {
        if (pos + 1 < p.size() && p[pos + 1] == L'钮') {
            pos += 2;
            continue;
        }
        return true;
    }
    return false;
}

}  // namespace

std::wstring AiActionRouteLabel(AiActionRouteKind kind) {
    switch (kind) {
    case AiActionRouteKind::VisionQuery: return L"识图问答";
    case AiActionRouteKind::CompositeClick: return L"本地定位点击";
    case AiActionRouteKind::MultiTurnTools: return L"复杂组合(多轮工具)";
    default: return L"动作执行(工具)";
    }
}

bool IsAiActionVisionQueryPrompt(const std::wstring& prompt) {
    const std::wstring p = Trim(prompt);
    if (p.empty()) return false;
    if (HasActionVerb(p)) return false;
    return HasAny(p, {L"输出", L"坐标", L"识别", L"分析", L"找到", L"在哪里", L"在哪", L"位置",
        L"多少", L"是什么", L"什么颜色", L"描述", L"读", L"检测", L"有没有", L"有几个", L"看出",
        L"看一下", L"看看", L"看下", L"看屏幕", L"屏幕上有"});
}

bool IsAiActionCompositeClickPrompt(const std::wstring& prompt) {
    const std::wstring p = Trim(prompt);
    if (p.empty()) return false;
    if (IsAiActionComplexCompositePrompt(p)) return false;
    return HasAny(p, {L"点击", L"双击", L"单击", L"点一下", L"按下"});
}

bool IsAiActionReplyMessagePrompt(const std::wstring& prompt) {
    const std::wstring p = Trim(prompt);
    // 「回答/回复 … 消息/聊天」或「回答对方发送的…」：单任务快路径，勿当多步组合
    if (!HasAny(p, {L"回答", L"回复"})) return false;
    if (HasAny(p, {L"消息", L"聊天", L"对方"})) return true;
    return p.find(L"发送") != std::wstring::npos;
}

bool IsAiActionComplexCompositePrompt(const std::wstring& prompt) {
    const std::wstring p = Trim(prompt);
    if (p.empty()) return false;
    if (!HasActionVerb(p)) return false;
    // 回复聊天：quickInput+发送 一条链，禁止误判成「复杂组合(多轮工具)」
    if (IsAiActionReplyMessagePrompt(p)) return false;
    // 「最后一条/最后一个」是名词修饰，不是「最后再操作」的步骤连接词
    if (HasAny(p, {L"然后", L"接着", L"之后", L"并且", L"同时", L"第一步", L"第二步", L"接下来"}))
        return true;
    if (p.find(L"最后再") != std::wstring::npos || p.find(L"最后点") != std::wstring::npos
        || p.find(L"最后按") != std::wstring::npos || p.find(L"最后输入") != std::wstring::npos)
        return true;
    // 「再」：避免「再不/再次」误伤时可再收紧；「先…后…」仍算多步
    if (p.find(L"再") != std::wstring::npos && HasAny(p, {L"点击", L"输入", L"按", L"打开"}))
        return true;
    // 「先…后…」须真有「先」；勿因「对方」里的「后」误伤
    if (p.find(L"先") != std::wstring::npos && HasAny(p, {L"后", L"再", L"然后"}))
        return true;

    auto hasInputActionVerb = [&]() -> bool {
        // 「输入框/栏/区…」是控件名词，不是「输入文字」动作（否则「点击搜索输入框」会误进多轮 Agent）
        size_t pos = 0;
        while ((pos = p.find(L"输入", pos)) != std::wstring::npos) {
            const size_t after = pos + 2;
            if (after < p.size()) {
                const wchar_t n = p[after];
                if (n == L'框' || n == L'栏' || n == L'区' || n == L'窗' || n == L'法'
                    || n == L'端' || n == L'源' || n == L'流' || n == L'项'
                    || n == L'口' || n == L'板' || n == L'格') {
                    pos = after;
                    continue;
                }
            }
            return true;
        }
        return false;
    };

    int verbs = 0;
    if (p.find(L"点击") != std::wstring::npos || p.find(L"双击") != std::wstring::npos) ++verbs;
    if (hasInputActionVerb()) ++verbs;
    if (p.find(L"按键") != std::wstring::npos || p.find(L"按下") != std::wstring::npos) ++verbs;
    else {
        size_t pos = 0;
        while ((pos = p.find(L"按", pos)) != std::wstring::npos) {
            if (pos + 1 < p.size() && p[pos + 1] == L'钮') {
                pos += 2;
                continue;
            }
            ++verbs;
            break;
        }
    }
    if (p.find(L"打开") != std::wstring::npos) ++verbs;
    if (p.find(L"关闭") != std::wstring::npos) ++verbs;
    return verbs >= 2;
}

AiActionRouteKind ClassifyAiActionRoute(const std::wstring& prompt, bool withImage) {
    if (!withImage) return AiActionRouteKind::ToolExecute;
    // 简单点击：宿主本地跑 locateAndClick（不经 Agent 选工具，省一轮规划 API）
    if (IsAiActionCompositeClickPrompt(prompt)) return AiActionRouteKind::CompositeClick;
    if (IsAiActionComplexCompositePrompt(prompt)) return AiActionRouteKind::MultiTurnTools;
    if (HasActionVerb(prompt)) return AiActionRouteKind::ToolExecute;
    if (IsAiActionVisionQueryPrompt(prompt)) return AiActionRouteKind::VisionQuery;
    return AiActionRouteKind::ToolExecute;
}

bool AiActionPromptLikelyNeedsScreenCapture(const std::wstring& prompt) {
    const std::wstring p = Trim(prompt);
    if (p.empty()) return false;
    // UI 点击 / 定位（不含笼统「按下」——易与按键混淆）
    if (HasAny(p, {L"点击", L"双击", L"单击", L"点一下", L"点选"})) return true;
    if (HasAny(p, {L"找图", L"识图", L"locateAndClick"})) return true;
    // 视觉滚动/拖拽/移动鼠标：无图执行是盲操作，需首帧截图
    if (HasAny(p, {L"滑动", L"滚轮", L"滚动", L"拖拽", L"拖到", L"移动鼠标", L"拖拽到"})) return true;
    if (HasAny(p, {L"屏幕上", L"界面上", L"截图中", L"看屏幕", L"看一下屏幕", L"看下屏幕"}))
        return true;
    if (HasAny(p, {L"按钮", L"图标", L"搜索框", L"搜索按钮"})
        && HasAny(p, {L"找", L"定位", L"点", L"在哪", L"哪里"})) {
        return true;
    }
    return false;
}

bool TryParseCoordinatePair(const std::wstring& text, int& outX, int& outY) {
    outX = outY = 0;
    const std::wstring s = Trim(text);
    if (s.empty()) return false;

    auto parseInts = [&](size_t i0) -> bool {
        wchar_t* ex = nullptr;
        const long x = wcstol(s.c_str() + i0, &ex, 10);
        if (ex == s.c_str() + i0) return false;
        while (ex < s.c_str() + s.size() && (*ex == L' ' || *ex == L',' || *ex == L'，')) ++ex;
        wchar_t* yEnd = ex;
        const long y = wcstol(ex, &yEnd, 10);
        if (yEnd == ex) return false;
        outX = static_cast<int>(x);
        outY = static_cast<int>(y);
        return true;
    };

    const size_t lp = s.find(L'(');
    const size_t rp = s.find(L')', lp == std::wstring::npos ? 0 : lp);
    if (lp != std::wstring::npos && rp != std::wstring::npos && rp > lp + 1)
        return parseInts(lp + 1);

    for (size_t i = 0; i < s.size(); ++i) {
        if (iswdigit(s[i]) || (s[i] == L'-' && i + 1 < s.size() && iswdigit(s[i + 1]))) {
            if (parseInts(i)) return true;
            break;
        }
    }
    return false;
}

bool TryParseBoundingBox(const std::wstring& text, int& outX1, int& outY1, int& outX2, int& outY2) {
    outX1 = outY1 = outX2 = outY2 = 0;
    const std::wstring s = Trim(text);
    if (s.empty()) return false;

    auto parseFourAt = [&](size_t i0) -> bool {
        int vals[4] = {};
        int n = 0;
        size_t i = i0;
        while (i < s.size() && n < 4) {
            while (i < s.size()
                && !(iswdigit(s[i]) || (s[i] == L'-' && i + 1 < s.size() && iswdigit(s[i + 1])))) {
                ++i;
            }
            if (i >= s.size()) break;
            wchar_t* end = nullptr;
            vals[n++] = static_cast<int>(wcstol(s.c_str() + i, &end, 10));
            if (end == s.c_str() + i) break;
            i = static_cast<size_t>(end - s.c_str());
        }
        if (n < 4) return false;
        outX1 = std::min(vals[0], vals[2]);
        outY1 = std::min(vals[1], vals[3]);
        outX2 = std::max(vals[0], vals[2]);
        outY2 = std::max(vals[1], vals[3]);
        return outX2 > outX1 && outY2 > outY1;
    };

    // 模型输出常有描述性前置（「第2个按钮在 […]」「框选第3个区域 (…)」），
    // 直接扫前 4 个整数会抓错；优先取方括号/圆括号分组内的数字。
    for (const wchar_t openCh : {L'[', L'('}) {
        const wchar_t closeCh = (openCh == L'[') ? L']' : L')';
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == openCh && i + 1 < s.size()) {
                size_t j = i + 1;
                while (j < s.size() && s[j] != closeCh) ++j;
                if (j < s.size() && parseFourAt(i + 1)) return true;
            }
        }
    }

    // 兜底：全文前 4 个整数
    return parseFourAt(0);
}

std::wstring ExtractClickTargetPhrase(const std::wstring& prompt) {
    std::wstring s = Trim(prompt);
    while (!s.empty() && (s.back() == L'。' || s.back() == L'！' || s.back() == L'？'
        || s.back() == L'，' || s.back() == L'.' || s.back() == L'!' || s.back() == L'?'
        || s.back() == L',')) {
        s.pop_back();
    }
    auto hasSpatialContext = [](const std::wstring& in) -> bool {
        return in.find(L"左侧") != std::wstring::npos
            || in.find(L"右侧") != std::wstring::npos
            || in.find(L"顶部") != std::wstring::npos
            || in.find(L"底部") != std::wstring::npos
            || in.find(L"导航") != std::wstring::npos
            || in.find(L"菜单") != std::wstring::npos
            || in.find(L"窗口") != std::wstring::npos
            || in.find(L"对话框") != std::wstring::npos
            || in.find(L"面板") != std::wstring::npos
            || in.find(L"位于") != std::wstring::npos
            || in.find(L"右上") != std::wstring::npos
            || in.find(L"左上") != std::wstring::npos
            || in.find(L"右下") != std::wstring::npos
            || in.find(L"左下") != std::wstring::npos
            || in.find(L"旁边") != std::wstring::npos
            || in.find(L"里的") != std::wstring::npos
            || in.find(L"中的") != std::wstring::npos
            || in.find(L"栏") != std::wstring::npos;
    };
    auto stripQuoteChars = [](std::wstring in) {
        std::wstring o;
        o.reserve(in.size());
        for (const wchar_t c : in) {
            if (c == L'"' || c == L'\'' || c == L'\u201c' || c == L'\u201d'
                || c == L'\u300c' || c == L'\u300d' || c == L'\u300e' || c == L'\u300f')
                continue;
            o.push_back(c);
        }
        return Trim(o);
    };
    // 优先取引号内短标签；但「桌面」「浏览」这类 ≤6 字且带方位时勿只留标签（易整屏乱框/点错）
    auto tryQuoted = [](const std::wstring& in, wchar_t open, wchar_t close) -> std::wstring {
        const size_t a = in.find(open);
        if (a == std::wstring::npos) return {};
        const size_t b = in.find(close, a + 1);
        if (b == std::wstring::npos || b <= a + 1) return {};
        std::wstring q = Trim(in.substr(a + 1, b - a - 1));
        if (q.empty() || q.size() > 24) return {};
        return q;
    };
    auto takeQuotedOrKeepSpatial = [&](wchar_t open, wchar_t close) -> bool {
        std::wstring q = tryQuoted(s, open, close);
        if (q.empty()) return false;
        if (q.size() <= 6 && hasSpatialContext(s)) {
            s = stripQuoteChars(s);
            return true; // 继续走下方前缀剥离，保留方位
        }
        s = std::move(q);
        return true; // 已是最终短语，仍可走前缀剥离（通常无前缀）
    };
    if (!takeQuotedOrKeepSpatial(L'\u201c', L'\u201d')
        && !takeQuotedOrKeepSpatial(L'"', L'"')
        && !takeQuotedOrKeepSpatial(L'\u300c', L'\u300d')
        && !takeQuotedOrKeepSpatial(L'\u300e', L'\u300f')
        && !takeQuotedOrKeepSpatial(L'\'', L'\'')) {
        // 无引号：整句处理
    }

    // 剥动作前缀（「开始」不在列表：开始按钮不能被误剥）
    static const wchar_t* kPrefixes[] = {
        L"请帮我点击", L"帮我点击一下", L"帮我点击", L"帮我打开", L"右键点击", L"左键点击",
        L"点击一下", L"单击", L"双击", L"点一下", L"点选", L"按下", L"点击", L"打开",
        L"启动", L"运行", L"定位", L"去点", L"寻找", L"找一下",
        L"click on", L"click the", L"click", L"open", L"find", L"locate",
    };
    bool changed = true;
    while (changed) {
        changed = false;
        for (const wchar_t* pfx : kPrefixes) {
            const size_t len = wcslen(pfx);
            if (s.size() > len && s.rfind(pfx, 0) == 0) {
                s = Trim(s.substr(len));
                changed = true;
            }
        }
    }
    // 多子句：默认取逗号前；若后段是方位/外形消歧则保留（邻近相似图标靠这个区分）
    for (size_t i = 0; i < s.size(); ++i) {
        const wchar_t c = s[i];
        if (c != L'，' && c != L',' && c != L'。' && c != L'；' && c != L';')
            continue;
        const std::wstring after = Trim(s.substr(i + 1));
        const bool keepDisambig =
            after.rfind(L"位于", 0) == 0 || after.rfind(L"在", 0) == 0
            || after.rfind(L"带", 0) == 0 || after.rfind(L"右侧", 0) == 0
            || after.rfind(L"左侧", 0) == 0 || after.rfind(L"顶部", 0) == 0
            || after.rfind(L"底部", 0) == 0 || after.rfind(L"右上", 0) == 0
            || after.rfind(L"左上", 0) == 0 || after.rfind(L"右下", 0) == 0
            || after.rfind(L"左下", 0) == 0 || after.rfind(L"水平", 0) == 0
            || after.rfind(L"垂直", 0) == 0 || after.rfind(L"时钟", 0) == 0
            || after.rfind(L"三个", 0) == 0 || after.find(L"旁边") != std::wstring::npos
            || after.find(L"相邻") != std::wstring::npos;
        if (keepDisambig) {
            // 保留「标签 + 消歧」，总长仍钳到 80
            break;
        }
        s = Trim(s.substr(0, i));
        break;
    }
    if (s.size() > 80)
        s.resize(80);
    return s;
}

bool PromptIntendsDoubleClick(const std::wstring& prompt) {
    return Trim(prompt).find(L"双击") != std::wstring::npos;
}

bool PromptIntendsRightClick(const std::wstring& prompt) {
    const std::wstring p = Trim(prompt);
    return p.find(L"右键") != std::wstring::npos
        || p.find(L"right click") != std::wstring::npos;
}

bool IsVisionLocateNotFound(const std::wstring& text) {
    const std::wstring s = Trim(text);
    if (s.empty()) return false;
    std::wstring upper = s;
    for (auto& c : upper) {
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
    }
    if (upper.find(L"NOT_FOUND") != std::wstring::npos
        || upper.find(L"NOTFOUND") != std::wstring::npos
        || upper.find(L"NOT FOUND") != std::wstring::npos
        || upper.find(L"NO_ELEMENT") != std::wstring::npos
        || upper == L"NONE" || upper == L"NULL")
        return true;
    // 短回复里的明确否定（避免长思考误伤）
    if (s.size() <= 80) {
        if (s.find(L"找不到") != std::wstring::npos
            || s.find(L"未找到") != std::wstring::npos
            || s.find(L"没有找到") != std::wstring::npos
            || s.find(L"无法定位") != std::wstring::npos
            || s.find(L"无法识别") != std::wstring::npos
            || s.find(L"未检测到") != std::wstring::npos
            || s.find(L"检测不到") != std::wstring::npos
            || s.find(L"未识别到") != std::wstring::npos
            || s.find(L"不存在") != std::wstring::npos
            || s.find(L"看不见") != std::wstring::npos
            || s.find(L"没看到") != std::wstring::npos
            || s.find(L"没有该") != std::wstring::npos
            || s.find(L"无此") != std::wstring::npos)
            return true;
    }
    return false;
}

bool IsVisionApiBoxTooLarge(int apiX1, int apiY1, int apiX2, int apiY2,
    int apiW, int apiH, double maxAreaRatio) {
    if (apiW <= 0 || apiH <= 0) return false;
    const long long bw = std::max(0, apiX2 - apiX1);
    const long long bh = std::max(0, apiY2 - apiY1);
    const long long area = bw * bh;
    const long long img = static_cast<long long>(apiW) * apiH;
    if (img <= 0) return false;
    const double ratio = static_cast<double>(area) / static_cast<double>(img);
    if (ratio > std::clamp(maxAreaRatio, 0.1, 0.95)) return true;
    // 单边占图过大：整列侧栏/整块面板乱框（面积比可能仍 <0.35）
    const double wFrac = static_cast<double>(bw) / static_cast<double>(apiW);
    const double hFrac = static_cast<double>(bh) / static_cast<double>(apiH);
    if (hFrac > 0.42 && wFrac > 0.12) return true;
    if (wFrac > 0.55 && hFrac > 0.22) return true;
    return false;
}

void MapApiPointToScreen(const AiCaptureMapping& map, int apiX, int apiY, int& screenX, int& screenY) {
    const int regionW = std::max(1, map.capX2 - map.capX1);
    const int regionH = std::max(1, map.capY2 - map.capY1);
    const int apiW = std::max(1, map.apiWidth > 0 ? map.apiWidth : map.srcWidth);
    const int apiH = std::max(1, map.apiHeight > 0 ? map.apiHeight : map.srcHeight);
    screenX = map.capX1 + static_cast<int>(static_cast<double>(apiX) * regionW / apiW);
    screenY = map.capY1 + static_cast<int>(static_cast<double>(apiY) * regionH / apiH);
}

bool IsApiPointClearlyOutsideImage(int apiX, int apiY, int apiW, int apiH) {
    if (apiW <= 0 || apiH <= 0) return true;
    // 允许 2px 边缘误差；大幅越界视为幻觉（如 720 宽图却给 984）
    return apiX < -2 || apiY < -2 || apiX >= apiW + 2 || apiY >= apiH + 2;
}

bool TryClampApiPointToImage(int& apiX, int& apiY, int apiW, int apiH) {
    if (IsApiPointClearlyOutsideImage(apiX, apiY, apiW, apiH)) return false;
    if (apiW <= 0 || apiH <= 0) return false;
    apiX = std::clamp(apiX, 0, apiW - 1);
    apiY = std::clamp(apiY, 0, apiH - 1);
    return true;
}

namespace {

bool VisionCoordsFitImage(int x1, int y1, int x2, int y2, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    return x1 >= -2 && y1 >= -2 && x2 <= w + 2 && y2 <= h + 2
        && x2 > x1 && y2 > y1;
}

bool VisionPointFitsImage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return false;
    return x >= -2 && y >= -2 && x < w + 2 && y < h + 2;
}

bool VisionCoordsInUnit1000(int a, int b, int c, int d) {
    const int lo = std::min(std::min(a, b), std::min(c, d));
    const int hi = std::max(std::max(a, b), std::max(c, d));
    return lo >= 0 && hi <= 1000;
}

void ScaleVisionRect(int& x1, int& y1, int& x2, int& y2,
    int fromW, int fromH, int toW, int toH) {
    fromW = std::max(1, fromW);
    fromH = std::max(1, fromH);
    toW = std::max(1, toW);
    toH = std::max(1, toH);
    x1 = static_cast<int>(static_cast<long long>(x1) * toW / fromW);
    y1 = static_cast<int>(static_cast<long long>(y1) * toH / fromH);
    x2 = static_cast<int>(static_cast<long long>(x2) * toW / fromW);
    y2 = static_cast<int>(static_cast<long long>(y2) * toH / fromH);
}

}  // namespace

bool ResolveVisionPointToApiImage(
    int& x, int& y,
    int apiW, int apiH, int srcW, int srcH,
    std::wstring* outNote) {
    if (outNote) outNote->clear();
    if (apiW <= 0 || apiH <= 0) return false;

    // 多数 grounding（含豆包）固定输出 0~1000。图内像素与 0~1000 重叠时优先归一化，
    // 否则会把 [677,134,…] 当成 1024×576 像素点到错误位置（搜索钮→快捷方式栏）。
    if (VisionCoordsInUnit1000(x, y, x, y)
        && (apiW != 1000 || apiH != 1000)) {
        x = static_cast<int>(static_cast<long long>(x) * apiW / 1000);
        y = static_cast<int>(static_cast<long long>(y) * apiH / 1000);
        x = std::clamp(x, 0, apiW - 1);
        y = std::clamp(y, 0, apiH - 1);
        if (outNote) *outNote = L"0~1000归一化";
        return true;
    }

    if (VisionPointFitsImage(x, y, apiW, apiH)) {
        x = std::clamp(x, 0, apiW - 1);
        y = std::clamp(y, 0, apiH - 1);
        if (outNote) *outNote = L"上传图像素";
        return true;
    }

    if (srcW > 0 && srcH > 0 && (srcW != apiW || srcH != apiH)
        && VisionPointFitsImage(x, y, srcW, srcH)) {
        x = static_cast<int>(static_cast<long long>(x) * apiW / srcW);
        y = static_cast<int>(static_cast<long long>(y) * apiH / srcH);
        x = std::clamp(x, 0, apiW - 1);
        y = std::clamp(y, 0, apiH - 1);
        if (outNote) *outNote = L"截屏原图像素";
        return true;
    }

    return false;
}

bool ResolveVisionRectToApiImage(
    int& x1, int& y1, int& x2, int& y2,
    int apiW, int apiH, int srcW, int srcH,
    std::wstring* outNote) {
    if (outNote) outNote->clear();
    if (apiW <= 0 || apiH <= 0) return false;
    if (x2 < x1) std::swap(x1, x2);
    if (y2 < y1) std::swap(y1, y2);
    if (x2 <= x1 || y2 <= y1) return false;

    // 与 Point 相同：0~1000 优先于「恰好落在上传图内」的像素解释
    if (VisionCoordsInUnit1000(x1, y1, x2, y2)
        && (apiW != 1000 || apiH != 1000)) {
        ScaleVisionRect(x1, y1, x2, y2, 1000, 1000, apiW, apiH);
        x1 = std::clamp(x1, 0, apiW - 1);
        y1 = std::clamp(y1, 0, apiH - 1);
        x2 = std::clamp(x2, 0, apiW - 1);
        y2 = std::clamp(y2, 0, apiH - 1);
        if (x2 <= x1 || y2 <= y1) return false;
        if (outNote) *outNote = L"0~1000归一化";
        return true;
    }

    if (VisionCoordsFitImage(x1, y1, x2, y2, apiW, apiH)) {
        x1 = std::clamp(x1, 0, apiW - 1);
        y1 = std::clamp(y1, 0, apiH - 1);
        x2 = std::clamp(x2, 0, apiW - 1);
        y2 = std::clamp(y2, 0, apiH - 1);
        if (x2 <= x1 || y2 <= y1) return false;
        if (outNote) *outNote = L"上传图像素";
        return true;
    }

    if (srcW > 0 && srcH > 0 && (srcW != apiW || srcH != apiH)
        && VisionCoordsFitImage(x1, y1, x2, y2, srcW, srcH)) {
        ScaleVisionRect(x1, y1, x2, y2, srcW, srcH, apiW, apiH);
        x1 = std::clamp(x1, 0, apiW - 1);
        y1 = std::clamp(y1, 0, apiH - 1);
        x2 = std::clamp(x2, 0, apiW - 1);
        y2 = std::clamp(y2, 0, apiH - 1);
        if (x2 <= x1 || y2 <= y1) return false;
        if (outNote) *outNote = L"截屏原图像素";
        return true;
    }

    return false;
}

bool ResolveAgentPointerToApiImage(
    int& x, int& y,
    int apiW, int apiH, int srcW, int srcH,
    std::wstring* outNote) {
    if (outNote) outNote->clear();
    if (apiW <= 0 || apiH <= 0) return false;

    // 1) 明显在图内（含 2px 边）→ 按像素；明显越界不走这条（避免 860 被当右缘像素）
    if (!IsApiPointClearlyOutsideImage(x, y, apiW, apiH)
        && VisionPointFitsImage(x, y, apiW, apiH)) {
        x = std::clamp(x, 0, apiW - 1);
        y = std::clamp(y, 0, apiH - 1);
        if (outNote) *outNote = L"上传图像素";
        return true;
    }

    // 2) 越界但仍像 0~1000（模型沿用识图习惯）
    if (VisionCoordsInUnit1000(x, y, x, y)
        && (apiW != 1000 || apiH != 1000)) {
        x = static_cast<int>(static_cast<long long>(x) * apiW / 1000);
        y = static_cast<int>(static_cast<long long>(y) * apiH / 1000);
        x = std::clamp(x, 0, apiW - 1);
        y = std::clamp(y, 0, apiH - 1);
        if (outNote) *outNote = L"0~1000归一化";
        return true;
    }

    // 3) 像截屏原图像素
    if (srcW > 0 && srcH > 0 && (srcW != apiW || srcH != apiH)
        && !IsApiPointClearlyOutsideImage(x, y, srcW, srcH)
        && VisionPointFitsImage(x, y, srcW, srcH)) {
        x = static_cast<int>(static_cast<long long>(x) * apiW / srcW);
        y = static_cast<int>(static_cast<long long>(y) * apiH / srcH);
        x = std::clamp(x, 0, apiW - 1);
        y = std::clamp(y, 0, apiH - 1);
        if (outNote) *outNote = L"截屏原图像素";
        return true;
    }

    return false;
}

bool IsRefineScreenDriftTooFar(int priorX, int priorY, int nextX, int nextY, int maxDistPx) {
    if (maxDistPx <= 0) return false;
    const long long dx = static_cast<long long>(nextX) - priorX;
    const long long dy = static_cast<long long>(nextY) - priorY;
    return (dx * dx + dy * dy) > static_cast<long long>(maxDistPx) * maxDistPx;
}

bool ShouldAcceptCoarseLocateWithoutRefine(
    const CoarseLocateRefineGateInput& in,
    CoarseLocateSkipReason* outReason) {
    if (outReason) *outReason = CoarseLocateSkipReason::None;

    // Midscene：清晰目标常只回中心点；归一化点再 Zoom 多为白烧
    if (in.pointOnly && in.adaptiveRefineDepth && in.coordsWereRemapped) {
        if (outReason) *outReason = CoarseLocateSkipReason::PointRemapped;
        return true;
    }

    if (!in.haveScreenBox || in.boxW <= 0 || in.boxH <= 0) return false;

    const int minSide = (std::max)(12, in.compactBboxMinSide);
    // 紧凑判定随观察区宽度缩放：4K/超宽屏下一个 200px 按钮相对很小，不应强迫 Zoom 精炼。
    // 基准 min(in.compactBboxMaxSide, 观察区宽 8%)，再钳到 [160, 320]。
    const int capW = (std::max)(1, in.captureW);
    const int scaledMaxSide = (std::min)(320,
        (std::max)(in.compactBboxMaxSide, capW * 8 / 100));
    const int maxSide = (std::max)(minSide, scaledMaxSide);
    const bool compactBox = in.boxW >= minSide && in.boxH >= minSide
        && in.boxW <= maxSide && in.boxH <= maxSide;
    const bool wideControl = in.boxH >= 14 && in.boxH <= 96
        && in.boxW >= in.boxH * 2 && in.boxW <= 1200;

    const int capH = (std::max)(1, in.captureH);
    const double areaRatio = static_cast<double>(in.boxW) * static_cast<double>(in.boxH)
        / (static_cast<double>(capW) * static_cast<double>(capH));
    const double aspect = static_cast<double>(in.boxW) / static_cast<double>((std::max)(1, in.boxH));
    const double areaMax = in.compactAreaRatioMax > 0.0 ? in.compactAreaRatioMax : 0.03;
    const double aspectMin = in.compactAspectMin > 0.0 ? in.compactAspectMin : 0.35;
    const double aspectMax = in.compactAspectMax > aspectMin ? in.compactAspectMax : 3.0;
    const bool areaOk = areaRatio <= areaMax;
    const bool aspectOk = aspect >= aspectMin && aspect <= aspectMax;
    // 文件名框/地址栏：很扁很长，高度常 <24；再 Zoom 易裁掉目标 → NOT_FOUND
    const bool thinWideBar = in.boxH >= 12 && in.boxH < 28
        && in.boxW >= 120 && aspect >= 4.0 && areaRatio <= 0.08;

    // 0~1000 / 原图像素换算后的粗框：中心通常已可点；宽控件再 Zoom 易白烧一轮
    // 邻近工具栏小图标（边 < 56）：强制 Zoom，避免点到隔壁相似图标
    if (in.coordsWereRemapped) {
        if (thinWideBar) {
            if (outReason) *outReason = CoarseLocateSkipReason::WideControlRemapped;
            return true;
        }
        const int maxSideBox = (std::max)(in.boxW, in.boxH);
        if (maxSideBox > 0 && maxSideBox < 56) {
            return false;
        }
        if ((compactBox || wideControl) && in.boxH >= 18
            && (compactBox ? in.boxW >= 64 : true)) {
            if (outReason) {
                *outReason = (wideControl && !compactBox)
                    ? CoarseLocateSkipReason::WideControlRemapped
                    : CoarseLocateSkipReason::CompactRemapped;
            }
            return true;
        }
        return false;
    }

    // 上传图像素空间：旧开关（无面积比门槛）
    if (in.acceptCompactBboxWithoutRefine && compactBox) {
        if (outReason) *outReason = CoarseLocateSkipReason::CompactPixel;
        return true;
    }

    // 自适应（对齐 Midscene：仅小/糊目标才 deepLocate；紧凑且相对观察区够小则跳过）
    // 略抬最小边，降低任务栏邻近小图标误点风险
    if (in.adaptiveRefineDepth && compactBox && areaOk && aspectOk
        && in.boxW >= 32 && in.boxH >= 28) {
        if (outReason) *outReason = CoarseLocateSkipReason::CompactPixel;
        return true;
    }
    return false;
}

void MapApiRectToScreen(const AiCaptureMapping& map,
    int apiX1, int apiY1, int apiX2, int apiY2,
    int& screenX1, int& screenY1, int& screenX2, int& screenY2) {
    MapApiPointToScreen(map, apiX1, apiY1, screenX1, screenY1);
    MapApiPointToScreen(map, apiX2, apiY2, screenX2, screenY2);
    if (screenX2 < screenX1) std::swap(screenX1, screenX2);
    if (screenY2 < screenY1) std::swap(screenY1, screenY2);
}

void BuildZoomRoiAroundScreenPoint(
    int screenX, int screenY,
    int halfSide,
    int boundX1, int boundY1, int boundX2, int boundY2,
    int& outX1, int& outY1, int& outX2, int& outY2) {
    const int hs = std::max(20, halfSide);
    const int bx1 = std::min(boundX1, boundX2);
    const int by1 = std::min(boundY1, boundY2);
    const int bx2 = std::max(boundX1, boundX2);
    const int by2 = std::max(boundY1, boundY2);
    outX1 = screenX - hs;
    outY1 = screenY - hs;
    outX2 = screenX + hs;
    outY2 = screenY + hs;
    if (outX1 < bx1) { outX2 += (bx1 - outX1); outX1 = bx1; }
    if (outY1 < by1) { outY2 += (by1 - outY1); outY1 = by1; }
    if (outX2 > bx2) { outX1 -= (outX2 - bx2); outX2 = bx2; }
    if (outY2 > by2) { outY1 -= (outY2 - by2); outY2 = by2; }
    outX1 = std::max(bx1, outX1);
    outY1 = std::max(by1, outY1);
    outX2 = std::min(bx2, outX2);
    outY2 = std::min(by2, outY2);
    if (outX2 <= outX1 + 4) { outX1 = bx1; outX2 = bx2; }
    if (outY2 <= outY1 + 4) { outY1 = by1; outY2 = by2; }
}

std::wstring BuildScreenClickActionsJson(int screenX, int screenY, bool includeStopMacro,
    const std::wstring& button, int clickCount) {
    const std::wstring btn = (button == L"right") ? L"right"
        : (button == L"middle") ? L"middle" : L"left";
    const int clicks = std::clamp(clickCount, 1, 2);
    // coordSpace=screen：已是屏幕绝对坐标，宿主执行时禁止再按截图比例二次映射
    std::wstring json = L"[{\"type\":\"moveMouse\",\"x\":" + std::to_wstring(screenX)
        + L",\"y\":" + std::to_wstring(screenY) + L",\"coordSpace\":\"screen\"}"
        L",{\"type\":\"mouseClick\",\"x\":" + std::to_wstring(screenX)
        + L",\"y\":" + std::to_wstring(screenY)
        + L",\"button\":\"" + btn + L"\",\"coordSpace\":\"screen\""
        + L",\"clickCount\":" + std::to_wstring(clicks)
        // duration 是两次点击的间隔：双击必须远小于系统双击时限
        + L",\"duration\":" + (clicks > 1 ? std::wstring(L"0.04") : std::wstring(L"0.01"))
        + L"}";
    if (includeStopMacro) json += L",{\"type\":\"stopMacro\"}";
    json += L"]";
    return json;
}

std::wstring MacroActionCompositeSkill() {
    return LR"(【组合 — section=composite】
locateAndClick(短标签)：整图 VLM→紧凑/归一化点可跳过 Zoom。refineLevels 默认1，难目标2。
双击打开图标 doubleClick=true；右键 button=right。NOT_FOUND→换描述/滚动，勿盲键。
findImage/findColor 有模板时优先。多步见 section=agent。
)";
}

std::wstring MacroActionUsageSkill() {
    return LR"(【场景用法 — lookupMacroAction section=usage】
多步任务见 section=agent。动手前先 updateTaskMemo(section=goal|todos)。

── 打开 / 导航 ──
openWebpage → 宿主 settle 后再观察。已在目标页禁止重开。白屏只 wait。
浏览器内置页：locateAndClick 点菜单逐级找入口，勿猜应用专属快捷键。
不确定某界面的快捷键/操作时：fetchWebPage 查官方文档（如「软件名 快捷键」）再动手。

── 窗口 / 启动 ──
listWindows → activateWindow(match=标题关键词) 复用已开窗口。
runProgram / openAppViaSearch 阶梯启动。Win+D 只会藏窗口。
★对话/聊天类界面：先核对截图里能看到「消息区 + 输入区」再操作（输入框通常在界面底部）。
  若截图中没有输入框，说明当前画面不是对话框本体：先 listWindows 看标题，
  用 activateWindow 切到含对话框的窗口，确认截图出现输入框后再 locate；
  不要在没有输入框的画面上反复 locate（必然 NOT_FOUND）。

── 数据缓存 ──
读到列表/表格后立刻 saveTaskData(name, content)；填写用 readTaskData。
禁止为同一数据再开第二视图；已看见就直接抄。

── 对话框 / 路径 ──
对话框（另存为等）以观察为准；灰按钮=前置未满足。
★另存为导航：resolveSystemPath → 文件名框 quickInput(完整路径或纯文件名, clearFirst) → Enter；
禁止 scrollWheel 空转找「桌面」。侧栏滚动条用 mouseDrag 拖滑块。

── 拖拽 ──
mouseDrag(fromX,fromY,toX,toY)：拖滚动条滑块、拖文件、框选。滚轮无效时优先拖。

── 打开文件/图标 ──
openFile(路径) 或 locateAndClick(doubleClick=true)。单击只选中打不开。
重命名：F2 → quickInput(主名, clearFirst=false) → Enter。

── 配方复用 ──
验证 ≥2 组完整循环（含组间转移）后 runActionRecipe(steps, rows) 批量。
模板只放每行重复步骤，勿含表头固定文本。

── 定位 / 点击 ──
locateAndClick(短标签≤80字)；表格起点 Ctrl+Home。未找到换描述最多1次。
灰按钮=前置未满足，改输入别连点。列表翻页用 scrollWheel；对话框侧栏用 mouseDrag。
★点击输入框成功后：下一步直接 quickInput 输入，不要先滚动/切窗/按 Tab——
  滚轮常作用在鼠标所在窗口而非目标窗口，输入框也不需要滚动就能输入。

── 回复 / 发送 ──
quickInput 后 keyClick(Enter) 或 locateAndClick(发送按钮)。

工具：openWebpage、runProgram、openFile、scrollWheel、mouseDrag、quickInput、keyClick、
locateAndClick、resolveSystemPath、activateWindow、runActionRecipe、saveTaskData、
readTaskData、switchIme、completeTask；少用 mouseClick/aiActionExecute。
)";
}

std::wstring MacroActionAgentSkill() {
    return LR"(【Agent — section=agent】
看清→同轮多工具→关键点验收→completeTask。按画面自适应。

· updateTaskMemo(goal|todos)
· locateAndClick(短标签)；quickInput(命名 clearFirst=true)；scrollWheel 翻列表；mouseDrag 拖滚动条/拖拽
· 点中输入框后直接 quickInput，勿先滚动/切窗/按 Tab；滚动后画面没变=滚错了窗口，立即停手
· 切窗 activateWindow；启动 runProgram / openAppViaSearch；路径 resolveSystemPath
· 对话类界面先核对截图有没有输入框；没有就先 listWindows/activateWindow 切到对话框再定位
· 另存为：完整路径写入文件名框，禁止空转 scrollWheel 找侧栏
· 对话框看图+探测事实，勿盲猜应用快捷键；灰钮改前置条件
· 未知界面的快捷键/操作：先 fetchWebPage 查官方文档或快捷键列表
  （如「软件名 快捷键」「软件名 导出设置」），引用来源，禁止编造快捷键
  或盲猜 F12/应用专属键；查不到就按画面探测，勿碰运气
· 表格：Ctrl+Home→runActionRecipe(rows 勿空)；失败禁连 Tab，先 Home 再改 rows
· 重复：runActionRecipe（试跑→verifiedGroups）；行末 Enter+Home
· 宿主已 settle，勿空 wait
)";
}

std::wstring BuildAiActionToolExecuteSystemPrompt(
    int captureWidth, int captureHeight, bool withImage) {
    std::wstring prompt =
        L"Windows 宏 Agent：只靠工具。细则 lookupMacroAction(section=agent)。\n"
        L"先 updateTaskMemo(goal|todos)。同轮多工具。按观察行动。\n";
    if (withImage && captureWidth > 0 && captureHeight > 0) {
        prompt += L"图 " + std::to_wstring(captureWidth) + L"×" + std::to_wstring(captureHeight)
            + L"。\n";
    }
    return prompt;
}

std::wstring BuildAiActionHybridSystemPrompt(int imageWidth, int imageHeight) {
    std::wstring s =
        L"Windows 宏 Agent：只靠工具。细则 lookupMacroAction(section=agent)。\n"
        L"先 updateTaskMemo(goal|todos)。同轮多工具。未变不传新图。\n"
        L"批量冷门动作可用 submitMacroActions；启动兜底 openAppViaSearch。\n";
    if (imageWidth > 0 && imageHeight > 0) {
        s += L"图/upload " + std::to_wstring(imageWidth) + L"×" + std::to_wstring(imageHeight)
            + L"。\n";
    }
    return s;
}

std::wstring BuildCompositeLocatePrompt(const std::wstring& userTask,
    int imageWidth, int imageHeight) {
    std::wstring p = Trim(userTask);
    if (p.empty()) p = L"目标";
    std::wstring s = L"目标：" + p + L"\n";
    if (imageWidth > 0 && imageHeight > 0) {
        s += std::to_wstring(imageWidth) + L"x" + std::to_wstring(imageHeight) + L"\n";
    }
    s += L"输出 [x1,y1,x2,y2]（0~1000）。整图找；多个相似只框描述完全匹配的那一个；无则 NOT_FOUND。";
    return s;
}

std::wstring BuildCompositeRefinePointPrompt(const std::wstring& userTask, int levelIndex,
    int imageWidth, int imageHeight) {
    std::wstring p = Trim(userTask);
    if (p.empty()) p = L"目标";
    (void)levelIndex;
    std::wstring s = L"精点：" + p + L"\n";
    if (imageWidth > 0 && imageHeight > 0) {
        s += std::to_wstring(imageWidth) + L"x" + std::to_wstring(imageHeight) + L"\n";
    }
    s += L"只输出 (x,y) 0~1000。多个相似控件勿点邻居；无则 NOT_FOUND。";
    return s;
}

std::wstring BuildCompositeCorrectLocatePrompt(const std::wstring& userTask,
    int prevX1, int prevY1, int prevX2, int prevY2,
    int imageWidth, int imageHeight) {
    std::wstring p = Trim(userTask);
    if (p.empty()) p = L"目标";
    std::wstring s = L"目标：" + p + L"\n";
    if (imageWidth > 0 && imageHeight > 0) {
        s += std::to_wstring(imageWidth) + L"x" + std::to_wstring(imageHeight) + L"\n";
    }
    s += L"重框 ["
        + std::to_wstring(prevX1) + L"," + std::to_wstring(prevY1) + L","
        + std::to_wstring(prevX2) + L"," + std::to_wstring(prevY2)
        + L"]。重新框选真正目标，输出 [x1,y1,x2,y2] 0~1000；无则 NOT_FOUND。";
    return s;
}

std::wstring BuildAiActionVisionQuerySystemPrompt(int imageWidth, int imageHeight) {
    std::wstring s =
        L"根据截图直接回答。优先 [x1,y1,x2,y2]（0~1000），否则 (x,y)。无多余解释。";
    if (imageWidth > 0 && imageHeight > 0) {
        s += L" 图 " + std::to_wstring(imageWidth) + L"×"
            + std::to_wstring(imageHeight) + L"。";
    }
    return s;
}
