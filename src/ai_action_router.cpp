#include "ai_action_router.h"

#include "script_action_builder.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
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

// ★★「打开目标」解析：把模型给的**自然表达**变成可直接打开的 URL（2026-09-28 加）
//
// 为什么需要它（用户实测报障「卡在莫名其妙的地方」，第一轮就白烧两轮）：
//   任务写「帮我打开哔哩哔哩」，模型就照着说 `openWebpage`（**不带地址**）⇒
//   旧实现回一句 `targetPath 不能为空` ⇒ 模型再试一次同样的调用 ⇒ 又失败。
//   而"打开哔哩哔哩"本来就该能打开 —— 这不是模型的错，是**工具只认完整 URL**。
//
// 判据顺序（都是**通用**规则，不含站点专属判断题）：
//   ① 已是 http/https ⇒ 原样；
//   ② 像域名（有点、无空格、无中文）⇒ 补 `https://`；
//   ③ 命中**常见站点名字典**（数据表，非判断逻辑）⇒ 用它的首页；
//   ④ 其余 ⇒ 返回空，由调用方给出**可执行**的错误（列出可用的名字示例）。
// ⚠ 这张表是"便利映射"，不是"引擎替用户决定"：解析不到时**如实说不知道**，
//   绝不猜一个相近的站点打开（打开错的网站比不打开更糟）。
std::wstring ResolveOpenWebpageTargetUrl(const std::wstring& raw) {
    std::wstring t = Trim(raw);
    if (t.empty()) return {};
    // 去掉模型常带的包裹字符：`"url"`、`<url>`、`（url）`
    while (!t.empty() && (t.front() == L'"' || t.front() == L'\'' || t.front() == L'<'
            || t.front() == L'（' || t.front() == L'(' || t.front() == L'“')) {
        t.erase(t.begin());
    }
    while (!t.empty() && (t.back() == L'"' || t.back() == L'\'' || t.back() == L'>'
            || t.back() == L'）' || t.back() == L')' || t.back() == L'”'
            || t.back() == L'。' || t.back() == L'.')) {
        // ⚠ 句点只在**结尾且后面没有域名成分**时剥掉（`www.bilibili.com` 的 m 结尾不受影响）
        t.pop_back();
    }
    t = Trim(t);
    if (t.empty()) return {};
    const std::wstring lower = [&] {
        std::wstring o = t;
        for (auto& c : o) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        return o;
    }();
    // ① 已有协议
    if (lower.rfind(L"http://", 0) == 0 || lower.rfind(L"https://", 0) == 0) return t;
    // 已知的浏览器内置页原样返回（由调用方按内置页路径处理）
    if (lower.rfind(L"edge://", 0) == 0 || lower.rfind(L"chrome://", 0) == 0
        || lower.rfind(L"about:", 0) == 0) {
        return t;
    }
    // ② 像域名：含点、无空格、无中文 ⇒ 补协议
    {
        bool hasDot = lower.find(L'.') != std::wstring::npos;
        bool hasSpace = lower.find(L' ') != std::wstring::npos
            || lower.find(L'　') != std::wstring::npos;
        bool hasCjk = false;
        for (wchar_t c : lower) {
            if (c >= 0x2E80) { hasCjk = true; break; }
        }
        if (hasDot && !hasSpace && !hasCjk) return L"https://" + t;
    }
    // ③ 常见站点名字典（小写子串匹配；**数据**，不是判断逻辑）
    struct SiteNameRow { const wchar_t* key; const wchar_t* home; };
    static const SiteNameRow kSites[] = {
        { L"哔哩哔哩", L"https://www.bilibili.com/" },
        { L"bilibili", L"https://www.bilibili.com/" },
        { L"b站", L"https://www.bilibili.com/" },
        { L"百度", L"https://www.baidu.com/" },
        { L"baidu", L"https://www.baidu.com/" },
        { L"知乎", L"https://www.zhihu.com/" },
        { L"zhihu", L"https://www.zhihu.com/" },
        { L"淘宝", L"https://www.taobao.com/" },
        { L"天猫", L"https://www.tmall.com/" },
        { L"京东", L"https://www.jd.com/" },
        { L"微博", L"https://weibo.com/" },
        { L"抖音", L"https://www.douyin.com/" },
        { L"小红书", L"https://www.xiaohongshu.com/" },
        { L"豆瓣", L"https://www.douban.com/" },
        { L"网易", L"https://www.163.com/" },
        { L"腾讯", L"https://www.qq.com/" },
        { L"优酷", L"https://www.youku.com/" },
        { L"爱奇艺", L"https://www.iqiyi.com/" },
        { L"youtube", L"https://www.youtube.com/" },
        { L"github", L"https://github.com/" },
        { L"必应", L"https://www.bing.com/" },
        { L"bing", L"https://www.bing.com/" },
        { L"谷歌", L"https://www.google.com/" },
        { L"google", L"https://www.google.com/" },
        { L"搜狗", L"https://www.sogou.com/" },
    };
    for (const auto& row : kSites) {
        if (lower.find(row.key) != std::wstring::npos) return row.home;
    }
    return {};
}

/// 解析失败时的**可执行**错误（列出可用写法；模型看得到，就能自己纠正）
std::wstring OpenWebpageTargetHint() {
    return L"。请给**完整网址**（如 https://www.bilibili.com/），或**常见站点名**"
           L"（哔哩哔哩/B站、百度、知乎、淘宝、京东、微博、抖音、小红书、豆瓣、"
           L"YouTube、GitHub、必应、谷歌…），或浏览器内置页（edge://history 等）。";
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
    // ★★ **交互类动词**（2026-09-28 补）：这些在语义上就是"在界面上点某个东西"，
    //   但字面上不含「点击」二字，旧判据会漏 ⇒ 未勾选带截图时首轮无图，模型**盲着干**。
    //   用户实测报障：「帮我打开哔哩哔哩，给up主"有山先生"主页的第一个视频**点赞**」
    //   —— 判成"无需看屏"，模型看不到屏幕只能猜（点到了帮助中心那种莫名其妙的地方）。
    //
    //   ⚠ 取舍：这个判据只决定**要不要多截一帧**（约 0.2~0.4s + 一份图 token），
    //     判"需要"的代价很小，判"不需要"的代价是整轮瞎猜 ⇒ **交互动词一律算需要**。
    //     而纯数据任务（读文件 / 跑命令 / 分析文本 / 算数）**仍然判不需要**，别放水成恒 true。
    if (HasAny(p, {L"点赞", L"点踩", L"投币", L"收藏", L"关注", L"取消关注", L"转发", L"分享",
            L"评论", L"发弹幕", L"订阅", L"加入购物车", L"下单", L"购买", L"支付", L"付款",
            L"提交", L"确认提交", L"登录", L"注册", L"退出登录", L"切换账号",
            L"发送消息", L"发布", L"上传", L"下载", L"安装", L"勾选", L"取消勾选",
            L"翻页", L"下一页", L"上一页", L"播放", L"暂停", L"全屏", L"退出全屏",
            L"下拉菜单", L"右键菜单", L"扫码"})) {
        return true;
    }
    return false;
}

namespace {

/// 从 s[i] 起跳过非数字字符后读一个数（支持小数/正负号），四舍五入成像素并推进 i。
/// ★为什么必须支持小数：带 grounding 训练或长推理链的模型经常回 `(88.5,117.3)` /
/// `[52.4,100.2,127.6,137.9]`。旧实现用 `wcstol` 逐个抓整数：
///   · 点对 `(88.5,117.3)` → x=88，接着 y 从 ".5" 解析失败 → 整条判定「无法解析坐标」（白烧一轮 API）；
///   · 框 `[52.4,100.2,...]` → 更糟：抓到 52、再跳到小数点后的 5 当 y1 → **静默错框**。
/// 归一化空间的 1 个单位在 2560 宽屏上是 2.56px，取整损耗本身可忽略，但「解析失败/错框」
/// 是实打实的定位事故（研究结论：坐标空间与取整误差必须先排除，再谈模型能力）。
bool ReadNumberAt(const std::wstring& s, size_t& i, double* out) {
    while (i < s.size()
        && !(iswdigit(s[i]) || s[i] == L'-' || s[i] == L'+'
            || (s[i] == L'.' && i + 1 < s.size() && iswdigit(s[i + 1])))) {
        ++i;
    }
    if (i >= s.size()) return false;
    wchar_t* end = nullptr;
    const double v = wcstod(s.c_str() + i, &end);
    if (end == s.c_str() + i) return false;
    i = static_cast<size_t>(end - s.c_str());
    *out = v;
    return true;
}

int RoundToInt(double v) {
    return static_cast<int>(std::lround(v));
}

}  // namespace

bool TryParseCoordinatePair(const std::wstring& text, int& outX, int& outY) {
    outX = outY = 0;
    const std::wstring s = Trim(text);
    if (s.empty()) return false;

    auto parsePairAt = [&](size_t i0) -> bool {
        size_t i = i0;
        double x = 0, y = 0;
        if (!ReadNumberAt(s, i, &x)) return false;
        if (!ReadNumberAt(s, i, &y)) return false;
        outX = RoundToInt(x);
        outY = RoundToInt(y);
        return true;
    };

    const size_t lp = s.find(L'(');
    const size_t rp = s.find(L')', lp == std::wstring::npos ? 0 : lp);
    if (lp != std::wstring::npos && rp != std::wstring::npos && rp > lp + 1)
        return parsePairAt(lp + 1);

    for (size_t i = 0; i < s.size(); ++i) {
        if (iswdigit(s[i]) || (s[i] == L'-' && i + 1 < s.size() && iswdigit(s[i + 1]))) {
            if (parsePairAt(i)) return true;
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
            double v = 0;
            // 支持小数（见 ReadNumberAt 的注释：旧实现会把 "52.4" 抓成 52 和 5 → 静默错框）
            if (!ReadNumberAt(s, i, &v)) break;
            vals[n++] = RoundToInt(v);
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

AiVisionAnswerKind ParseVisionLocateAnswer(
    const std::wstring& text, int& outX1, int& outY1, int& outX2, int& outY2) {
    outX1 = outY1 = outX2 = outY2 = 0;
    const std::wstring s = Trim(text);
    if (s.empty()) return AiVisionAnswerKind::NoAnswer;

    // ① 先看括号分组里有几个数：4 个是「框」，2~3 个是「点」。
    //    这一步是**空框闸**的落点 —— 见头文件注释里的 `[0,0,0,0]` 实测。
    for (const wchar_t openCh : {L'[', L'('}) {
        const wchar_t closeCh = (openCh == L'[') ? L']' : L')';
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != openCh || i + 1 >= s.size()) continue;
            size_t j = i + 1;
            while (j < s.size() && s[j] != closeCh) ++j;
            if (j >= s.size()) continue;
            int vals[4] = {};
            int n = 0;
            size_t p = i + 1;
            while (p < j && n < 4) {
                double v = 0;
                if (!ReadNumberAt(s, p, &v)) break;
                vals[n++] = RoundToInt(v);
            }
            if (n >= 4) {
                const int x1 = (std::min)(vals[0], vals[2]);
                const int y1 = (std::min)(vals[1], vals[3]);
                const int x2 = (std::max)(vals[0], vals[2]);
                const int y2 = (std::max)(vals[1], vals[3]);
                if (x2 <= x1 || y2 <= y1) return AiVisionAnswerKind::NoAnswer;  // 空框 = 没回答
                outX1 = x1; outY1 = y1; outX2 = x2; outY2 = y2;
                return AiVisionAnswerKind::Box;
            }
            if (n == 2 || n == 3) {
                // 只在这个分组内部取点：保证「数出来的」和「解析出的」是同一组数字。
                if (!TryParseCoordinatePair(s.substr(i, j - i + 1), outX1, outY1)) break;
                outX2 = outX1;
                outY2 = outY1;
                return AiVisionAnswerKind::Point;
            }
            break;  // 分组里只有 1 个数：不是位置
        }
    }

    // ② 没有括号分组：沿用既有的「全文前 4 个数」兜底，再退到 2 个数。
    int bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;
    if (TryParseBoundingBox(s, bx1, by1, bx2, by2)) {
        outX1 = bx1; outY1 = by1; outX2 = bx2; outY2 = by2;
        return AiVisionAnswerKind::Box;
    }
    if (TryParseCoordinatePair(s, outX1, outY1)) {
        outX2 = outX1;
        outY2 = outY1;
        return AiVisionAnswerKind::Point;
    }
    return AiVisionAnswerKind::NoAnswer;
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
            || in.find(L"栏") != std::wstring::npos
            || in.find(L"第一个") != std::wstring::npos
            || in.find(L"第1个") != std::wstring::npos
            || in.find(L"第一张") != std::wstring::npos
            || in.find(L"缩略图") != std::wstring::npos
            || in.find(L"封面") != std::wstring::npos;
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

void MapScreenPointToApi(const AiCaptureMapping& map, int screenX, int screenY, int& apiX, int& apiY) {
    const int regionW = std::max(1, map.capX2 - map.capX1);
    const int regionH = std::max(1, map.capY2 - map.capY1);
    const int apiW = std::max(1, map.apiWidth > 0 ? map.apiWidth : map.srcWidth);
    const int apiH = std::max(1, map.apiHeight > 0 ? map.apiHeight : map.srcHeight);
    // 与 MapApiPointToScreen 同一套比例（互为逆运算）：api = (screen - cap) * api尺寸 / 区域尺寸。
    // 钳到图内：屏幕点可能落在捕获区域之外（窗口挪了/多屏），钳出来的是"最近边界"，
    // 调用方拿到的是「这张图里它在哪」，钳过的值不会指向图外的幻影坐标。
    const int rawX = static_cast<int>(
        static_cast<double>(screenX - map.capX1) * apiW / regionW);
    const int rawY = static_cast<int>(
        static_cast<double>(screenY - map.capY1) * apiH / regionH);
    apiX = std::clamp(rawX, 0, apiW - 1);
    apiY = std::clamp(rawY, 0, apiH - 1);
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

/// ★★两种读法**在本帧无法区分**时的说明（docs §61）。
///
/// 事情本身：prompt 要的是 `0~1000` 归一化，而整图帧恰好是 **960×540** ⇒
/// 模型若违规回答**像素**（`672,72` 这种），判据「四个数都 ≤1000」**同样成立**。
/// 两套读法都合法，而解析器只能选一套 —— 这不是 bug，是**信息不足**。
///
/// 纪律：**不许挑一种当成结论**（那只是把猜换个方向，而假阳性代价是假阴性的 4 倍），
/// 也不许沉默（沉默等于假装没有歧义）。做法是**如实留痕**：
/// 把另一种读法的数值一起写进诊断，让真实日志去回答「模型到底按哪套回答」——
/// 先测量，再决定要不要动判据。
std::wstring DescribeVisionCoordAmbiguity(int o1, int o2, int o3, int o4,
    int apiW, int apiH) {
    // 「按像素读」要成立，这四个数得落进图内（含 ±2 容差，与 VisionCoordsFitImage 同尺）
    if (!(o1 >= -2 && o2 >= -2 && o3 < apiW + 2 && o4 < apiH + 2)) return std::wstring();
    return L" ⚠本帧 " + std::to_wstring(apiW) + L"×" + std::to_wstring(apiH)
        + L" ≤1000 ⇒ 与「按像素读」**无法区分**（那种读法会得到 "
        + std::to_wstring(o1) + L"," + std::to_wstring(o2) + L","
        + std::to_wstring(o3) + L"," + std::to_wstring(o4) + L"）";
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
        const int ox = x, oy = y;   // 原始数：另一种读法（像素）要照原样报出来
        x = static_cast<int>(static_cast<long long>(x) * apiW / 1000);
        y = static_cast<int>(static_cast<long long>(y) * apiH / 1000);
        x = std::clamp(x, 0, apiW - 1);
        y = std::clamp(y, 0, apiH - 1);
        // ⚠ 两套读法都成立时**如实留痕**（见 DescribeVisionCoordAmbiguity）
        if (outNote) *outNote = L"0~1000归一化"
            + DescribeVisionCoordAmbiguity(ox, oy, ox, oy, apiW, apiH);
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
        const int o1 = x1, o2 = y1, o3 = x2, o4 = y2;   // 原始数（像素读法要照原样报）
        ScaleVisionRect(x1, y1, x2, y2, 1000, 1000, apiW, apiH);
        x1 = std::clamp(x1, 0, apiW - 1);
        y1 = std::clamp(y1, 0, apiH - 1);
        x2 = std::clamp(x2, 0, apiW - 1);
        y2 = std::clamp(y2, 0, apiH - 1);
        if (x2 <= x1 || y2 <= y1) return false;
        // ⚠ 两套读法都成立时**如实留痕**（见 DescribeVisionCoordAmbiguity）
        if (outNote) *outNote = L"0~1000归一化"
            + DescribeVisionCoordAmbiguity(o1, o2, o3, o4, apiW, apiH);
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
    const int capH = (std::max)(1, in.captureH);
    const double areaRatio = static_cast<double>(in.boxW) * static_cast<double>(in.boxH)
        / (static_cast<double>(capW) * static_cast<double>(capH));
    // 宽控件（工具栏宽按钮/输入框）：中心通常可点 → 可省一轮 Zoom。
    // 但必须**钳住宽度上界**：菜单行/列表行也「宽」，而模型给的框常比真行更宽
    // （实测 533×40 的「历史记录」菜单框直接点中心 → 点开了「设置」）。
    // 真正很扁很长的地址栏/公式栏由 thinWideBar 单独放行，不受这里收紧影响。
    const bool wideControl = in.boxH >= 14 && in.boxH <= 56
        && in.boxW >= in.boxH * 2 && in.boxW <= 420
        && areaRatio <= 0.02;
    const double aspect = static_cast<double>(in.boxW) / static_cast<double>((std::max)(1, in.boxH));
    const double areaMax = in.compactAreaRatioMax > 0.0 ? in.compactAreaRatioMax : 0.03;
    const double aspectMin = in.compactAspectMin > 0.0 ? in.compactAspectMin : 0.35;
    const double aspectMax = in.compactAspectMax > aspectMin ? in.compactAspectMax : 3.0;
    const bool areaOk = areaRatio <= areaMax;
    const bool aspectOk = aspect >= aspectMin && aspect <= aspectMax;
    // 文件名框/地址栏：很扁很长，高度常 <24；再 Zoom 易裁掉目标 → NOT_FOUND
    const bool thinWideBar = in.boxH >= 12 && in.boxH < 28
        && in.boxW >= 120 && aspect >= 4.0 && areaRatio <= 0.08;

    // ★「小标签 / 小卡片」放行（用户实测「选个卡牌、点个按钮都要十几秒」）：
    // 文字按钮常带副标题（「正常模式（要过关点这个）」实测粗框 245×93），
    // 它既过不了 compactBox（宽 > 观察区 8%）也过不了 wideControl（高 > 56），
    // 于是白烧一轮 15s 的二级 Zoom —— 而那一轮还答错了（漂移被拒，最后仍用一级点）。
    // 判据改成看**相对面积**：相对整屏够小（≤1%）且两边都不夸张 → 中心就是可点处。
    // 宽度上界（≤观察区 1/6）必须保留：实测 533×40 的「历史记录」菜单行粗框
    // 直接点中心会点开「设置」——那是「整行比真行更宽」，不是小标签。
    // 下界同理保留（≥32×28 且有边 ≥56）：任务栏邻近的小图标不能因为「面积小」就跳过精炼。
    const bool smallLabel = areaRatio > 0.0 && areaRatio <= 0.01
        && in.boxW >= 32 && in.boxW <= (std::max)(24, capW / 6)
        && in.boxH >= 28 && in.boxH <= (std::max)(24, capH / 8)
        && (std::max)(in.boxW, in.boxH) >= 56
        && aspect >= 0.3 && aspect <= 6.0;

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
        if ((compactBox || wideControl || smallLabel) && in.boxH >= 18
            && (compactBox ? in.boxW >= 64 : true)) {
            if (outReason) {
                if (smallLabel && !compactBox && !wideControl) {
                    *outReason = CoarseLocateSkipReason::SmallLabel;
                } else {
                    *outReason = (wideControl && !compactBox)
                        ? CoarseLocateSkipReason::WideControlRemapped
                        : CoarseLocateSkipReason::CompactRemapped;
                }
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
    if (in.adaptiveRefineDepth
        && ((compactBox && areaOk && aspectOk && in.boxW >= 32 && in.boxH >= 28)
            || smallLabel)) {
        if (outReason) {
            *outReason = (smallLabel && !(compactBox && areaOk && aspectOk))
                ? CoarseLocateSkipReason::SmallLabel
                : CoarseLocateSkipReason::CompactPixel;
        }
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
★target 写法决定命中率：最准是 2~10 字、屏幕上真实存在的文字/控件名（「历史记录」「保存」「更多」）。
方位/颜色/形状只作最短限定（「右上角更多」）；堆成长句（「历史记录面板右上角的三个点更多选项按钮」）
会让 VLM 被带偏、UIA 名称匹配变歧义——**长描述不是更准，是更不准**。
双击打开图标 doubleClick=true；右键 button=right。NOT_FOUND→可换更短的描述 / 滚动，不建议盲按键。
换个说法反复点同一处通常是空转：建议先 observePage / 看截图确认状态，
再考虑 clickRef / invokeUiControl / runCommand。
⚠ 引擎**不做**任何「第 N 次就换路线」的限制（批 A 已撤销那些闸）：定位每次都真的执行、
每次都真识图 —— 所以「再试一次会不会被拦」不是要考虑的事，值不值得重试才是。
findImage/findColor 有模板时优先。多步见 section=agent。
)";
}

std::wstring MacroActionUsageSkill() {
    return LR"(【场景用法 — lookupMacroAction section=usage】
多步任务见 section=agent。动手前先 updateTaskMemo(section=goal|todos)。

── 路线选择（先想这条，再想怎么点）──
★不需要「看见界面」的活一律用 runCommand(powershell/cmd) 一步做完，别用点击/输入逐格模拟：
  写文件（Excel/CSV/JSON/文本）、批量改名/整理、统计与转码、查/写注册表、算数、curl 取数据。
  例：桌面建表 → resolveSystemPath("desktop") 拿路径 → runCommand 写 CSV/xlsx → 完。
  实测同一任务走 GUI 要 17 轮 + 约 99 个合成按键；走命令 2~3 轮。
★必须看界面/只有 GUI 入口时才走下面的定位点击：浏览器网页 → 扩展 DOM；
  浏览器外壳与桌面软件 → UIA（listUiControls → invokeUiControl）；自绘/游戏 → locateAndClick 识图。
★命令要读结果就自己重定向到文件，再 readAgentFile 读；runCommand 不回显 stdout。

── 打开 / 导航 ──
★查资料（玩法/用法/报错/名词）＝ **webSearch(query) → fetchWebPage(选中的 URL)**：
  两步都是宿主 HTTP，**不开浏览器、不抢前台**；webSearch 还能 readTop=1~3 同一轮把正文读回来。
  ⚠ 中文查询别用空格堆多个词（实测会把结果带偏），把关键短语连写。
  openWebpage + observePage 只用于**需要点击/登录/翻页**的交互网页（⚠ 它会抢走当前前台，
  用完记得 activateWindow 切回来）——一次性读资料不要走这条路。
★★「统计浏览记录/书签、最近访问了什么」= **readBrowserHistory**（1 轮；**完整**标题+URL+精确时间）。
  ⚠⚠ **别去 edge://history 观察/截图抄**：内置页**禁扩展注入** ⇒ observePage **必失败**；列表截图**必被省略号截断**
  （zoom 只是放大省略号）⇒ 抄必错（实测那条路 8+ 轮、23.6 秒）。返回已完整，**别再 zoom 核对**。
openWebpage → 只开给人看的 https 首页（禁 api.*/JSON）。同站搜人用 searchOnPage(query)。白屏只 wait。
网页：优先 searchOnPage(query) 后 clickRef（点树上那个名字最贴近目标的项；点元素不跟推荐 href 偷跳）。树上没有或未装扩展则 locateAndClick。开关看 checked；小范围已变勿再点（会取消）。扩展导航会固化为打开网页/找图（逻辑转化可回放）。登录框/表单填写用 typeByLabel(label)：一次调用即可，不必先 observePage。canvas 用 locateAndClick。
不确定某界面的快捷键/操作时：webSearch 找官方文档 → fetchWebPage 读正文（只禁**站点内部 API**，搜索页能抓）。

── 窗口 / 启动 ──
listWindows → activateWindow(match=标题关键词) 复用已开窗口。
桌面控件优先 listUiControls 看编号台账 → invokeUiControl(name) 精确触发（不烧识图）；枚举不到再用 locateAndClick。
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
★先想一遍：把数据写进表格/文件本来就能用 runCommand 一条命令做完（快几十倍、不错行，
  用法见 section=command）；只有必须看着界面填（改既有表格的特定单元格等）才用配方。
验证 ≥2 组完整循环（含组间转移）后 runActionRecipe(steps, rows) 批量。
模板只放每行重复步骤，勿含表头固定文本。
★模板是**每组执行一遍**的，先想清「组间怎么前进」：
  · 逐行追加数据 → 模板里只用 Enter(下行)+Home(回 A 列)，起点 Ctrl+Home 单独发一次；
  · 每组都回到同一区域覆盖填写（同一张表反复填新值）→ 模板里放 Ctrl+Home 才是对的。
  拿不准就看截图确认表头有没有被覆盖（宿主会在模板含 Ctrl+Home 时提醒这一点，不拦你）。
★已经写入若干行后：不要再 Ctrl+A + Delete 清表重来（会丢掉已写内容，宿主会要 confirmShortcut）；
  换模板/重跑前先看截图确认光标在哪一行。

── 定位 / 点击 ──
locateAndClick(短标签)：最准 2~10 字，写屏幕上的文字/控件名；方位只作最短限定，
别堆成长句（长描述让 VLM 跑偏、UIA 匹配歧义）。表格起点 Ctrl+Home。未找到可换更短的描述再试。
★换个说法反复定位同一目标通常是空转：建议先 observePage / 看截图确认状态，
再考虑 clickRef / invokeUiControl / runCommand。
⚠ 引擎不限制定位次数、也不因为「同一目标重复」而拦你（批 A 已撤销那些闸）——
不要为「会不会被拦」而放弃一条本来走得通的路。
灰按钮=前置未满足，改输入别连点。目标不在当前树/可视区才 scrollWheel；对话框侧栏用 mouseDrag。
★点击输入框成功后：下一步直接 quickInput 输入，不要先滚动/切窗/按 Tab——
  滚轮常作用在鼠标所在窗口而非目标窗口，输入框也不需要滚动就能输入。

── 回复 / 发送 ──
quickInput 后 keyClick(Enter) 或 locateAndClick(发送按钮)。

工具：runCommand、**webSearch、fetchWebPage**、openWebpage、searchOnPage、observePage、clickRef、typeByLabel、typeRef、listUiControls、invokeUiControl、runProgram、openFile、scrollWheel、mouseDrag、quickInput、keyClick、
locateAndClick、resolveSystemPath、activateWindow、runActionRecipe、saveTaskData、
readTaskData、switchIme、completeTask、readDocument、**readBrowserHistory**；少用 mouseClick/aiActionExecute。
)";
}

std::wstring MacroActionAgentSkill() {
    return LR"(【Agent — section=agent】
看清→同轮多工具→验收→completeTask。

· updateTaskMemo(goal|todos)
· 查资料先 webSearch（不开浏览器、不抢前台；中文别用空格堆词）→ fetchWebPage 读选中的 URL
· 交互网页（要点击/登录/翻页）才用 searchOnPage/observePage/clickRef/typeByLabel/typeRef（⚠ 抢前台）；点树上名字最贴近目标的项；树上没有或未装扩展则 locateAndClick；登录框/表单直接 typeByLabel(label)；开关小范围已变勿再点（会取消）；屏上已有则点，勿先滚；勿拼搜索框/Enter
· 先选路线：文件/数据/批量类用 runCommand(powershell) 一步做完；只有必须看界面才定位点击
· 办公文件（xlsx/docx/pptx/pdf/csv）：先 readDocument 直接读；写用 runCommand 的 COM/CSV 配方，别开软件逐格输入
· 浏览器历史/书签：readBrowserHistory（1 轮拿完整标题+URL+时间）；**别开 edge://history 截图抄**
· 桌面控件：listUiControls 看编号台账 → invokeUiControl(name) 精确触发；枚举不到（游戏/自绘）才 locateAndClick
  ★列表型内容（历史/书签/文件/消息）优先用它，可加 typeFilter/nameFilter；**别 zoom 读截图**（标题被省略号截断，UIA 名字才完整）
· 桌面框 quickInput(clearFirst)；scrollWheel/mouseDrag；滚后没变=滚错窗
· 切窗 activateWindow；启动 runProgram/openAppViaSearch；路径 resolveSystemPath
· 对话先确认截图有输入框；没有先 listWindows
· 另存为：路径写入文件名框，禁空转滚轮
· 对话框看图；勿盲猜快捷键；灰钮改前置
· 未知键：fetchWebPage 查官方文档，禁编造
· 表格：优先 runCommand 一次做完（见 section=command）；必须点界面才 Ctrl+Home→runActionRecipe。
  配方模板**每组执行一遍** ⇒ 想清「组间怎么前进」：逐行追加用 Enter(下行)+Home(回 A 列)，起点 Ctrl+Home 单独发。
  已写入若干行后不要 Ctrl+A 清表重来（会丢已写内容）；失败禁连 Tab
· 宿主已 settle，勿空 wait
)";
}

std::wstring MacroActionCommandSkill() {
    // 产品 Skill 文件：skills/agent/command.md（随包分发）。改这里必须同步该文件。
    return LR"(【命令行路线 — section=command】
★先判断路线：结果落在文件/数据里 → runCommand 一条命令做完；结果落在界面状态里 → 定位点击。
runCommand(shell=powershell|cmd, command=..., waitMs=1500, hidden=true)
· 用途：写 Excel/CSV/JSON/文本、批量改名整理、统计转码、算数、读写注册表、HTTP 取数
· 办公文件（xlsx/docx/pptx/pdf）的读写配方见 section=office；**读**优先用 readDocument 工具
· 路径必须绝对：桌面先 resolveSystemPath(folder="desktop")；中文路径用单引号包住
· 不回显 stdout：要结果就命令里 Out-File 到文件，再 readAgentFile/readTaskData 读
· 命令含换行/双引号务必转义（\n、\"）；能写单行就写单行
· 落到既有「运行程序」动作（runProgram + inputText 参数），可在编辑器改、可回放
· 已装 Office 才用 COM 写 xlsx；否则写 CSV（Excel 直接能开）更快更稳
· 不要用它提权/删用户数据/下载执行未知程序；这类先 completeTask 说明需要用户确认
· 打开浏览器内置页（历史/下载/设置）用 openWebpage("edge://history")，宿主会让浏览器
  自己带 URL 打开（不用合成键，避免 URL 被打进搜索栏）
· 打开 GUI 程序等界面用 runProgram + 观察，不要用 runCommand 等窗口
)";
}

std::wstring MacroActionOfficeSkill() {
    // 产品 Skill 文件：skills/agent/office.md（随包分发）。改这里必须同步该文件。
    return LR"(【办公文档 — section=office】
★口诀：数据来源和去处都是**文件** → readDocument/runCommand；来源在**屏幕**上 → 视觉读一次
（saveTaskData 存下）后全部走命令行。不要为了读内容去 openFile+截图+识图抄。

读：readDocument(path, maxChars=6000, pages=3)
· 支持 xlsx/xlsm/xls/xlsb、csv/tsv、docx/doc、pptx/ppt、pdf、txt/md/json/log/xml
· 表格返回「制表符分隔、一行一记录」（首行常是表头）；文档返回段落文本
· 引擎：excel-com/word-com（装了 Office，值最准）/ **ooxml-native**（我们自己的引擎直读 zip+XML：
  **不起外部进程、不需要 Office**，xlsx 默认走它）/ ooxml（老的脚本回退路线）/
  pdftotext / pdf-render（PDF 无文本层→渲染成图片发给你看）/ text
· ★日期已能正确读：Excel 存的是序列号（2024-01-01=45292），原生引擎按数字格式还原成
  `2024-01-01` / `12:00:00` / `2024-01-01 18:00:00`（ISO）。
  ⚠ 百分比等其它格式给**原始数值**（0.12 就是 12%）—— 那是真值，算数用它对。
  ⚠ 公式：有缓存值给值，没有就写 `=公式原文`（告诉你这里有公式，而不是给你空格子）。
· 读不到内容：openFile 后用视觉读，或先 runCommand 转文本；别死磕

★★浏览器历史/书签：**readBrowserHistory(limit)**（kind=history|bookmarks|browsers）。1 轮拿**完整**标题+URL+精确时间，制表符分隔直接进表。
  ⚠ 别开 edge://history 观察/截图抄：内置页**禁扩展注入**（observePage 必失败），列表截图**必被省略号截断**。

写：
★首选 **writeSpreadsheet**（**不需要装 Office**，我们自己的引擎直接读写 OOXML）：
· 新建：mode="create" + tsv（制表符分隔，首行常是表头）。数字自动识别，
  但**带前导 0 的（007）保持文本**。⚠ 目标已存在时**默认拒绝覆盖**（要覆盖传 overwrite=true）。
· 改已有文件：mode="setCells" + cells[{ref,text|number|bool|formula,cached?}]
  —— **只动你点名的格子**，文件里的图表/公式/条件格式/样式/别的表**一字不动**
  （这正是 COM「另存一份」做不到的）。⚠ 改前会自动备份，返回里给备份路径。
· 公式**不会被计算**：`cached` 是直读用的缓存值，不传就等 Excel 打开时重算（直读拿不到值）。
· 写完会把该表读回来给你核对。
· 不支持老格式 .xls；需要 Excel 的**格式化显示值**（日期显示成 2024-01-01、千分位）时才走 COM。

写（runCommand，路径先 resolveSystemPath）：
· xlsx（装 Office）：$xl=New-Object -ComObject Excel.Application; $xl.Visible=$false;
  $xl.DisplayAlerts=$false; $wb=$xl.Workbooks.Add(); $ws=$wb.Worksheets.Item(1);
  $ws.Cells.Item(1,1)='序号'; …; $wb.SaveAs($p,51); $wb.Close($false); $xl.Quit()
· CSV（无 Office 也行）：@('序号,标题','1,值') | Out-File -Encoding utf8 $p  ← 必须 utf8
· Word：Documents.Add() → Content.Text → SaveAs($docx)；要 PDF 再
  $d.ExportAsFixedFormat($pdf, 17)
· PPT：Presentations.Add($false) → Slides.Add(1,12) → Shapes.AddTextbox(...).TextFrame… → SaveAs
· 已有 xlsx 转 PDF：$wb.ExportAsFixedFormat(0, $pdf)

硬约定：
· 中文 CSV 必须写 **BOM**：Excel 对无 BOM 的 UTF-8 会显示乱码（实测 城市→鍩庡競）；
  稳妥写法 [System.IO.File]::WriteAllText($p,$t,(New-Object System.Text.UTF8Encoding($true)))
· Export-Csv 不写 -Encoding UTF8 会输出 ASCII → 中文变 ??（不可逆）
· COM 必须 $wb.Close($false) + $xl.Quit() + ReleaseComObject，否则留隐藏 EXCEL.EXE 占住文件
· Word/Excel COM 可能挂住（实测 >2 分钟且无对话框）：宿主 readDocument 有 60s 超时；
  不要用 Word 打开 PDF 转换（会挂），要 PDF 用 ExportAsFixedFormat(路径,17)
· 文件被 Excel 独占打开时读不了（~$ 锁文件）：先关闭 / 复制一份 / GetActiveObject 附着已开实例
· 程序生成的 xlsx 公式没有缓存值，直读为空：写计算好的值，或用 COM $ws.Calculate()
· XML 查询必须带命名空间（GetElementsByTagName('sldId') 对 p:sldId 返回 0 且不报错）
· Word 会把一句拆成多个 <w:r>：读文本要按段落合并 w:t
· 老格式 .xls/.doc/.ppt 只能 COM；.pptx 不能手搓最小 OOXML（真实 pptx 44 个 part，要模板/COM）
· 读的引擎与局限：pdf-ifilter（Windows 自带，无版式/扫描件抽不到字）、pdf-render（渲染成图给你看）、
  ooxml（快、无 Office 依赖、公式只有缓存值）、excel-com/word-com（值最准）
· 全部落到既有「运行程序」动作（runProgram + inputText），可回放、可在编辑器改

★关于第三方办公引擎（**我们不依赖**，详见 skills/agent/office.md §2.5）：
· 这里原来有一节「GenOffice 路线」（推荐优先用它的 CLI/MCP 工具读写 xlsx/docx/pptx），**已删除**。
  理由：本产品**不依赖任何别人的软件运行** —— 把「写 Office 文件」外包给第三方 CLI，
  对方没装就能力缺失、版本不同就行为不同、它的子命令清单还得跟着对方发版维护。
· 所以：**不要**去探测、启动或推荐任何第三方办公软件，也**不要**指望 runAgentCommand 放行它们
  （白名单里没有任何第三方程序特例）。
· 值得借鉴的是**做法与思路**（与具体项目无关，我们自己在 src/ooxml/ 里实现）：
  ① 工具层不调模型（文档读写是确定性的，模型只负责编排）；
  ② **字节保留式编辑**（只重写改动的那部分，其余原样搬运 ⇒ 公式/图表/透视表/样式原样存活，
     这正好补上上面「COM 另存会丢」那条的缺口）；
  ③ 产物可验收（读回单元格做**确定性**验证；必要时渲染成图给你看）。
· 自研层可用之前，写文件就走上面的 COM / CSV 配方。
· 若用户**自己**配了外部 MCP server（mcp_servers.json），工具表里会出现 mcp__<server>__* ——
  那是用户显式配置的、厂商中立的扩展点，可以用；但别去猜/推荐具体装哪个软件。
)";
}

std::wstring MacroActionGameSkill() {
    // 产品 Skill 文件：skills/agent/game.md（随包分发）。改这里必须同步该文件。
    return LR"(【实时游戏 / 动态画面 — section=game】
★核心结论（有实测出处，见 docs/realtime-game-vision-loop-research.md）：
  没有哪个「VLM 每步都问」的方案能实时跑游戏（grounding 0.7~6.9s，游戏回路 ~15Hz）。
  正确姿势 = ①VLM 找一次 → ②之后用**本地**手段跟住 → ③本地判失败才再叫 VLM。

★建议先搞懂机制再动手（抽象目标时最省轮次的一步）：「通关这关 / 打过这一波」这类目标，建议先用一轮把
  ①胜负条件 ②资源规则（**冷却/费用是全局共享还是每个单位各自一份**）③操作机制（先选再点目标？
  能一次放几个？）④节奏（多久一波）搞清楚：lookupMacroAction(section=game) + 看画面 +
  需要时用 **webSearch**（零 API key、宿主直连、**不开浏览器不抢前台**）搜「游戏名+模式」，
  再用 fetchWebPage 读选中那条的正文，结论写进 updateTaskMemo。
  ⚠ 实测：中文查询**别用空格堆多个词**（「植物大战僵尸融合版 我是僵尸 攻略」被带偏成「植物」；
  「植物大战僵尸融合版」第一条就是对的中文 Wiki）⇒ 关键短语连写，多概念分几次搜。
  ⚠ 别用 runCommand 开浏览器搜（它**不回显输出**，结果读不到）；也别为此开 openWebpage
  ——那会抢走游戏前台（实测还额外花 3 轮切来切去，拿到的是别的游戏的摘要）。
  操作明确（点这个按钮/按这个键）就直接做，不必为省事而查。
  真实踩坑：PvZ「我是僵尸」里 AI 以为冷却只能放一个，于是一次只放一个僵尸送死；
  实际冷却按卡牌各自算、阳光共享 → 批量/连续放多个更容易形成波次。

★不确定就「用一次动作问一问」，**别在脑子里反复推**（实测：有一局为「我是僵尸怎么算赢」
  和「阳光是 3000 还是 30000」推了 8 轮、写了几千字，一个动作都没落）：
  ① 每轮引擎都会**如实回执**「这一击之后界面有没有变化」—— 那是唯一可靠的现实校验，比推演硬；
  ② 规则/按钮/数值不确定 ⇒ 用一个**成本最低的动作**去问（点一次那张卡、点一次空地、
     按一次快捷键、screenshot 看一帧），然后**看回执**，一轮就有答案；
  ③ ⚠ 自查「是不是在绕圈」：如果这一轮的疑问和上一轮**几乎一样**，立刻改用一次动作验证，
     或换个信息来源（fetchWebPage 查玩法 / screenshot 看清画面）；
  ④ 已确认的结论写进 updateTaskMemo 标「已确认」，假设标「待验证」再去验一次；
  ⑤ 读不清卡槽/价格/计数器时 **screenshot 比推演便宜**；小字看不清就 `zoom` 那一块
     （整帧是降采样过的，卡片角标/价格只剩几十像素，靠它读数字必然读错），见下面的 ★zoom；
  ⑥ 点一次没反应（回执说界面没变）就是答案：那东西很可能不可点，换目标，**别再点第二次**；
  ⑦ 回执说「**无法确认**这一击是否生效」时**别为它单开一轮**：动态画面每帧都在重绘，
     本地测不出「生效没有」是正常的（既不是成功也不是失败）；动作执行**每轮本来就回传
     观察帧**，直接看下一帧或继续推进，不要为了验收去补 screenshot；
  ⑧ 索引里标「文字」的条目只表示「屏幕上有这几个字」（模式标签/标题/计数器都会出现），
     **不保证可点**。

★zoom（放大镜，看不清时用它，别拿 1024 宽的整帧猜）：
  · 坐标直接抄**索引里给的那一对**（upload 截图像素 = 元素索引/文字索引/mouseClick 同一套，
    零换算）；也可 zoom(target="屏幕上的短标签") —— 它查**同一张元素索引**，
    ⇒「放大的地方」与 locateAndClick「会点的地方」是同一处。
  · 回执写清**区域、倍率 ×N、图内坐标 → upload 的公式**（upload_x = x1 + 图x/N）：
    要点击就把算出来的 upload 值交给 mouseClick/locateAndClick，**别把图内像素直接当坐标**。
  · 一次放大一块；同一轮可调多次（每张都是独立的图，回执里的 [附图 N] 与图前编号一一对应）。
  · 画面刚变过（刚点完/刚过关）旧坐标就过期：先重新 screenshot 再 zoom。

★省轮次的两条手法：
  1) 两步操作一次做完：locateAndClick(targets=["拿起的卡","要放的位置"])
     —— 一次调用里逐个识图定位并**立即连点**，中间不插观察/验收（省一整轮主模型
     10~40s 思考 + 每次 1.5~2.6s 的界面稳定等待）。游戏画面一直在动，逐步确认价值不大。
  2) 批量推进：多个单位同时进攻就把目标一次列进 targets（≤6），或连续多条
     locateAndClick(targets=[...])；开局选卡一次选够再进游戏。

★省识图成本最有效的办法：**把同一批的多个落点一次做完**。
  · 屏幕上**有文字标签**的位置（自选僵尸卡牌 / 正常模式 / 主菜单 / 暂停 / 确定）：**直接点**。
    locateAndClick(target="自选僵尸卡牌") 会用宿主本地 OCR 文字索引直接命中坐标
    （0 次识图，日志打「文字直点」）。不必为它反复识图确认。
  · 反复点的位置（草坪格子 / 卡槽 / 道具栏）：引擎**不**记「上次在哪」（跨帧坐标不作数，
    宁可慢也不拿旧坐标）⇒ 每次都真识图 —— 所以把这一批要点的多个位置一次列进
    locateAndClick(targets=[…])，或一次 submitMacroActions 提交整批。

本地手段优先级：
1) 颜色：findColor/getColor/colorMatch 判血条/蓝条/技能亮灭/小地图点位（毫秒级，最稳）
2) 找图：findImage（多尺度+唯一性+像素终审）。模板需要**有纹理**：纯色模板会被归一化
   相关算成「处处满分」，宿主已直接拒绝并给出原因 → 换 findColor 或换有纹理的区域
3) 循环反复点同一目标：每次都**真识图**（宁可慢，不许拿旧坐标）；
   能合并就把多个落点一次列进 targets，比一个一个试省得多
4) UIA 对 DirectX/自绘游戏无效

locateAndClick：target 短（2~10 字）且写屏幕上真实存在的东西；小/糊目标传 refineLevels=2
（宿主会把分辨率花在目标附近的小区域，比整屏缩小识别率高）。同一目标重复识图不划算。

输入手法：持续按住用 keyDown/keyUp（比 keyClick 连点可靠）；转视角用 moveMouseRelative
（注意系统指针加速会缩放相对位移，精确点 HUD 要用找图/颜色拿到屏幕坐标再 mouseClick）；
节奏用动作的 duration（=重复间隔，不是执行前等待），用 wait 凑帧不划算（Sleep 被量化到 ~15ms）。
连续多步可用 submitMacroActions 一次提交（等价 speculative multi-action，省大量 API 轮次），
但每步要确定（时间/按键/坐标已知），不能把「还没定位的东西」写进去。

⚠ **引擎在这些地方都不设限、也不拦你**（批 A/B/C 已撤销那些闸）：同坐标可以再点、
同一目标可以再定位、工具表永远给全 —— 判断「值不值得」是你的活，不是引擎的活。
常见误区（多半不划算）：同坐标反复连点；换个说法反复定位同一目标；
猜绝对坐标点没定位过的东西；用 wait 拼节奏 / keyClick 实现长按；同一步骤失败 2 次还原地重试
（可换目标描述/换行/换单位）；在带反作弊的在线游戏里跑自动化（SendInput 必带 LLMHF_INJECTED
标记，无法清除）——遇到这类可先 completeTask 说明风险。
)";
}

std::wstring BuildAiActionToolExecuteSystemPrompt(
    int captureWidth, int captureHeight, bool withImage) {
    // 注意：★路线规则**不写在这里**。系统提示词每一轮都要付费，而「该用命令行还是点界面」
    // 只在一部分任务里才相关。它放在 Skill（lookupMacroAction(section=command)）里，
    // 由工具函数在**相关时机**（要用配方逐格填表 / 打开表格软件 / 在表格里输入）
    // 把指针塞进工具结果——按需付费，且不影响其它任务。
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
    // 坐标契约对齐 Midscene/OS-Atlas：一律 0~1000 归一化，并明确禁止像素坐标——
    // 混用两种口径是「点了隔壁」最常见的系统性来源（4K/缩图尤其明显）。
    // 多候选：一次调用给出最多 3 个候选，宿主做空间聚类取簇心（研究结论：聚类远优于
    // 单点/平均——平均甚至比随机还差），互相矛盾时才会多花一轮 Zoom 精炼。
    s += L"输出 [x1,y1,x2,y2]（0~1000）。整图找；多个相似只框描述完全匹配的那一个；无则 NOT_FOUND。"
        L"坐标一律相对整图 0~1000 归一化，❌禁止输出像素坐标或图宽图高。只输出结果，勿解释。"
        L"若有多个可能位置：每行一个框，最多 3 行，最可能的放第一行。";
    return s;
}

std::wstring BuildCompositeRefinePointPrompt(const std::wstring& userTask, int levelIndex,
    int imageWidth, int imageHeight, bool prevPredictionMarked) {
    std::wstring p = Trim(userTask);
    if (p.empty()) p = L"目标";
    (void)levelIndex;
    std::wstring s = L"精点：" + p + L"\n";
    if (imageWidth > 0 && imageHeight > 0) {
        s += std::to_wstring(imageWidth) + L"x" + std::to_wstring(imageHeight) + L"\n";
    }
    if (prevPredictionMarked) {
        // 红叉 = 上一轮的落点（PrecisionCUA 的闭环形态）。★只描述这个**参考物**，
        // 然后**仍然要绝对坐标**：明确要求「回答偏移量」会把准确率打对折
        // （同一论文：Visual-Anchor 提示词让 GPT-5.4-Pro 从 41.0% 掉到 18.5%）。
        s += L"图中红色十字=上一轮预测的点（可能没对准目标）。据此校正落点："
            L"给出**目标本身**中心的 (x,y) 0~1000（绝对坐标）。"
            L"❌不要输出偏移量/方向描述（如「向左 30px」）。\n";
    }
    s += L"只输出 (x,y) 0~1000（相对本放大图归一化，❌禁止输出像素坐标）。"
        L"多个相似控件勿点邻居；无则 NOT_FOUND。";
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
        + L"]。重新框选真正目标，输出 [x1,y1,x2,y2] 0~1000（整图归一化，❌禁止输出像素坐标）；无则 NOT_FOUND。";
    return s;
}

std::wstring BuildAiActionVisionQuerySystemPrompt(int imageWidth, int imageHeight) {
    std::wstring s =
        L"根据截图直接回答。优先 [x1,y1,x2,y2]（0~1000，相对整图归一化；❌禁止输出像素坐标），"
        L"否则 (x,y) 0~1000。无多余解释。"
        L"第1个/最上=重复卡片网格最上最左，勿点底部推荐或页脚。";
    if (imageWidth > 0 && imageHeight > 0) {
        s += L" 图 " + std::to_wstring(imageWidth) + L"×"
            + std::to_wstring(imageHeight) + L"。";
    }
    return s;
}
